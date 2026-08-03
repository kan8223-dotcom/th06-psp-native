#include "GameWindow.hpp"
#include "AnmManager.hpp"
#include "BulletManager.hpp"
#include "EffectManager.hpp"
#include "EnemyManager.hpp"
#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "ScreenEffect.hpp"
#include "SoundPlayer.hpp"
#include "Stage.hpp"
#include "Supervisor.hpp"
#include "ZunMath.hpp"
#include "graphics/Software.hpp"
#ifdef TH06_PS3
#include <sys/systime.h>

GfxInterface *Th06CreateRsxBackend(); // th06_ps3
extern "C" void th06_breadcrumb(const char *msg); // ps3/eye.cpp

// Low-overhead bomb profiler.  The RSX backend samples these counters at
// present time and publishes only one summary after the bomb ends, so the
// diagnostic UDP traffic cannot become part of the slowdown being measured.
static u32 s_Th06PerfDrawUs;
static u32 s_Th06PerfCalcUs;
static u32 s_Th06PerfSoundUs;

static u64 Th06PerfNowUs()
{
    u64 sec, nsec;
    sysGetCurrentTime(&sec, &nsec);
    return sec * 1000000ull + nsec / 1000ull;
}

extern "C" void th06_perf_snapshot(u32 *drawUs, u32 *calcUs, u32 *soundUs, u32 *bombActive, u32 *bombKey,
                                    u32 *bombTimer, u32 *bullets, u32 *items, u32 *effects, u32 *flushes,
                                    u32 *stage, u32 *stageFrame, u32 *timelineFrame, u32 *sceneFlags,
                                    u32 *spellIdx, u32 *msgIdx)
{
    *drawUs = s_Th06PerfDrawUs;
    *calcUs = s_Th06PerfCalcUs;
    *soundUs = s_Th06PerfSoundUs;
    *bombActive = g_Player.bombInfo.isInUse ? 1u : 0u;
    *bombKey = (u32)g_GameManager.CharacterShotType();
    *bombTimer = g_Player.bombInfo.isInUse ? (u32)g_Player.bombInfo.timer.AsFrames() : 0u;
    *bullets = (u32)g_BulletManager.bulletCount;
    *items = g_ItemManager.itemCount;
    *effects = (u32)g_EffectManager.activeEffects;
    *flushes = g_AnmManager ? g_AnmManager->flushesThisFrame : 0u;

    // Scene flags used by the delayed frame-spike recorder:
    //  bit 0 stage active, 1 dialogue, 2 dialogue skippable, 3 enemy spell,
    //  4 player bomb, 5 demo, 6 replay, 7 game menu, 8 retry menu,
    //  9 generic menu, 10 boss present, 11 time stopped.
    const bool stageActive = g_Gui.impl != NULL;
    const bool dialogue = stageActive && g_Gui.HasCurrentMsgIdx();
    u32 flags = stageActive ? (1u << 0) : 0u;
    if (dialogue)
        flags |= 1u << 1;
    if (dialogue && g_Gui.IsDialogueSkippable())
        flags |= 1u << 2;
    if (stageActive && g_EnemyManager.spellcardInfo.isActive)
        flags |= 1u << 3;
    if (g_Player.bombInfo.isInUse)
        flags |= 1u << 4;
    if (g_GameManager.demoMode)
        flags |= 1u << 5;
    if (g_GameManager.isInReplay)
        flags |= 1u << 6;
    if (g_GameManager.isInGameMenu)
        flags |= 1u << 7;
    if (g_GameManager.isInRetryMenu)
        flags |= 1u << 8;
    if (g_GameManager.isInMenu)
        flags |= 1u << 9;
    if (stageActive && g_Gui.BossPresent())
        flags |= 1u << 10;
    if (g_GameManager.isTimeStopped)
        flags |= 1u << 11;

    *stage = stageActive ? (u32)g_GameManager.currentStage : 0u;
    *stageFrame = stageActive ? g_GameManager.gameFrames : 0u;
    *timelineFrame = stageActive ? (u32)g_EnemyManager.timelineTime.AsFrames() : 0u;
    *sceneFlags = flags;
    *spellIdx = (stageActive && g_EnemyManager.spellcardInfo.isActive)
                    ? g_EnemyManager.spellcardInfo.idx
                    : 0xffffffffu;
    *msgIdx = dialogue ? (u32)g_Gui.impl->msg.currentMsgIdx : 0xffffffffu;
}
#endif
#ifndef TH06_PS3 // th06_ps3: GL backends excluded on PS3 (RSX backend arrives later)
#include "graphics/FixedFunctionGL.hpp"
#include "graphics/WebGL.hpp"
#endif
#include "i18n.hpp"
#include "utils.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_timer.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

GameWindow g_GameWindow;
GfxInterface *g_GfxBackend;
i32 g_TickCountToEffectiveFramerate;
f64 g_LastFrameTime;

#define FRAME_TIME (1000. / 60.)

static const struct
{
    const char *name;
    GfxInterface *(*TryInit)();
} s_RenderBackends[] = {
#ifndef TH06_PS3 // th06_ps3: GL backends excluded on PS3
    {"GL 2.1 / GL ES 2.0 / WebGL", WebGL::Create},
    {"Fixed function GL(ES)", FixedFunctionGL::Init},
#elif TH06_USE_RSX
    {"RSX", Th06CreateRsxBackend}, // th06_ps3: hardware backend (ps3/RsxGfx.cpp) - RPCS3では凍結中(DECISIONS.md D-003)
#endif
#if !(defined(TH06_PS3) && TH06_USE_RSX && TH06_RSX_FORCE)
    {"Software fallback (VERY SLOW)", Software::Init}
#endif
};

#ifdef TH06_PS3
static void Th06BreadcrumbRenderer(const char *event, const char *name)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "%s %s", event, name);
    th06_breadcrumb(msg);
}
#endif

RenderResult GameWindow::Render()
{
    i32 res;
    f64 slowdown;
    ZunViewport viewport;
    f64 delta;
    u32 curtime;

    if (this->lastActiveAppValue == 0)
    {
        return RENDER_RESULT_KEEP_RUNNING;
    }

    if (this->curFrame == 0)
    {
    RUN_CHAINS:
#ifdef TH06_PS3
        const u64 perfDrawStartUs = Th06PerfNowUs();
#endif
        if (g_Supervisor.cfg.frameskipConfig <= this->curFrame)
        {
            if (g_Supervisor.RedrawWholeFrame())
            {
                viewport.x = 0;
                viewport.y = 0;
                viewport.width = GAME_WINDOW_WIDTH;
                viewport.height = GAME_WINDOW_HEIGHT;
                viewport.minZ = 0.0;
                viewport.maxZ = 1.0;
                viewport.Set();
                g_GfxBackend->SetClearColor(
                    ((g_Stage.skyFog.color >> 16) & 0xFF) / 255.0f, ((g_Stage.skyFog.color >> 8) & 0xFF) / 255.0f,
                    (g_Stage.skyFog.color & 0xFF) / 255.0f, (g_Stage.skyFog.color >> 24) / 255.0f);
                g_GfxBackend->Clear(CLEAR_COLOR_BUFFER | CLEAR_DEPTH_BUFFER);
                g_AnmManager->SetProjectionMode(PROJECTION_MODE_PERSPECTIVE);
                g_Supervisor.viewport.Set();
            }

            g_AnmManager->ClearVertexBuffer();
            g_AnmManager->flushesThisFrame = 0;
            g_Chain.RunDrawChain();
            g_AnmManager->SetCurrentTexture(0);
        }

        g_AnmManager->FlushVertexBuffer();
#ifdef TH06_PS3
        s_Th06PerfDrawUs = (u32)(Th06PerfNowUs() - perfDrawStartUs);
#endif
        g_Supervisor.viewport.x = 0;
        g_Supervisor.viewport.y = 0;
        g_Supervisor.viewport.width = GAME_WINDOW_WIDTH;
        g_Supervisor.viewport.height = GAME_WINDOW_HEIGHT;
        g_AnmManager->SetProjectionMode(PROJECTION_MODE_PERSPECTIVE);
        g_Supervisor.viewport.Set();
#ifdef TH06_PS3
        const u64 perfCalcStartUs = Th06PerfNowUs();
#endif
        res = g_Chain.RunCalcChain();
#ifdef TH06_PS3
        s_Th06PerfCalcUs = (u32)(Th06PerfNowUs() - perfCalcStartUs);
        const u64 perfSoundStartUs = Th06PerfNowUs();
#endif
        g_SoundPlayer.PlaySounds();
#ifdef TH06_PS3
        s_Th06PerfSoundUs = (u32)(Th06PerfNowUs() - perfSoundStartUs);
#endif

        if (res == 0)
        {
            return RENDER_RESULT_EXIT_SUCCESS;
        }
        if (res == -1)
        {
            return RENDER_RESULT_EXIT_ERROR;
        }
        this->curFrame++;
    }

    if (g_Supervisor.cfg.windowed || g_Supervisor.ShouldRunAt60Fps())
    {
        if (this->curFrame != 0)
        {
            g_Supervisor.framerateMultiplier = 1.0;
            slowdown = SDL_GetTicks();
            if (slowdown < g_LastFrameTime)
            {
                g_LastFrameTime = slowdown;
            }
            delta = std::fabs(slowdown - g_LastFrameTime);
            if (delta >= FRAME_TIME)
            {
                do
                {
                    g_LastFrameTime += FRAME_TIME;
                    delta -= FRAME_TIME;
                } while (delta >= FRAME_TIME);

                if (g_Supervisor.cfg.frameskipConfig < this->curFrame)
                    goto I_HAVE_NO_CLUE_WHY_BUT_I_MUST_JUMP_HERE;
                goto RUN_CHAINS;
            }
        }
    }
    else
    {
        if (g_Supervisor.cfg.frameskipConfig >= this->curFrame)
        {
            Present();
            goto RUN_CHAINS;
        }

    I_HAVE_NO_CLUE_WHY_BUT_I_MUST_JUMP_HERE:
        Present();
        if (g_Supervisor.framerateMultiplier == 0.f)
        {
            if (2 <= g_TickCountToEffectiveFramerate)
            {
                curtime = SDL_GetTicks();
                if (curtime < g_Supervisor.lastFrameTime)
                {
                    g_Supervisor.lastFrameTime = curtime;
                }
                delta = curtime - g_Supervisor.lastFrameTime;
                delta = (delta * 60.) / 2. / 1000.;
                delta /= (g_Supervisor.cfg.frameskipConfig + 1);
                if (delta >= .865)
                {
                    delta = 1.0;
                }
                else if (delta >= .6)
                {
                    delta = 0.8;
                }
                else
                {
                    delta = 0.5;
                }
                g_Supervisor.effectiveFramerateMultiplier = delta;
                g_Supervisor.lastFrameTime = curtime;
                g_TickCountToEffectiveFramerate = 0;
            }
        }
        else
        {
            g_Supervisor.effectiveFramerateMultiplier = g_Supervisor.framerateMultiplier;
        }
        this->curFrame = 0;
        g_TickCountToEffectiveFramerate = g_TickCountToEffectiveFramerate + 1;
    }
    return RENDER_RESULT_KEEP_RUNNING;
}

void GameWindow::Present()
{
    // In D3D, this was done after the present call, but SDL makes no guarantees
    // about the color buffer state immediately after a swap, so it has to be moved to be before it
    g_AnmManager->TakeScreenshotIfRequested();
    if (g_Supervisor.unk198 != 0)
    {
        g_Supervisor.unk198--;
    }

    g_GfxBackend->SwapBuffers();

    return;
}

void GameWindow::CreateGameWindow()
{
    SDL_Init(SDL_INIT_GAMECONTROLLER);

    for (u32 i = 0; i < ARRAY_SIZE(s_RenderBackends); i++)
    {
#ifdef TH06_PS3
        Th06BreadcrumbRenderer("TRY RENDER", s_RenderBackends[i].name);
#endif
        g_GfxBackend = s_RenderBackends[i].TryInit();
        if (g_GfxBackend)
        {
            utils::DebugPrint2("Using renderer backend %s", s_RenderBackends[i].name);
#ifdef TH06_PS3
            Th06BreadcrumbRenderer("USING RENDER", s_RenderBackends[i].name);
#endif
            break;
        }
        utils::DebugPrint2("Renderer creation for backend %s failed", s_RenderBackends[i].name);
#ifdef TH06_PS3
        Th06BreadcrumbRenderer("FAIL RENDER", s_RenderBackends[i].name);
#endif
    }

#ifdef TH06_PS3
    if (!g_GfxBackend)
    {
        th06_breadcrumb("NO RENDER BACKEND");
        exit(1);
    }
#endif

    g_GameWindow.lastActiveAppValue = 1;
}

// LRESULT __stdcall GameWindow::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
// {
//     switch (uMsg)
//     {
//     case 0x3c9:
//         if (g_Supervisor.midiOutput != NULL)
//         {
//             g_Supervisor.midiOutput->UnprepareHeader((LPMIDIHDR)lParam);
//         }
//         break;
//     case WM_ACTIVATEAPP:
//         g_GameWindow.lastActiveAppValue = wParam;
//         if (g_GameWindow.lastActiveAppValue != 0)
//         {
//             g_GameWindow.isAppActive = 0;
//         }
//         else
//         {
//             g_GameWindow.isAppActive = 1;
//         }
//         break;
//     case WM_SETCURSOR:
//         if (!g_Supervisor.cfg.windowed)
//         {
//             if (g_GameWindow.isAppActive != 0)
//             {
//                 SetCursor(LoadCursorA(NULL, IDC_ARROW));
//                 ShowCursor(1);
//             }
//             else
//             {
//                 ShowCursor(0);
//                 SetCursor((HCURSOR)0x0);
//             }
//         }
//         else
//         {
//             SetCursor(LoadCursorA(NULL, IDC_ARROW));
//             ShowCursor(1);
//         }
//
//         return 1;
//     case WM_CLOSE:
//         g_GameWindow.isAppClosing = 1;
//         return 1;
//     }
//     return DefWindowProcA(hWnd, uMsg, wParam, lParam);
// }

ZunResult GameWindow::InitD3dRendering()
{
    if (!g_GfxBackend)
    {
        g_GameErrorContext.Fatal(TH_ERR_D3D_INIT_FAILED);
        return ZUN_ERROR;
    }
    //    u8 using_d3d_hal;
    //    D3DPRESENT_PARAMETERS present_params;
    //    D3DDISPLAYMODE display_mode;
    ZunVec3 eye;
    ZunVec3 at;
    ZunVec3 up;
    f32 half_width;
    f32 half_height;
    f32 aspect_ratio;
    f32 field_of_view_y;
    f32 camera_distance;

    // OpenGL considers textures to be incomplete if the bound texture has no image defined
    // Incomplete textures result in texturing being turned off, but EoSD has places where it
    // uses the texturing engine to color fragments without using the texture itself. The dummy
    // texture is necessary to ensure the texture can't be considered incomplete in these cases.
    g_AnmManager->CreateTextureObject();
    g_AnmManager->dummyTextureHandle = g_AnmManager->currentTextureHandle;
    g_GfxBackend->SetTextureImage(1, 1, PIXEL_RGBA, PIXEL_UNSIGNED_BYTE, NULL);

    //    using_d3d_hal = 1;
    //    std::memset(&present_params, 0, sizeof(D3DPRESENT_PARAMETERS));
    //    g_Supervisor.d3dIface->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display_mode);
    if (!g_Supervisor.cfg.windowed)
    {
        if ((((g_Supervisor.cfg.opts >> GCOS_FORCE_16BIT_COLOR_MODE) & 1) == 1))
        {
            //            present_params.BackBufferFormat = D3DFMT_R5G6B5;
            g_Supervisor.cfg.colorMode16bit = 1;
        }
        else if (g_Supervisor.cfg.colorMode16bit == 0xff)
        {
            //            if ((display_mode.Format == D3DFMT_X8R8G8B8) || (display_mode.Format == D3DFMT_A8R8G8B8))
            //            {
            //                present_params.BackBufferFormat = D3DFMT_X8R8G8B8;
            g_Supervisor.cfg.colorMode16bit = 0;
            g_GameErrorContext.Log(TH_ERR_SCREEN_INIT_32BITS);
            //            }
            //            else
            //            {
            //                present_params.BackBufferFormat = D3DFMT_R5G6B5;
            //                g_Supervisor.cfg.colorMode16bit = 1;
            //                GameErrorContext::Log(&g_GameErrorContext, TH_ERR_SCREEN_INIT_16BITS);
            //            }
        }
        //        else if (g_Supervisor.cfg.colorMode16bit == 0)
        //        {
        //            present_params.BackBufferFormat = D3DFMT_X8R8G8B8;
        //        }
        //        else
        //        {
        //            present_params.BackBufferFormat = D3DFMT_R5G6B5;
        //        }
        if (!((g_Supervisor.cfg.opts >> GCOS_FORCE_60FPS) & 1))
        {

            //            present_params.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        }
        else
        {
            //            present_params.FullScreen_RefreshRateInHz = 60;
            //            present_params.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
            //            GameErrorContext::Log(&g_GameErrorContext, TH_ERR_SET_REFRESH_RATE_60HZ);
        }

        //        if (g_Supervisor.cfg.frameskipConfig == 0)
        //        {
        //            present_params.SwapEffect = D3DSWAPEFFECT_FLIP;
        //        }
        //        else
        //        {
        //            present_params.SwapEffect = D3DSWAPEFFECT_COPY_VSYNC;
        //        }
    }
    //    else
    //    {
    //        present_params.BackBufferFormat = display_mode.Format;
    //        present_params.SwapEffect = D3DSWAPEFFECT_COPY;
    //        present_params.Windowed = 1;
    //    }
    //    present_params.BackBufferWidth = GAME_WINDOW_WIDTH;
    //    present_params.BackBufferHeight = GAME_WINDOW_HEIGHT;
    //    present_params.EnableAutoDepthStencil = true;
    //    present_params.AutoDepthStencilFormat = D3DFMT_D16;
    //    present_params.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;

    g_Supervisor.vsyncEnabled = 1;

    g_Supervisor.lockableBackbuffer = 1;
    //    memcpy(&g_Supervisor.presentParameters, &present_params, sizeof(D3DPRESENT_PARAMETERS));
    //    for (;;)
    //    {
    //        if (((g_Supervisor.cfg.opts >> GCOS_REFERENCE_RASTERIZER_MODE) & 1) != 0)
    //        {
    //            goto REFERENCE_RASTERIZER_MODE;
    //        }
    //        else
    //        {
    //            if (g_Supervisor.d3dIface->CreateDevice(0, D3DDEVTYPE_HAL, g_GameWindow.window,
    //                                                    D3DCREATE_HARDWARE_VERTEXPROCESSING, &present_params,
    //                                                    &g_Supervisor.d3dDevice) < 0)
    //            {
    //                GameErrorContext::Log(&g_GameErrorContext, TH_ERR_TL_HAL_UNAVAILABLE);
    //                if (g_Supervisor.d3dIface->CreateDevice(0, D3DDEVTYPE_HAL, g_GameWindow.window,
    //                                                        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present_params,
    //                                                        &g_Supervisor.d3dDevice) < 0)
    //                {
    //                    GameErrorContext::Log(&g_GameErrorContext, TH_ERR_HAL_UNAVAILABLE);
    //                REFERENCE_RASTERIZER_MODE:
    //                    if (g_Supervisor.d3dIface->CreateDevice(0, D3DDEVTYPE_REF, g_GameWindow.window,
    //                                                            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present_params,
    //                                                            &g_Supervisor.d3dDevice) < 0)
    //                    {
    //                        if (((g_Supervisor.cfg.opts >> GCOS_FORCE_60FPS) & 1) != 0 && !g_Supervisor.vsyncEnabled)
    //                        {
    //                            GameErrorContext::Log(&g_GameErrorContext,
    //                            TH_ERR_CANT_CHANGE_REFRESH_RATE_FORCE_VSYNC);
    //                            present_params.FullScreen_RefreshRateInHz = 0;
    //                            g_Supervisor.vsyncEnabled = 1;
    //                            present_params.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    //                            continue;
    //                        }
    //                        else
    //                        {
    //                            if (present_params.Flags == D3DPRESENTFLAG_LOCKABLE_BACKBUFFER)
    //                            {
    //                                GameErrorContext::Log(&g_GameErrorContext, TH_ERR_BACKBUFFER_NONLOCKED);
    //                                present_params.Flags = 0;
    //                                g_Supervisor.lockableBackbuffer = 0;
    //                                continue;
    //                            }
    //                            else
    //                            {
    //                                GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_D3D_INIT_FAILED);
    //                                if (g_Supervisor.d3dIface != NULL)
    //                                {
    //                                    g_Supervisor.d3dIface->Release();
    //                                    g_Supervisor.d3dIface = NULL;
    //                                }
    //                                return 1;
    //                            }
    //                        }
    //                    }
    //                    else
    //                    {
    //                        GameErrorContext::Log(&g_GameErrorContext, TH_USING_REF_MODE);
    //                        g_Supervisor.hasD3dHardwareVertexProcessing = 0;
    //                        using_d3d_hal = 0;
    //                    }
    //                }
    //                else
    //                {
    //                    GameErrorContext::Log(&g_GameErrorContext, TH_USING_HAL_MODE);
    //                    g_Supervisor.hasD3dHardwareVertexProcessing = 0;
    //                }
    //            }
    //            else
    //            {
    //                GameErrorContext::Log(&g_GameErrorContext, TH_USING_TL_HAL_MODE);
    //                g_Supervisor.hasD3dHardwareVertexProcessing = 1;
    //            }
    //            break;
    //        }
    //    }

    // Camera set up so that at z = 0.0, world coordinates map exactly to (quadrant 4) window coordinates

    half_width = (float)GAME_WINDOW_WIDTH / 2.0;
    half_height = (float)GAME_WINDOW_HEIGHT / 2.0;
    aspect_ratio = (float)GAME_WINDOW_WIDTH / (float)GAME_WINDOW_HEIGHT;
    field_of_view_y = 0.52359879; // PI / 6.0f
    camera_distance = half_height / ZUN_TANF(field_of_view_y / 2.0f);
    up.x = 0.0;
    up.y = 1.0;
    up.z = 0.0;
    at.x = half_width;
    at.y = -half_height;
    at.z = 0.0;
    eye.x = half_width;
    eye.y = -half_height;
    eye.z = -camera_distance;
    //    D3DXMatrixLookAtLH(&g_Supervisor.viewMatrix, &eye, &at, &up);

    ZunMatrix viewMatrix = createViewMatrix(eye, at, up);
    g_AnmManager->SetTransformMatrix(MATRIX_VIEW, viewMatrix);
    g_Supervisor.viewMatrix = viewMatrix;

    ZunMatrix perspectiveMatrix = perspectiveMatrixFromFOV(field_of_view_y, aspect_ratio, 100.0f, 10000.0f);
    g_AnmManager->SetTransformMatrix(MATRIX_PROJECTION, perspectiveMatrix);
    g_Supervisor.projectionMatrix = perspectiveMatrix;

    //    D3DXMatrixPerspectiveFovLH(&g_Supervisor.projectionMatrix, field_of_view_y, aspect_ratio, 100.0, 10000.0);
    //    g_Supervisor.d3dDevice->SetTransform(D3DTS_VIEW, &g_Supervisor.viewMatrix);
    //    g_Supervisor.d3dDevice->SetTransform(D3DTS_PROJECTION, &g_Supervisor.projectionMatrix);
    g_Supervisor.viewport.Get();

    //    g_Supervisor.d3dDevice->GetDeviceCaps(&g_Supervisor.d3dCaps);
    //    if (((((g_Supervisor.cfg.opts >> GCOS_USE_D3D_HW_TEXTURE_BLENDING) & 1) == 0) &&
    //         ((g_Supervisor.d3dCaps.TextureOpCaps & D3DTEXOPCAPS_ADD) == 0)))
    //    {
    //        GameErrorContext::Log(&g_GameErrorContext, TH_ERR_NO_SUPPORT_FOR_D3DTEXOPCAPS_ADD);
    //        g_Supervisor.cfg.opts = g_Supervisor.cfg.opts | (1 << GCOS_USE_D3D_HW_TEXTURE_BLENDING);
    //    }
    //    if (g_Supervisor.ShouldRunAt60Fps() &&
    //        ((g_Supervisor.d3dCaps.PresentationIntervals & D3DPRESENT_INTERVAL_IMMEDIATE) == 0))
    //    {
    //        GameErrorContext::Log(&g_GameErrorContext, TH_ERR_CANT_FORCE_60FPS_NO_ASYNC_FLIP);
    //        g_Supervisor.cfg.opts = g_Supervisor.cfg.opts & ~(1 << GCOS_FORCE_60FPS);
    //    }
    //    if ((((g_Supervisor.cfg.opts >> GCOS_FORCE_16BIT_COLOR_MODE) & 1) == 0) && (using_d3d_hal != 0))
    //    {
    //        if (g_Supervisor.d3dIface->CheckDeviceFormat(0, D3DDEVTYPE_HAL, present_params.BackBufferFormat, 0,
    //                                                     D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8) == 0)
    //        {
    //            g_Supervisor.colorMode16Bits = 1;
    //        }
    //        else
    //        {
    //            g_Supervisor.colorMode16Bits = 0;
    //            g_Supervisor.cfg.opts = g_Supervisor.cfg.opts | (1 << GCOS_FORCE_16BIT_COLOR_MODE);
    //            GameErrorContext::Log(&g_GameErrorContext, TH_ERR_D3DFMT_A8R8G8B8_UNSUPPORTED);
    //        }
    //    }
    InitD3dDevice();
    ScreenEffect::SetViewport(0);
    g_GameWindow.isAppClosing = 0;
    g_Supervisor.lastFrameTime = 0;
    g_Supervisor.framerateMultiplier = 0.0;
    return ZUN_SUCCESS;
}

void GameWindow::InitD3dDevice(void)
{
    AnmManager *anm1;
    AnmManager *anm2;
    AnmManager *anm3;
    AnmManager *anm4;

    g_GfxBackend->Enable(CAPS_BLEND);

    g_GfxBackend->SetBlendMode(BLEND_INV_SRC_ALPHA);

    if (((g_Supervisor.cfg.opts >> GCOS_TURN_OFF_DEPTH_TEST) & 1) == 0)
    {
        g_GfxBackend->Enable(CAPS_DEPTH_TEST);
        g_AnmManager->SetDepthMask(true);
        g_AnmManager->SetDepthFunc(DEPTH_FUNC_LEQUAL);
    }

    g_AnmManager->SetFogColor(0xFF'A0'A0'A0);
    g_AnmManager->SetFogRange(1'000.0f, 5'000.0f);

    // All of these are set per texture object in OpenGL (and also most are defaults)
    //    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    //    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    //    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    //    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    //    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ADDRESSW, D3DTADDRESS_CLAMP);
    //    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    //    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    if (g_AnmManager != NULL)
    {
        anm1 = g_AnmManager;
        anm1->currentBlendMode = 0xff;
        anm4 = g_AnmManager;
        anm4->currentTextureHandle = 0;
    }
    g_Stage.skyFogNeedsSetup = 1;
    return;
}
