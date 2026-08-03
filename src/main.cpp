#include <SDL2/SDL.h>
#include <SDL2/SDL_mouse.h>
#include <cstdio>
#include <cstring>
#ifdef TH06_PS3
#include <lv2/process.h>
#endif

#include "AnmManager.hpp"
#include "Chain.hpp"
#include "FileSystem.hpp"
#include "GameErrorContext.hpp"
#include "GameWindow.hpp"
#include "SoundPlayer.hpp"
#include "Stage.hpp"
#include "Supervisor.hpp"
#if defined(TH06_PS3) && TH06_CD_AUTH
extern "C" int th06_cdauth_run(int pollSeconds); // ps3/cdauth.cpp
extern "C" int th06_cdauth_runtime_poll(void);
extern "C" int th06_cdauth_runtime_wait(void);
extern "C" void th06_cdauth_shutdown(void);
#endif
#ifdef TH06_PS3
extern "C" int th06_rsx_requested = 0; // ○ホールドでRSX試験 (既定SW)
extern "C" int th06_pad_circle_held(void); // ps3/pad.cpp
extern "C" void th06_pad_shutdown(void); // ps3/pad.cpp
extern "C" void th06_guest_init(const char *argv0); // ps3/sentry_guest.cpp
extern "C" int th06_guest_confirm_if_trial(); // 180 rendered frames後にloader trialを承認
extern "C" void th06_breadcrumb(const char *msg); // ps3/eye.cpp
extern "C" void th06_video_poll_events(void); // ps3/video.cpp
extern "C" int th06_video_init(void); // CD prompt uses the proven 640x480 game framebuffer
extern "C" int th06_video_exit_requested(void); // ps3/video.cpp
extern "C" int th06_video_xmb_blocks_game(void); // XMB owns RSX or is in its close cooldown
extern "C" int th06_video_xmb_is_open(void); // actual translucent overlay interval only
extern "C" void th06_video_xmb_idle_flip(void); // present the last completed game frame without drawing
extern "C" void th06_rsx_prepare_xmb_live_frame(void); // fully reclaim RSX state before a live XMB frame
extern "C" void th06_audio_pump(void); // keep the audio port fed with silence while XMB pauses the game
extern "C" void th06_audio_trim_backlog(unsigned int targetFrames); // discard stale post-XMB mix
extern "C" void th06_audio_set_suspended(int suspended); // hard pause for runtime CD removal
extern "C" void th06_audio_shutdown(void); // ps3/audio_out.cpp
extern "C" int th06_overlay_tick(void); // SELECT help / Special Thanks
extern "C" int th06_overlay_force_close(void);
extern "C" void th06_overlay_shutdown(void);
extern "C" void th06_overlay_note_game_render(void);
extern "C" void th06_update_init(void); // ps3/self_update.cpp
extern "C" void th06_update_pump(void);
extern "C" int th06_update_ready(void);
extern "C" void th06_update_spawn(void);
extern "C" void th06_savedata_init(void);
extern "C" void th06_savedata_pump(void);
extern "C" void th06_savedata_flush(void);
#if defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED
extern "C" int th06_arcade_rsx_begin_frame(void);
extern "C" void th06_arcade_rsx_end_frame(void);
#endif
#if defined(TH06_SPRITE_SWEEP) && TH06_SPRITE_SWEEP
extern "C" void th06_sprite_sweep(void); // ps3/sweep.cpp
#endif
#if defined(TH06_MIN_SCENE) && TH06_MIN_SCENE
extern "C" void th06_min_scene(void); // ps3/sweep.cpp
#endif
#define TH06_STR2(x) #x
#define TH06_STR(x) TH06_STR2(x)
#define TH06_BUILD_ID_STR TH06_STR(TH06_BUILD_ID)
#endif
#include "ZunResult.hpp"
#include "i18n.hpp"
#include "utils.hpp"

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

#if defined(TH06_PS3) && defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED
    // SPU ARCADE owns video/flip, while TH06 reuses its proven RsxGfx fixed
    // 640x480 RT. SwapBuffers blits into the cabinet texture instead of a
    // display buffer; one shared mutex serializes the libgcm context.
    th06_rsx_requested = 1;
#elif defined(TH06_PS3) // th06_ps3 dbg: unbuffered stdout + boot breadcrumbs (remove after Phase 1)
    th06_breadcrumb("MAIN ENTER " TH06_BUILD_ID_STR);
    freopen("/dev_hdd0/tmp/th06_stdout.txt", "w", stdout); // th06_ps3 dbg: RPCS3 TTY is not wired for stdio
    freopen("/dev_hdd0/tmp/th06_stderr.txt", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    th06_breadcrumb("FREOPEN OK");
    th06_guest_init(argc > 0 ? argv[0] : nullptr); // freopen後 (展開ログをファイルに残す)
    th06_breadcrumb("GUEST INIT OK");
    th06_update_init();
#ifndef TH06_RSX_FORCE
#define TH06_RSX_FORCE 0
#endif
    // バックエンドは便ごとにビルドで決める (押しボタン運用は廃止: 田中の手を煩わせない)
    th06_rsx_requested = TH06_RSX_FORCE ? 1 : th06_pad_circle_held();
    th06_breadcrumb(th06_rsx_requested ? "BACKEND RSX" : "BACKEND SW");
#ifndef TH06_BUILD_ID
#define TH06_BUILD_ID "unknown"
#endif
    printf("[th06_ps3] BUILD %s\n", TH06_BUILD_ID);
    printf("[th06_ps3] MAIN ENTER\n");
#if TH06_CD_AUTH
    {
        // The CD prompt is a normal game framebuffer. SDL/RsxGfx reuses this
        // idempotent initialization after authentication.
        if (th06_video_init() < 0)
        {
            th06_breadcrumb("CD UI VIDEO NG");
            return 1;
        }
        th06_breadcrumb("CD UI VIDEO OK");
        printf("[th06_ps3] CD auth...\n");
        if (th06_cdauth_run(-1) != 0)
        {
            printf("[th06_ps3] CD auth REFUSED - insert the EoSD disc\n");
            th06_cdauth_shutdown();
            return 1; // v1: refusal screen deferred (font path) - see DECISIONS D-005
        }
    }
#endif
#endif

    i32 renderResult = 0;
#ifdef TH06_PS3
    u32 dbgHeartbeat = 0; // th06_ps3 dbg (declared here: goto crosses init otherwise)
    bool xmbActive = false;
    bool xmbClosePaused = false;
#endif
    //    MSG msg;
    //    i32 waste1, waste2, waste3, waste4, waste5, waste6;

    //    if (utils::CheckForRunningGameInstance())
    //    {
    //        g_GameErrorContext.Flush();
    //
    //        return 1;
    //    }

    //    g_Supervisor.hInstance = hInstance;

#ifdef TH06_PS3
    // Hydrate the original score.dat/replay working set from a proper PS3
    // SaveData entry before any game subsystem opens it. On the first build
    // with this bridge, adopt the old TH06PS300 progress non-destructively.
    th06_savedata_init();
    printf("[th06_ps3] LoadConfig...\n");
    th06_breadcrumb("LOADCONFIG");
#endif
#if defined(TH06_PS3) && defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED
    // The cabinet lives inside another title and must never block on newlib
    // config I/O from its worker PPU thread. Use the game's normal defaults;
    // score/replay data remains independent and is loaded by the game itself.
    std::memset(&g_Supervisor.cfg, 0, sizeof(GameConfiguration));
    g_Supervisor.cfg.opts =
        1 << GCOS_USE_D3D_HW_TEXTURE_BLENDING;
    g_Supervisor.cfg.lifeCount = 2;
    g_Supervisor.cfg.bombCount = 3;
    g_Supervisor.cfg.colorMode16bit = 0xff;
    g_Supervisor.cfg.version = GAME_VERSION;
    g_Supervisor.cfg.padXAxis = 600;
    g_Supervisor.cfg.padYAxis = 600;
    g_Supervisor.cfg.musicMode = WAV;
    g_Supervisor.cfg.playSounds = 1;
    g_Supervisor.cfg.defaultDifficulty = 1;
    g_Supervisor.cfg.windowed = false;
    g_Supervisor.cfg.frameskipConfig = 0;
    g_Supervisor.cfg.controllerMapping = g_ControllerMapping;
    th06_breadcrumb("CONFIG DEFAULT");
#else
    if (g_Supervisor.LoadConfig(TH_CONFIG_FILE) != ZUN_SUCCESS)
    {
        g_GameErrorContext.Flush();
        return -1;
    }
#endif

    //    if (GameWindow::InitD3dInterface())
    //    {
    //        g_GameErrorContext.Flush();
    //        return 1;
    //    }

    //    SystemParametersInfo(SPI_GETSCREENSAVEACTIVE, 0, &g_GameWindow.screenSaveActive, 0);
    //    SystemParametersInfo(SPI_GETLOWPOWERACTIVE, 0, &g_GameWindow.lowPowerActive, 0);
    //    SystemParametersInfo(SPI_GETPOWEROFFACTIVE, 0, &g_GameWindow.powerOffActive, 0);
    //    SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, 0, NULL, SPIF_SENDCHANGE);
    //    SystemParametersInfo(SPI_SETLOWPOWERACTIVE, 0, NULL, SPIF_SENDCHANGE);
    //    SystemParametersInfo(SPI_SETPOWEROFFACTIVE, 0, NULL, SPIF_SENDCHANGE);

restart:
#ifdef TH06_PS3
    printf("[th06_ps3] CreateGameWindow...\n");
    th06_breadcrumb("CREATEWINDOW");
#endif
    GameWindow::CreateGameWindow();

    g_AnmManager = new AnmManager();

    if (GameWindow::InitD3dRendering() != ZUN_SUCCESS)
    {
        g_GameErrorContext.Flush();
        return 1;
    }

#ifdef TH06_PS3
    printf("[th06_ps3] InitD3dRendering OK, sound/input init...\n");
    th06_breadcrumb("RENDER INIT OK");
#endif
    g_SoundPlayer.InitializeDSound();
    th06_breadcrumb("SOUND INIT OK");
    Controller::GetJoystickCaps();
    Controller::ResetKeyboard();
    th06_breadcrumb("INPUT INIT OK");

    if (Supervisor::RegisterChain() != ZUN_SUCCESS)
    {
        goto stop;
    }
    th06_breadcrumb("CHAIN INIT OK");
    if (!g_Supervisor.cfg.windowed)
    {
        SDL_ShowCursor(SDL_DISABLE);
    }

    g_GameWindow.curFrame = 0;
#ifdef TH06_PS3
    printf("[th06_ps3] MAIN LOOP ENTER\n");
    th06_breadcrumb("MAIN LOOP");
#endif

    while (true)
    {
        SDL_Event e;

        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT)
            {
                goto stop;
            }
        }

#ifdef TH06_PS3
        th06_video_poll_events();
        th06_update_pump();
        if (th06_update_ready())
        {
            th06_breadcrumb("DUPDATE STOP");
            goto stop;
        }
        if (th06_video_exit_requested())
        {
            th06_breadcrumb("EXIT GAME REQUEST");
            goto stop;
        }
        // Keep TH06 updating behind the translucent in-game XMB. Unlike the
        // old static-snapshot path, each overlay frame explicitly reclaims all
        // RSX state because XMB and TH06 otherwise invalidate one another's
        // cached programs/surfaces. SDL input is suppressed separately so XMB
        // navigation cannot move or shoot in the game.
        if (th06_video_xmb_is_open())
        {
            th06_overlay_force_close();
            if (!xmbActive)
            {
                xmbActive = true;
                th06_breadcrumb("XMB GAME LIVE");
            }
            xmbClosePaused = false;
            GameWindow::InitD3dDevice();
            th06_rsx_prepare_xmb_live_frame();
        }
        // Keep the proven one-second quarantine after DRAW_END/MENU_CLOSE for
        // this first live candidate. Delayed compositor commands have
        // previously overwritten fresh game frames black/brown on hardware.
        else if (th06_video_xmb_blocks_game())
        {
            if (!xmbClosePaused)
            {
                xmbClosePaused = true;
                th06_breadcrumb("XMB CLOSE QUARANTINE");
                th06_audio_trim_backlog(2048);
            }
            th06_audio_pump();
            th06_video_xmb_idle_flip();
            continue;
        }
        else if (xmbActive || xmbClosePaused)
        {
            xmbActive = false;
            xmbClosePaused = false;
            th06_breadcrumb("XMB GAME RESUME");
            // XMB invalidates hardware state without updating TH06's
            // AnmManager-side caches. Force the first VM after resume to
            // reissue blend/depth/texture state instead of trusting cached
            // values from before the overlay.
            GameWindow::InitD3dDevice();
        }
#if TH06_CD_AUTH
        if (!th06_cdauth_runtime_poll())
        {
            // The physical CD is the running licence gate, not only an
            // installer source. Freeze TH06 and its audio immediately on an
            // eject event. Re-enter only after the replacement disc passes
            // the same six-DAT identity check.
            th06_overlay_force_close();
            th06_audio_set_suspended(1);
            th06_breadcrumb("CD RUNTIME PAUSE");
            printf("[th06_ps3] CD runtime PAUSE\n");
            if (th06_cdauth_runtime_wait() != 0)
                goto stop;
            th06_audio_set_suspended(0);
            th06_audio_trim_backlog(0);
            GameWindow::InitD3dDevice();
            th06_breadcrumb("CD RUNTIME RESUME");
            printf("[th06_ps3] CD runtime RESUME\n");
            continue;
        }
#endif
        th06_savedata_pump();
        const int overlayState = th06_overlay_tick();
        if (overlayState == 1)
        {
            // Keep the port responsive but do not advance TH06 or replay time.
            th06_audio_pump();
            continue;
        }
        if (overlayState == 2)
        {
            // CPU framebuffer presentation changes RSX state. Reassert the
            // game's programs, surfaces and blend state before resuming.
            GameWindow::InitD3dDevice();
            // Do not poll the pad again on the closing frame. This both keeps
            // one sample per frame and prevents the close button leaking into
            // the title/game menu.
            th06_audio_pump();
            continue;
        }
        if ((dbgHeartbeat++ % 600) == 0)
            printf("[th06_ps3] heartbeat frame=%u\n", dbgHeartbeat - 1);
#if defined(TH06_SPRITE_SWEEP) && TH06_SPRITE_SWEEP
        // 寄生型掃引 (2026-07-26): 起動前フックはRSXが1スプライトも描けない未解決障害
        // (NIGHTREPORT参照)。タイトル到達後=描ける状態が確立してから乗っ取る
        if (dbgHeartbeat == 400)
        {
            th06_sprite_sweep();
            printf("[sweep] exiting\n");
            return 0;
        }
#endif
#if defined(TH06_MIN_SCENE) && TH06_MIN_SCENE
        // 最小シーン (2026-07-26): 通常タイトル/ゲーム進行から切り離して、
        // 実ANM経路だけを固定描画する。RSX初回draw不発対策として少しだけ
        // メインループを回してから乗っ取る。
        if (dbgHeartbeat == 60)
        {
            th06_min_scene();
            printf("[minscene] exiting\n");
            return 0;
        }
#endif
#endif
#ifdef TH06_PS3
        th06_overlay_note_game_render();
#endif
#if defined(TH06_PS3) && defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED
        if (!th06_arcade_rsx_begin_frame())
        {
            th06_breadcrumb("TH06 GCM LOCK TIMEOUT");
            SDL_Delay(1);
            continue;
        }
        // The outer arcade frame changes every persistent graphics binding.
        // Invalidate RsxGfx's caches and fully reclaim its RT/program/ROP
        // state before every embedded frame (same proven path as XMB return).
        th06_rsx_prepare_xmb_live_frame();
#endif
        renderResult = g_GameWindow.Render();
#if defined(TH06_PS3) && defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED
        th06_arcade_rsx_end_frame();
#endif
        if (renderResult != 0)
        {
            break;
        }
#ifdef TH06_PS3
        if (dbgHeartbeat == 180)
            th06_guest_confirm_if_trial();
#endif

        //        SDL_Delay(1000.0f / 60.0f);

        //        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        //        {
        //            TranslateMessage(&msg);
        //            DispatchMessage(&msg);
        //        }
        //        else
        //        {
        //            testCoopLevelRes = g_Supervisor.d3dDevice->TestCooperativeLevel();
        //            if (testCoopLevelRes == D3D_OK)
        //            {
        //        renderResult = g_GameWindow.Render();
        //                if (renderResult != 0)
        //                {
        //                    goto stop;
        //                }
        //            }
        //            else if (testCoopLevelRes == D3DERR_DEVICENOTRESET)
        //            {
        //                g_AnmManager->ReleaseSurfaces();
        //                testResetRes = g_Supervisor.d3dDevice->Reset(&g_Supervisor.presentParameters);
        //                if (testResetRes != 0)
        //                {
        //                    goto stop;
        //                }
        //                GameWindow::InitD3dDevice();
        //                g_Supervisor.unk198 = 3;
        //            }
        //        }
    }

stop:
#ifdef TH06_PS3
    th06_breadcrumb("MAIN STOP");
    th06_savedata_flush();
    th06_audio_set_suspended(0);
#endif
    g_Chain.Release();
    g_SoundPlayer.Release();
#ifdef TH06_PS3
    th06_overlay_shutdown();
#if TH06_CD_AUTH
    th06_cdauth_shutdown();
#endif
#if !(defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED)
    th06_audio_shutdown();
#endif
#endif

    delete g_AnmManager;
    g_AnmManager = NULL;

    if (g_GfxBackend != NULL)
        delete g_GfxBackend;
    SDL_Quit();
#ifdef TH06_PS3
    th06_pad_shutdown();
#endif

    if (renderResult == 2)
    {
        g_GameErrorContext.ResetContext();

        g_GameErrorContext.Log(TH_ERR_OPTION_CHANGED_RESTART);

        if (!g_Supervisor.cfg.windowed)
        {
            SDL_ShowCursor(SDL_ENABLE);
        }

        goto restart;
    }

    FileSystem::WriteDataToFile(TH_CONFIG_FILE, &g_Supervisor.cfg, sizeof(g_Supervisor.cfg));
    //    SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, g_GameWindow.screenSaveActive, NULL, SPIF_SENDCHANGE);
    //    SystemParametersInfo(SPI_SETLOWPOWERACTIVE, g_GameWindow.lowPowerActive, NULL, SPIF_SENDCHANGE);
    //    SystemParametersInfo(SPI_SETPOWEROFFACTIVE, g_GameWindow.powerOffActive, NULL, SPIF_SENDCHANGE);

    SDL_ShowCursor(SDL_ENABLE);
    g_GameErrorContext.Flush();
#ifdef TH06_PS3
    if (th06_update_ready())
        th06_update_spawn();
#if !(defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED)
    sysProcessExit(0);
#endif
#endif
    return 0;
}
