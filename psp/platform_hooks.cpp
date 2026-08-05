#include "platform_hooks.hpp"

#include "diag.hpp"
#include "memory.hpp"

#include <pspkernel.h>
#include <pspmath.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_surface.h>

#include "AnmManager.hpp"
#include "AnmIdx.hpp"
#include "BulletManager.hpp"
#include "Chain.hpp"
#include "AsciiManager.hpp"
#include "EclManager.hpp"
#include "Effect.hpp"
#include "EffectManager.hpp"
#include "EnemyManager.hpp"
#include "FileSystem.hpp"
#include "GameManager.hpp"
#include "GameWindow.hpp"
#include "Gui.hpp"
#include "graphics/PspGuGfx.hpp"
#include "ItemManager.hpp"
#include "MainMenu.hpp"
#include "Player.hpp"
#include "ReplayManager.hpp"
#include "ResultScreen.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"
#include "i18n.hpp"
#include "pbg3/Pbg3Archive.hpp"

extern ReplayManager *g_ReplayManager;

namespace
{
volatile unsigned int gTransitionTraceBudget;
volatile unsigned int gTransitionTraceMarkers;
unsigned int gDemoLoadStartUs;
unsigned int gDemoAssetCount;
unsigned int gDemoAssetBytes;
volatile unsigned int gStageReinitTrace;
volatile unsigned int gResultTransitionTrace;
volatile unsigned int gBossDeathTraceFrames;
unsigned long long gPerfCalcUs;
unsigned long long gPerfDrawUs;
unsigned int gPerfCalcCalls;
unsigned int gPerfDrawCalls;
unsigned long long gPerfFlightCalcUs;
unsigned long long gPerfFlightDrawUs;
volatile unsigned int gBulletRenderMath;
volatile unsigned int gBulletFastDraws;
volatile unsigned int gBulletFallbackDraws;
unsigned int gReplayCompactedBytes;
StageReplayData *gReplayStoppedStages[7];

// Real-hardware stage changes used to stop in EclManager::Unload(), inside
// one of the two newlib free() calls.  ECL data is immutable after Load(), and
// the largest file in the retail TH06ST.DAT is 60,796 bytes, so PSP keeps one
// bounded arena and reuses it for every stage.  The temporary PBG buffer is
// freed immediately after copying, before ECL execution can damage adjacent
// allocator metadata.  This also removes allocation/free churn from the
// stage-transition path without changing the engine sources.
constexpr std::size_t kPspEclArenaBytes = 64 * 1024;
constexpr int kPspEclMaxSubs = 4096;
alignas(64) unsigned char gPspEclArena[kPspEclArenaBytes];
EclRawInstr *gPspEclSubTable[kPspEclMaxSubs];
unsigned int gPspEclLoadedBytes;
unsigned int gMassBulletPopupStride;
unsigned int gMassBulletPopupIndex;
Bullet *gPspBulletDrawBuckets[4][640];

long CurrentStackFreeKb()
{
    const int freeBytes = sceKernelGetThreadStackFreeSize(sceKernelGetThreadId());
    return freeBytes >= 0 ? freeBytes / 1024 : freeBytes;
}

unsigned int FloatBits(float value)
{
    unsigned int bits;
    static_assert(sizeof(bits) == sizeof(value), "float bit log expects 32-bit float");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float FastRenderRint(float value)
{
    float result;
    asm volatile("round.w.s %0, %1\n\t"
                 "cvt.s.w %0, %0"
                 : "=&f"(result)
                 : "f"(value));
    return result;
}

bool CanFastRenderRint(float value)
{
    return std::isfinite(value) && value >= -32768.0f && value <= 32767.0f;
}

bool IsSimpleSpriteMatrix(const ZunMatrix &matrix)
{
    // Draw/Draw2 overwrite translation, and their ZunVec3 multiply ignores
    // the fourth column.  The remaining six off-diagonal terms are the only
    // ones that would prevent the PSP sprite shortcut below.
    return matrix.m[1][0] == 0.0f && matrix.m[2][0] == 0.0f &&
           matrix.m[0][1] == 0.0f && matrix.m[2][1] == 0.0f &&
           matrix.m[0][2] == 0.0f && matrix.m[1][2] == 0.0f;
}

void WriteBulletVertex(VertexTex1Xyzrhw &dst, float x, float y, float z, float u, float v)
{
    dst.position.x = x;
    dst.position.y = y;
    dst.position.z = z;
    dst.position.w = 1.0f;
    dst.textureUV.x = u;
    dst.textureUV.y = v;
}

void AppendBulletQuad(AnmManager *manager, const AnmVm *vm, const float *x, const float *y)
{
    // State setters flush only when their value actually changes.  Calling
    // them here preserves the engine's ordering while consecutive bullets
    // with the same texture/color/blend remain one batch.
    manager->SetCurrentTexture(manager->textures[vm->sprite->sourceFileIndex].handle);
    manager->SetVertexAttributes(VERTEX_ATTR_TEX_COORD);
    manager->SetRenderStateForVm(vm);

    if (manager->vertexBufferEndPtr + 6 > manager->vertexBuffer + 0x18000)
        manager->FlushVertexBuffer();

    VertexTex1Xyzrhw *dst = manager->vertexBufferEndPtr;
    const float u0 = vm->sprite->uvStart.x + vm->uvScrollPos.x;
    const float u1 = vm->sprite->uvEnd.x + vm->uvScrollPos.x;
    const float v0 = vm->sprite->uvStart.y + vm->uvScrollPos.y;
    const float v1 = vm->sprite->uvEnd.y + vm->uvScrollPos.y;
    const float z = vm->pos.z;

    // Emit the final two triangles directly.  The inherited path first made
    // four temporary vertices and then copied three whole structs twice.
    WriteBulletVertex(dst[0], x[0], y[0], z, u0, v0);
    WriteBulletVertex(dst[1], x[1], y[1], z, u1, v0);
    WriteBulletVertex(dst[2], x[2], y[2], z, u0, v1);
    WriteBulletVertex(dst[3], x[1], y[1], z, u1, v0);
    WriteBulletVertex(dst[4], x[2], y[2], z, u0, v1);
    WriteBulletVertex(dst[5], x[3], y[3], z, u1, v1);
    manager->vertexBufferEndPtr += 6;
    ++manager->spritesToDraw;

    // AnmManager's generic path caches UVs in a private TU-local temporary.
    // It was deliberately bypassed above, so force the next non-bullet draw
    // to refresh that temporary rather than trusting stale UVs.
    manager->currentSprite = nullptr;
}

bool BulletQuadOutsideViewport(const float *x, const float *y)
{
    float minX = x[0];
    float maxX = x[0];
    float minY = y[0];
    float maxY = y[0];
    for (int i = 1; i < 4; ++i)
    {
        minX = std::min(minX, x[i]);
        maxX = std::max(maxX, x[i]);
        minY = std::min(minY, y[i]);
        maxY = std::max(maxY, y[i]);
    }
    const ZunViewport &viewport = g_Supervisor.viewport;
    return maxX < viewport.x || maxY < viewport.y ||
           minX > viewport.x + viewport.width || minY > viewport.y + viewport.height;
}

bool FastBulletOrthographicDraw(AnmManager *manager, const AnmVm *vm)
{
    if (!manager || !vm || !vm->sprite || !vm->flags.isVisible || !vm->flags.flag1 || vm->color == 0)
        return false;
    if (vm->rotation.x != 0.0f || vm->rotation.y != 0.0f ||
        !std::isfinite(vm->rotation.z) || !std::isfinite(vm->pos.x) ||
        !std::isfinite(vm->pos.y) || !std::isfinite(vm->pos.z) ||
        !std::isfinite(vm->scaleX) || !std::isfinite(vm->scaleY))
        return false;
    if (((g_Supervisor.cfg.opts >> GCOS_DONT_USE_VERTEX_BUF) & 1) != 0)
        return false;

    float x[4];
    float y[4];
    if (vm->rotation.z == 0.0f)
    {
        const float halfWidth = vm->sprite->widthPx * vm->scaleX * 0.5f;
        const float halfHeight = vm->sprite->heightPx * vm->scaleY * 0.5f;
        const float rawLeft = (vm->flags.anchor & AnmVmAnchor_Left) ? vm->pos.x : vm->pos.x - halfWidth;
        const float rawRight = (vm->flags.anchor & AnmVmAnchor_Left) ? vm->pos.x + halfWidth * 2.0f
                                                                     : vm->pos.x + halfWidth;
        const float rawTop = (vm->flags.anchor & AnmVmAnchor_Top) ? vm->pos.y : vm->pos.y - halfHeight;
        const float rawBottom = (vm->flags.anchor & AnmVmAnchor_Top) ? vm->pos.y + halfHeight * 2.0f
                                                                     : vm->pos.y + halfHeight;
        if (!CanFastRenderRint(rawLeft) || !CanFastRenderRint(rawRight) ||
            !CanFastRenderRint(rawTop) || !CanFastRenderRint(rawBottom))
            return false;
        const float left = FastRenderRint(rawLeft);
        const float right = FastRenderRint(rawRight);
        const float top = FastRenderRint(rawTop);
        const float bottom = FastRenderRint(rawBottom);
        x[0] = x[2] = left;
        x[1] = x[3] = right;
        y[0] = y[1] = top;
        y[2] = y[3] = bottom;
    }
    else
    {
        if (vm->rotation.z < -16.0f * ZUN_PI || vm->rotation.z > 16.0f * ZUN_PI)
            return false;
        float sine;
        float cosine;
        vfpu_sincos(vm->rotation.z, &sine, &cosine);
        const float rawHalfWidth = vm->sprite->widthPx * vm->scaleX * 0.5f;
        const float rawHalfHeight = vm->sprite->heightPx * vm->scaleY * 0.5f;
        if (!CanFastRenderRint(vm->pos.x) || !CanFastRenderRint(vm->pos.y) ||
            !CanFastRenderRint(rawHalfWidth) || !CanFastRenderRint(rawHalfHeight))
            return false;
        const float xOffset = FastRenderRint(vm->pos.x);
        const float yOffset = FastRenderRint(vm->pos.y);
        const float halfWidth = FastRenderRint(rawHalfWidth);
        const float halfHeight = FastRenderRint(rawHalfHeight);
        const float localX[4] = {-halfWidth - 0.5f, halfWidth - 0.5f,
                                 -halfWidth - 0.5f, halfWidth - 0.5f};
        const float localY[4] = {-halfHeight - 0.5f, -halfHeight - 0.5f,
                                  halfHeight - 0.5f, halfHeight - 0.5f};
        for (int i = 0; i < 4; ++i)
        {
            x[i] = localX[i] * cosine + localY[i] * sine + xOffset;
            y[i] = -localX[i] * sine + localY[i] * cosine + yOffset;
            if (vm->flags.anchor & AnmVmAnchor_Left)
                x[i] += halfWidth;
            if (vm->flags.anchor & AnmVmAnchor_Top)
                y[i] += halfHeight;
        }
    }

    if (BulletQuadOutsideViewport(x, y))
        return true;
    manager->SetProjectionMode(PROJECTION_MODE_ORTHOGRAPHIC);
    AppendBulletQuad(manager, vm, x, y);
    return true;
}

bool FastBulletPerspectiveDraw(AnmManager *manager, const AnmVm *vm)
{
    if (!manager || !vm || !vm->sprite || !vm->flags.isVisible || !vm->flags.flag1 || vm->color == 0 ||
        !IsSimpleSpriteMatrix(vm->matrix) || vm->rotation.x != 0.0f || vm->rotation.y != 0.0f ||
        !std::isfinite(vm->rotation.z) || !std::isfinite(vm->pos.x) || !std::isfinite(vm->pos.y) ||
        !std::isfinite(vm->pos.z) || !std::isfinite(vm->scaleX) || !std::isfinite(vm->scaleY) ||
        ((g_Supervisor.cfg.opts >> GCOS_DONT_USE_VERTEX_BUF) & 1) != 0)
        return false;
    if (vm->rotation.z < -16.0f * ZUN_PI || vm->rotation.z > 16.0f * ZUN_PI)
        return false;

    if (!CanFastRenderRint(vm->pos.x) || !CanFastRenderRint(vm->pos.y))
        return false;
    float tx = FastRenderRint(vm->pos.x) - 0.5f;
    float ty = -FastRenderRint(vm->pos.y) + 0.5f;
    if (vm->flags.anchor & AnmVmAnchor_Left)
        tx += vm->sprite->widthPx * vm->scaleX * 0.5f;
    if (vm->flags.anchor & AnmVmAnchor_Top)
        ty -= vm->sprite->heightPx * vm->scaleY * 0.5f;

    float sine = 0.0f;
    float cosine = 1.0f;
    if (vm->rotation.z != 0.0f)
        vfpu_sincos(vm->rotation.z, &sine, &cosine);
    const float sx = vm->matrix.m[0][0] * vm->scaleX;
    const float sy = vm->matrix.m[1][1] * -vm->scaleY;
    float x[4];
    float y[4];
    for (int i = 0; i < 4; ++i)
    {
        const float localX = manager->vertexBufferContents[i].position.x * sx;
        const float localY = manager->vertexBufferContents[i].position.y * sy;
        x[i] = localX * cosine - localY * sine + tx;
        y[i] = localX * sine + localY * cosine + ty;
    }
    manager->SetProjectionMode(PROJECTION_MODE_PERSPECTIVE);
    AppendBulletQuad(manager, vm, x, y);
    return true;
}

bool BossDeathTracing()
{
    return __atomic_load_n(&gBossDeathTraceFrames, __ATOMIC_ACQUIRE) != 0;
}

bool BossDeathTraceArmingEnabled()
{
#if defined(TH06_PSP_REPLAY_TRACE)
    return true;
#else
    return th06_psp_periodic_diag_enabled() != 0;
#endif
}

bool DetailedStageTransitionTracing()
{
#if defined(TH06_PSP_STAGE_SOAK) || defined(TH06_PSP_RECORD_SOAK) || defined(TH06_PSP_RESULT_SOAK)
    return true;
#else
    return th06_psp_periodic_diag_enabled() != 0;
#endif
}

bool GameTeardownTracing()
{
    return __atomic_load_n(&gStageReinitTrace, __ATOMIC_ACQUIRE) != 0 ||
           __atomic_load_n(&gResultTransitionTrace, __ATOMIC_ACQUIRE) != 0;
}

// Reimu A's homing update divides by the length of its newly steered velocity.
// On a frame where the desired turn exactly cancels the old velocity that
// length is zero.  x86 tolerated the resulting NaN, while the real PSP has
// already proven that the same zero-normalize family can terminate the game.
// Repair only the singular input before the unchanged Player callback reaches
// its divide; every ordinary trajectory is left untouched.
void StabilizeReimuAHomingBullets(Player *player)
{
    if (player == nullptr)
        return;

    constexpr float kMinLengthSquared = 1.0e-12f;
    for (int idx = 0; idx < 80; ++idx)
    {
        PlayerBullet &bullet = player->bullets[idx];
        if (bullet.bulletState != BULLET_STATE_FIRED || bullet.bulletType != BULLET_TYPE_1)
            continue;

        float speed = bullet.unk_134.y;
        if (!std::isfinite(speed) || speed <= 0.000001f)
        {
            speed = 1.0f;
            bullet.unk_134.y = speed;
        }

        if (player->positionOfLastEnemyHit.x > -100.0f && bullet.unk_140.AsFrames() < 40 &&
            bullet.unk_140.HasTicked())
        {
            const float dx = player->positionOfLastEnemyHit.x - bullet.position.x;
            const float dy = player->positionOfLastEnemyHit.y - bullet.position.y;
            float turnDivisor = std::sqrt(dx * dx + dy * dy) / (speed / 4.0f);
            if (!std::isfinite(turnDivisor) || turnDivisor < 1.0f)
                turnDivisor = 1.0f;

            const float nextX = dx / turnDivisor + bullet.velocity.x;
            const float nextY = dy / turnDivisor + bullet.velocity.y;
            const float nextLengthSquared = nextX * nextX + nextY * nextY;
            if (!std::isfinite(nextLengthSquared) || nextLengthSquared <= kMinLengthSquared)
            {
                // The engine adds dx/d and dy/d immediately after this hook.
                // Choose an old velocity which makes that result (0,-1).
                bullet.velocity.x = -dx / turnDivisor;
                bullet.velocity.y = -dy / turnDivisor - 1.0f;
            }
        }
        else
        {
            const float velocityLengthSquared =
                bullet.velocity.x * bullet.velocity.x + bullet.velocity.y * bullet.velocity.y;
            if (!std::isfinite(velocityLengthSquared) || velocityLengthSquared <= kMinLengthSquared)
            {
                bullet.velocity.x = 0.0f;
                bullet.velocity.y = -1.0f;
            }
        }
    }
}
}

extern "C" float __real_sinf(float value);
extern "C" float __real_cosf(float value);
extern "C" float __real_rintf(float value);

extern "C" float __wrap_sinf(float value)
{
    if (__atomic_load_n(&gBulletRenderMath, __ATOMIC_RELAXED) != 0 && std::isfinite(value) &&
        value >= -16.0f * ZUN_PI && value <= 16.0f * ZUN_PI)
        return vfpu_sinf(value);
    return __real_sinf(value);
}

extern "C" float __wrap_cosf(float value)
{
    if (__atomic_load_n(&gBulletRenderMath, __ATOMIC_RELAXED) != 0 && std::isfinite(value) &&
        value >= -16.0f * ZUN_PI && value <= 16.0f * ZUN_PI)
        return vfpu_cosf(value);
    return __real_cosf(value);
}

extern "C" float __wrap_rintf(float value)
{
    // Screen-space bullet geometry is small and finite.  Keep the generic
    // newlib path for every other caller and for any corrupted/out-of-range
    // input so the PSP FPU can never see the invalid conversion traps already
    // encountered during bring-up.
    if (__atomic_load_n(&gBulletRenderMath, __ATOMIC_RELAXED) != 0 && std::isfinite(value) &&
        value >= -32768.0f && value <= 32767.0f)
        return FastRenderRint(value);
    return __real_rintf(value);
}

extern "C" ZunResult th06_real_anm_draw(AnmManager *manager, const AnmVm *vm)
    asm("__real__ZN10AnmManager4DrawEPK5AnmVm");
extern "C" ZunResult th06_wrap_anm_draw(AnmManager *manager, const AnmVm *vm)
    asm("__wrap__ZN10AnmManager4DrawEPK5AnmVm");

extern "C" ZunResult th06_wrap_anm_draw(AnmManager *manager, const AnmVm *vm)
{
    if (__atomic_load_n(&gBulletRenderMath, __ATOMIC_RELAXED) != 0)
    {
        if (FastBulletOrthographicDraw(manager, vm))
        {
            __atomic_add_fetch(&gBulletFastDraws, 1u, __ATOMIC_RELAXED);
            return ZUN_SUCCESS;
        }
        __atomic_add_fetch(&gBulletFallbackDraws, 1u, __ATOMIC_RELAXED);
    }
    return th06_real_anm_draw(manager, vm);
}

extern "C" ZunResult th06_real_anm_draw2(AnmManager *manager, const AnmVm *vm)
    asm("__real__ZN10AnmManager5Draw2EPK5AnmVm");
extern "C" ZunResult th06_wrap_anm_draw2(AnmManager *manager, const AnmVm *vm)
    asm("__wrap__ZN10AnmManager5Draw2EPK5AnmVm");

extern "C" ZunResult th06_wrap_anm_draw2(AnmManager *manager, const AnmVm *vm)
{
    if (__atomic_load_n(&gBulletRenderMath, __ATOMIC_RELAXED) != 0)
    {
        if (FastBulletPerspectiveDraw(manager, vm))
        {
            __atomic_add_fetch(&gBulletFastDraws, 1u, __ATOMIC_RELAXED);
            return ZUN_SUCCESS;
        }
        __atomic_add_fetch(&gBulletFallbackDraws, 1u, __ATOMIC_RELAXED);
    }
    return th06_real_anm_draw2(manager, vm);
}

extern "C" void th06_psp_bullet_fast_stats(unsigned int *fastDraws, unsigned int *fallbackDraws)
{
    if (fastDraws)
        *fastDraws = __atomic_load_n(&gBulletFastDraws, __ATOMIC_RELAXED);
    if (fallbackDraws)
        *fallbackDraws = __atomic_load_n(&gBulletFallbackDraws, __ATOMIC_RELAXED);
}

// A handful of the inherited engine's diagnostic calls sit outside their
// platform guards.  Rather than leave them as dead no-ops, route them into the
// PSP boot record: SOUND INIT OK / INPUT INIT OK / CHAIN INIT OK in
// src/main.cpp are exactly the stages we cannot otherwise see, and it costs
// nothing to read them, with src/ still untouched.
//
// Boot-scoped, like the rest of psp/diag: once the engine presents a frame the
// hook goes quiet, so breadcrumbs on any per-frame path cost one branch.
extern "C" void th06_breadcrumb(const char *msg)
{
    if (msg != nullptr && th06_psp_boot_phase())
        th06_psp_boot_note(msg);
}

// AnmManager is a single ~2.5 MiB heap allocation.  On real hardware the
// first GE build died between backend creation and its first draw, which is
// also exactly where that allocation happens.  Wrap only large C++ new calls
// and only during boot; ordinary allocations and normal play stay untouched.
extern "C" void *__real__Znwj(unsigned int bytes);
extern "C" void *__wrap__Znwj(unsigned int bytes)
{
    th06_psp_note_new_request(bytes, static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(
                                              __builtin_return_address(0))));
    static bool tracing;
    // Asset setup after the title screen is the first substantial allocation
    // burst on PSP.  Keep the old 1 MiB boot threshold, but retain 64 KiB+
    // allocations at runtime so a failed demo/stage transition leaves the
    // requested size as evidence.
    const bool boot = th06_psp_boot_phase() != 0;
    const unsigned int threshold = boot ? 1024u * 1024u : 64u * 1024u;
    const bool trace = bytes >= threshold && !tracing && (boot || th06_psp_verbose_transition_diag());
    if (trace)
    {
        tracing = true;
        th06_psp_runtime_progress();
        th06_psp_boot_notef("LARGE NEW ENTER %uB", bytes);
    }
    void *result = __real__Znwj(bytes);
    if (trace)
    {
        th06_psp_boot_notef("LARGE NEW OK %uB @%p", bytes, result);
        th06_psp_runtime_progress();
        tracing = false;
    }
    return result;
}

// Linker wraps keep the inherited engine unchanged while exposing the exact
// PBG3 boundary.  FileSystem::OpenPath normally logs only before decode (and
// its DEBUG print is compiled out), which cannot distinguish a successful
// return from a crash in the decompressor.  These records deliberately remain
// active after boot: title -> demo loads are where real-hardware evidence is
// required.
extern "C" long th06_real_pbg3_load(Pbg3Archive *archive, const char *path)
    asm("__real__ZN11Pbg3Archive4LoadEPKc");
extern "C" long th06_wrap_pbg3_load(Pbg3Archive *archive, const char *path)
    asm("__wrap__ZN11Pbg3Archive4LoadEPKc");

extern "C" long th06_wrap_pbg3_load(Pbg3Archive *archive, const char *path)
{
    const bool runtime = !th06_psp_boot_phase();
    const unsigned int start = sceKernelGetSystemTimeLow();
    if (runtime)
    {
        th06_psp_runtime_progress();
        th06_psp_boot_notef("ARC LOAD BEGIN %s", path ? path : "?");
    }
    const long result = th06_real_pbg3_load(archive, path);
    if (runtime)
    {
        th06_psp_boot_notef("ARC LOAD END %s R%ld %ums", path ? path : "?", result,
                            (sceKernelGetSystemTimeLow() - start) / 1000u);
        th06_psp_runtime_progress();
    }
    return result;
}

extern "C" u8 *th06_real_pbg3_read(Pbg3Archive *archive, unsigned long entryIdx, const char *filename)
    asm("__real__ZN11Pbg3Archive19ReadDecompressEntryEmPKc");
extern "C" u8 *th06_wrap_pbg3_read(Pbg3Archive *archive, unsigned long entryIdx, const char *filename)
    asm("__wrap__ZN11Pbg3Archive19ReadDecompressEntryEmPKc");

extern "C" u8 *th06_wrap_pbg3_read(Pbg3Archive *archive, unsigned long entryIdx, const char *filename)
{
    const bool runtime = !th06_psp_boot_phase();
    const bool verbose = th06_psp_verbose_transition_diag() != 0;
    const bool resultTrace = __atomic_load_n(&gResultTransitionTrace, __ATOMIC_ACQUIRE) != 0;
    const unsigned long expected = archive ? archive->GetEntrySize(entryIdx) : 0;
    const unsigned int start = sceKernelGetSystemTimeLow();
    if (runtime && filename && std::strcmp(filename, "demo00.rpy") == 0)
    {
        gDemoLoadStartUs = start;
        gDemoAssetCount = 0;
        gDemoAssetBytes = 0;
        __atomic_store_n(&gTransitionTraceMarkers, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&gTransitionTraceBudget, verbose ? 8u : 0u, __ATOMIC_RELEASE);
        th06_psp_boot_note("DEMO LOAD BEGIN");
    }
    if (runtime && gDemoLoadStartUs != 0)
    {
        ++gDemoAssetCount;
        gDemoAssetBytes += static_cast<unsigned int>(expected);
    }
    if (runtime && (verbose || resultTrace))
    {
        th06_psp_runtime_progress();
        th06_psp_boot_notef("ARC BEGIN %s %luKB", filename ? filename : "?", expected / 1024u);
    }
    u8 *result = th06_real_pbg3_read(archive, entryIdx, filename);
    if (runtime)
        th06_psp_runtime_progress();
    if (runtime && (verbose || resultTrace))
    {
        th06_psp_boot_notef("ARC END %s %s %ums", filename ? filename : "?", result ? "OK" : "NG",
                            (sceKernelGetSystemTimeLow() - start) / 1000u);
    }
    return result;
}

extern "C" int th06_psp_verbose_transition_diag()
{
#if defined(TH06_PSP_VERBOSE_TRANSITION_DIAG)
    return 1;
#else
    return 0;
#endif
}

extern "C" int th06_psp_transition_trace_take(unsigned int marker)
{
    if (__atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) == 0)
        return 0;
    const unsigned int previous = __atomic_fetch_or(&gTransitionTraceMarkers, marker, __ATOMIC_ACQ_REL);
    return (previous & marker) == 0;
}

extern "C" void th06_psp_cpu_perf_window(unsigned long long *calcUs, unsigned long long *drawUs,
                                          unsigned int *calcCalls, unsigned int *drawCalls)
{
    if (calcUs)
        *calcUs = gPerfCalcUs;
    if (drawUs)
        *drawUs = gPerfDrawUs;
    if (calcCalls)
        *calcCalls = gPerfCalcCalls;
    if (drawCalls)
        *drawCalls = gPerfDrawCalls;
    gPerfCalcUs = 0;
    gPerfDrawUs = 0;
    gPerfCalcCalls = 0;
    gPerfDrawCalls = 0;
}

extern "C" void th06_psp_cpu_perf_flight_window(unsigned long long *calcUs, unsigned long long *drawUs)
{
    if (calcUs)
        *calcUs = gPerfFlightCalcUs;
    if (drawUs)
        *drawUs = gPerfFlightDrawUs;
    gPerfFlightCalcUs = 0;
    gPerfFlightDrawUs = 0;
}

extern "C" int th06_real_run_calc_chain(Chain *chain) asm("__real__ZN5Chain12RunCalcChainEv");
extern "C" int th06_wrap_run_calc_chain(Chain *chain) asm("__wrap__ZN5Chain12RunCalcChainEv");

extern "C" int th06_wrap_run_calc_chain(Chain *chain)
{
    const unsigned long long perfStartUs = sceKernelGetSystemTimeWide();
    const bool enteredWithTrace = __atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0;
    if (enteredWithTrace)
        th06_psp_boot_note("CHAIN CALC BEGIN");
    const int result = th06_real_run_calc_chain(chain);
    // The trace is armed from inside the title callback in this same call.
    // CALC END as the first chain marker proves all asset registration returned.
    if (__atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0)
    {
        th06_psp_boot_notef("CHAIN CALC END R%d", result);
        __atomic_sub_fetch(&gTransitionTraceBudget, 1, __ATOMIC_ACQ_REL);
    }
    if (BossDeathTracing())
    {
        th06_psp_boot_notef("BOSS DEATH CALC END R%d", result);
        __atomic_sub_fetch(&gBossDeathTraceFrames, 1, __ATOMIC_ACQ_REL);
    }
    const unsigned long long elapsedUs = sceKernelGetSystemTimeWide() - perfStartUs;
    gPerfCalcUs += elapsedUs;
    gPerfFlightCalcUs += elapsedUs;
    ++gPerfCalcCalls;
    return result;
}

extern "C" int th06_real_run_draw_chain(Chain *chain) asm("__real__ZN5Chain12RunDrawChainEv");
extern "C" int th06_wrap_run_draw_chain(Chain *chain) asm("__wrap__ZN5Chain12RunDrawChainEv");

extern "C" int th06_wrap_run_draw_chain(Chain *chain)
{
    const unsigned long long perfStartUs = sceKernelGetSystemTimeWide();
    const bool trace = __atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0;
    const bool bossDeathTrace = BossDeathTracing();
    if (trace)
        th06_psp_boot_note("CHAIN DRAW BEGIN");
    if (bossDeathTrace)
        th06_psp_boot_note("BOSS DEATH DRAW BEGIN");
    const int result = th06_real_run_draw_chain(chain);
    if (bossDeathTrace)
        th06_psp_boot_notef("BOSS DEATH DRAW END R%d", result);
    if (trace)
    {
        th06_psp_boot_notef("CHAIN DRAW END R%d", result);
        __atomic_sub_fetch(&gTransitionTraceBudget, 1, __ATOMIC_ACQ_REL);
    }
    const unsigned long long elapsedUs = sceKernelGetSystemTimeWide() - perfStartUs;
    gPerfDrawUs += elapsedUs;
    gPerfFlightDrawUs += elapsedUs;
    ++gPerfDrawCalls;
    return result;
}

extern "C" long th06_real_gui_register_chain() asm("__real__ZN3Gui13RegisterChainEv");
extern "C" long th06_wrap_gui_register_chain() asm("__wrap__ZN3Gui13RegisterChainEv");

extern "C" long th06_wrap_gui_register_chain()
{
    const bool trace = __atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0;
    if (trace)
    {
        th06_psp_runtime_progress();
        th06_psp_boot_notef("GUI REGISTER BEGIN S%ldKB", CurrentStackFreeKb());
    }
    const long result = th06_real_gui_register_chain();
    if (trace)
    {
        th06_psp_boot_notef("GUI REGISTER END R%ld S%ldKB", result, CurrentStackFreeKb());
        th06_psp_runtime_progress();
    }
    return result;
}

// Stage-end is signalled inside Gui::OnUpdate, before Supervisor tears down
// the old GameManager on its next update.  Record that boundary so a death at
// the boss animation and a death in stage construction cannot be conflated.
extern "C" unsigned int th06_real_gui_update(Gui *gui) asm("__real__ZN3Gui8OnUpdateEPS_");
extern "C" unsigned int th06_wrap_gui_update(Gui *gui) asm("__wrap__ZN3Gui8OnUpdateEPS_");

extern "C" unsigned int th06_wrap_gui_update(Gui *gui)
{
#if defined(TH06_PSP_STAGE_SOAK) || defined(TH06_PSP_RECORD_SOAK)
    // Replay capture does not guarantee that port-side dialogue waits are
    // dismissed.  This input is injected after Player/Enemy updates and only
    // for the PPSSPP soak executable, so it advances text without changing
    // the recorded combat path.
    g_LastFrameInput &= static_cast<unsigned short>(~TH_BUTTON_SHOOT);
    g_CurFrameInput |= TH_BUTTON_SHOOT | TH_BUTTON_SKIP;
#endif
    const int before = g_Supervisor.curState;
    const unsigned int result = th06_real_gui_update(gui);
    if (before != SUPERVISOR_STATE_GAMEMANAGER_REINIT &&
        g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER_REINIT)
    {
        const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
        th06_psp_boot_notef("STAGE END SIGNAL OLD%ld S%ldKB H%u/%uKB", g_GameManager.currentStage,
                            CurrentStackFreeKb(), heap.usedBytes / 1024u, heap.freeBytes / 1024u);
        th06_psp_runtime_progress();
    }
    return result;
}

// Effect 13--15 normalizes its ECL-provided axis twice.  x86/D3DX tolerated
// zero and Z-parallel degenerate axes, but the real PSP traps in the divide.
// Boss spell/death cleanup is the first common place this is exposed.  Repair
// only the mathematically singular inputs and leave every valid effect axis
// byte-for-byte untouched.
extern "C" long th06_real_effect_update4(Effect *effect)
    asm("__real__ZN13EffectManager21EffectUpdateCallback4EP6Effect");
extern "C" long th06_wrap_effect_update4(Effect *effect)
    asm("__wrap__ZN13EffectManager21EffectUpdateCallback4EP6Effect");

extern "C" long th06_wrap_effect_update4(Effect *effect)
{
    const float lengthSquared = effect->pos2.x * effect->pos2.x + effect->pos2.y * effect->pos2.y +
                                effect->pos2.z * effect->pos2.z;
    const float crossLengthSquared = effect->pos2.x * effect->pos2.x + effect->pos2.y * effect->pos2.y;
    if (!(lengthSquared > 0.0000001f) || !(crossLengthSquared > 0.0000001f))
    {
        static bool logged;
        effect->pos2.x = 1.0f;
        effect->pos2.y = 0.0f;
        effect->pos2.z = 0.0f;
        if (!logged && th06_psp_periodic_diag_enabled())
        {
            th06_psp_boot_note("EFFECT AXIS DEGENERATE -> 1,0,0");
            logged = true;
        }
    }
    return th06_real_effect_update4(effect);
}

// Rumia's defeat still exits on real hardware before Gui can publish the
// stage-end state.  Arm this trace only when CalcDamageToEnemy returns a value
// that will be lethal after EnemyManager's own cap/spell-card scaling.  The
// wrappers below then leave begin/end pairs around the exact calls in the
// unchanged engine death path.  Normal hits and normal frames do no I/O.
extern "C" long th06_real_player_damage(Player *player, const ZunVec3 *enemyPos,
                                         const ZunVec3 *enemySize, bool *bombLaser)
    asm("__real__ZN6Player17CalcDamageToEnemyEPK7ZunVec3S2_Pb");
extern "C" long th06_wrap_player_damage(Player *player, const ZunVec3 *enemyPos,
                                         const ZunVec3 *enemySize, bool *bombLaser)
    asm("__wrap__ZN6Player17CalcDamageToEnemyEPK7ZunVec3S2_Pb");

extern "C" long th06_wrap_player_damage(Player *player, const ZunVec3 *enemyPos,
                                         const ZunVec3 *enemySize, bool *bombLaser)
{
    const long rawDamage = th06_real_player_damage(player, enemyPos, enemySize, bombLaser);
    // Fine-grained death tracing performs synchronous Memory Stick writes and
    // is never part of the normal executable path.  It is available only in
    // an explicitly marked diagnostic run.
    if (BossDeathTraceArmingEnabled() && !BossDeathTracing() && rawDamage > 0)
    {
        for (int bossIdx = 0; bossIdx < 8; ++bossIdx)
        {
            Enemy *const boss = g_EnemyManager.bosses[bossIdx];
            // EnemyManager only subtracts the returned damage while unk10 is
            // set.  Boss scripts deliberately hold life at 1 with unk10=0
            // during invulnerable transitions; treating those hits as lethal
            // re-armed the file trace on virtually every shot.
            if (boss == nullptr || !boss->flags.active || !boss->flags.unk6 || !boss->flags.unk10 ||
                enemyPos != &boss->position || boss->life <= 0)
                continue;

            long effectiveDamage = rawDamage >= 70 ? 70 : rawDamage;
            if (g_EnemyManager.spellcardInfo.isActive != 0)
            {
                if (bombLaser == nullptr || !*bombLaser)
                    effectiveDamage = effectiveDamage > 7 ? effectiveDamage / 7 : (effectiveDamage ? 1 : 0);
                else if (g_EnemyManager.spellcardInfo.usedBomb != 0)
                    effectiveDamage = effectiveDamage > 3 ? effectiveDamage / 3 : (effectiveDamage ? 1 : 0);
                else
                    effectiveDamage = 0;
            }
            if (effectiveDamage < boss->life)
                continue;

            __atomic_store_n(&gBossDeathTraceFrames, 2, __ATOMIC_RELEASE);
            th06_psp_boot_notef("BOSS LETHAL I%d LIFE%ld RAW%ld EFF%ld F%u", bossIdx,
                                static_cast<long>(boss->life), rawDamage, effectiveDamage,
                                static_cast<unsigned int>(g_EnemyManager.timelineTime.AsFrames()));
            break;
        }
    }
    return rawDamage;
}

extern "C" Effect *th06_real_spawn_particles(EffectManager *manager, long effectIdx,
                                               const ZunVec3 *position, long count,
                                               unsigned long color)
    asm("__real__ZN13EffectManager14SpawnParticlesElPK7ZunVec3lm");
extern "C" Effect *th06_wrap_spawn_particles(EffectManager *manager, long effectIdx,
                                               const ZunVec3 *position, long count,
                                               unsigned long color)
    asm("__wrap__ZN13EffectManager14SpawnParticlesElPK7ZunVec3lm");

extern "C" Effect *th06_wrap_spawn_particles(EffectManager *manager, long effectIdx,
                                               const ZunVec3 *position, long count,
                                               unsigned long color)
{
    const bool trace = BossDeathTracing();
    if (trace)
        th06_psp_boot_notef("BOSS FX BEGIN I%ld N%ld A%ld", effectIdx, count,
                            static_cast<long>(manager ? manager->activeEffects : -1));
    Effect *const result = th06_real_spawn_particles(manager, effectIdx, position, count, color);

    // --wrap redirects undefined references, but g_Effects lives in the same
    // EffectManager.o as EffectUpdateCallback4 and therefore stores the real
    // function address directly.  Patch only the three callback-4 effect IDs
    // after creation so the PSP singular-axis guard is actually reached.
    if (manager != nullptr && effectIdx >= 13 && effectIdx <= 15)
    {
        const EffectUpdateCallback realCallback =
            reinterpret_cast<EffectUpdateCallback>(th06_real_effect_update4);
        const EffectUpdateCallback safeCallback =
            reinterpret_cast<EffectUpdateCallback>(th06_wrap_effect_update4);
        unsigned int patched = 0;
        for (int idx = 0; idx < 512; ++idx)
        {
            Effect &effect = manager->effects[idx];
            if (effect.inUseFlag && effect.effectId == effectIdx && effect.updateCallback == realCallback)
            {
                effect.updateCallback = safeCallback;
                ++patched;
            }
        }
        static bool callbackPatchLogged;
        if (patched != 0 && !callbackPatchLogged && th06_psp_periodic_diag_enabled())
        {
            th06_psp_boot_notef("EFFECT CALLBACK4 PATCH I%ld N%u", effectIdx, patched);
            callbackPatchLogged = true;
        }
    }
    if (trace)
        th06_psp_boot_notef("BOSS FX END P%p", result);
    return result;
}

extern "C" long th06_real_despawn_bullets(BulletManager *manager, long maxScore, bool awardPoints)
    asm("__real__ZN13BulletManager14DespawnBulletsElb");
extern "C" long th06_wrap_despawn_bullets(BulletManager *manager, long maxScore, bool awardPoints)
    asm("__wrap__ZN13BulletManager14DespawnBulletsElb");

extern "C" long th06_wrap_despawn_bullets(BulletManager *manager, long maxScore, bool awardPoints)
{
    const bool trace = BossDeathTracing();
    if (trace)
        th06_psp_boot_notef("BOSS BULLETS BEGIN M%ld A%d", maxScore, awardPoints ? 1 : 0);

    // A dense cancel can create one four/five-digit popup for every one of
    // 640 bullets, and every digit remains drawable for 60 frames.  The
    // aggregate bonus and all point items are handled independently, so cap
    // only these redundant individual labels on PSP.  This removes thousands
    // of transformed vertices without changing score, items, or bullet state.
    unsigned int activeBullets = 0;
    for (unsigned int i = 0; i < sizeof(manager->bullets) / sizeof(manager->bullets[0]); ++i)
        if (manager->bullets[i].state != 0)
            ++activeBullets;
    constexpr unsigned int kMassPopupLimit = 48;
    gMassBulletPopupStride = activeBullets > kMassPopupLimit
                                 ? (activeBullets + kMassPopupLimit - 1u) / kMassPopupLimit
                                 : 1u;
    gMassBulletPopupIndex = 0;
    const long result = th06_real_despawn_bullets(manager, maxScore, awardPoints);
    gMassBulletPopupStride = 0;
    if (trace)
        th06_psp_boot_notef("BOSS BULLETS END R%ld", result);
    return result;
}

extern "C" void th06_real_create_popup1(AsciiManager *manager, const ZunVec3 *position, long value,
                                           unsigned long color)
    asm("__real__ZN12AsciiManager12CreatePopup1EPK7ZunVec3lm");
extern "C" void th06_wrap_create_popup1(AsciiManager *manager, const ZunVec3 *position, long value,
                                           unsigned long color)
    asm("__wrap__ZN12AsciiManager12CreatePopup1EPK7ZunVec3lm");

extern "C" void th06_wrap_create_popup1(AsciiManager *manager, const ZunVec3 *position, long value,
                                           unsigned long color)
{
    const unsigned int stride = gMassBulletPopupStride;
    if (stride > 1u && (gMassBulletPopupIndex++ % stride) != 0u)
        return;
    th06_real_create_popup1(manager, position, value, color);
}

// The inherited surface helper uploads the complete SDL surface correctly,
// but always emits UVs for (0,0,w,h), ignoring the requested srcRect.  That is
// invisible for 640x480 title screens and breaks the ending's scrolling
// 640x780 staff image.  Keep the engine untouched and issue the same draw with
// the actual crop coordinates on PSP.
extern "C" void th06_wrap_copy_surface_rect(AnmManager *manager, long surfaceIdx, long dstX, long dstY,
                                               long rectLeft, long rectTop, long rectWidth, long rectHeight)
    asm("__wrap__ZN10AnmManager27CopySurfaceRectToBackBufferElllllll");

extern "C" void th06_wrap_copy_surface(AnmManager *manager, long surfaceIdx, long srcX, long srcY,
                                         long dstX, long dstY)
    asm("__wrap__ZN10AnmManager23CopySurfaceToBackBufferElllll");

extern "C" void th06_wrap_copy_surface_rect(AnmManager *manager, long surfaceIdx, long dstX, long dstY,
                                               long rectLeft, long rectTop, long rectWidth, long rectHeight)
{
    if (manager == nullptr || surfaceIdx < 0 ||
        static_cast<unsigned long>(surfaceIdx) >= sizeof(manager->surfaces) / sizeof(manager->surfaces[0]) ||
        rectWidth <= 0 || rectHeight <= 0)
        return;

    SDL_Surface *source = manager->surfaces[surfaceIdx];
    if (source == nullptr)
        return;

    ZunViewport originalViewport;
    originalViewport.Get();
    ZunViewport fullscreenViewport;
    fullscreenViewport.x = 0;
    fullscreenViewport.y = 0;
    fullscreenViewport.width = GAME_WINDOW_WIDTH;
    fullscreenViewport.height = GAME_WINDOW_HEIGHT;
    fullscreenViewport.minZ = 0.0f;
    fullscreenViewport.maxZ = 1.0f;
    fullscreenViewport.Set();

    manager->SetProjectionMode(PROJECTION_MODE_ORTHOGRAPHIC);
    manager->CreateTextureObject();

    const auto bitCeil = [](unsigned int value) {
        unsigned int result = 1;
        while (result < value)
            result <<= 1;
        return result;
    };
    const int sourceWidth = source->w;
    const int sourceHeight = source->h;
    const unsigned int textureWidth = bitCeil(static_cast<unsigned int>(sourceWidth));
    const unsigned int textureHeight = bitCeil(static_cast<unsigned int>(sourceHeight));
    g_GfxBackend->SetTextureImage(textureWidth, textureHeight, PIXEL_RGB, PIXEL_UNSIGNED_BYTE, nullptr);
    // PSP's RGB24 shim surfaces are tightly packed and remain alive for the
    // complete synchronous upload.  Feed their pixels directly to the GE
    // resampler instead of allocating and copying another 640x480x3 (900 KiB)
    // staging buffer.  Lock also supplies the stable surface serial used by
    // the GE cache; unlock restores the cached-surface height sentinel.
    SDL_LockSurface(source);
    g_GfxBackend->SetTextureSubImage(0, 0, sourceWidth, sourceHeight, source->pixels);
    SDL_UnlockSurface(source);

    VertexTex1DiffuseXyz vertices[4];
    vertices[0].position = ZunVec3(static_cast<float>(dstX), static_cast<float>(dstY), 0.0f);
    vertices[1].position = ZunVec3(static_cast<float>(dstX + rectWidth), static_cast<float>(dstY), 0.0f);
    vertices[2].position = ZunVec3(static_cast<float>(dstX), static_cast<float>(dstY + rectHeight), 0.0f);
    vertices[3].position = ZunVec3(static_cast<float>(dstX + rectWidth), static_cast<float>(dstY + rectHeight), 0.0f);

    const float u0 = static_cast<float>(rectLeft) / textureWidth;
    const float v0 = static_cast<float>(rectTop) / textureHeight;
    const float u1 = static_cast<float>(rectLeft + rectWidth) / textureWidth;
    const float v1 = static_cast<float>(rectTop + rectHeight) / textureHeight;
    vertices[0].textureUV = ZunVec2(u0, v0);
    vertices[1].textureUV = ZunVec2(u1, v0);
    vertices[2].textureUV = ZunVec2(u0, v1);
    vertices[3].textureUV = ZunVec2(u1, v1);

    manager->SetVertexAttributes(VERTEX_ATTR_TEX_COORD);
    manager->SetAttributePointer(VERTEX_ARRAY_POSITION, sizeof(vertices[0]), &vertices[0].position);
    manager->SetAttributePointer(VERTEX_ARRAY_TEX_COORD, sizeof(vertices[0]), &vertices[0].textureUV);
    manager->SetColorOp(COMPONENT_ALPHA, COLOR_OP_REPLACE);
    manager->SetColorOp(COMPONENT_RGB, COLOR_OP_REPLACE);
    manager->SetDepthMask(false);
    manager->SetDepthFunc(DEPTH_FUNC_ALWAYS);
    manager->BackendDrawCall();
    manager->SetColorOp(COMPONENT_ALPHA, COLOR_OP_MODULATE);
    manager->SetColorOp(COMPONENT_RGB, COLOR_OP_MODULATE);
    g_GfxBackend->DeleteTexture(manager->currentTextureHandle);
    manager->SetCurrentSprite(nullptr);
    manager->SetCurrentTexture(0);
    manager->SetCurrentBlendMode(0xff);
    originalViewport.Set();
}

// CopySurfaceToBackBuffer and CopySurfaceRectToBackBuffer live in the same
// engine translation unit.  GNU ld --wrap cannot redirect the former's local
// call to the latter, so callers such as ResultScreen otherwise reach the
// inherited ApplySurfaceToColorBuffer path and allocate a redundant
// 640x480x3 copy every frame.  Route the public full-surface entry point to the
// PSP direct-upload implementation as well.
extern "C" void th06_wrap_copy_surface(AnmManager *manager, long surfaceIdx, long srcX, long srcY,
                                         long dstX, long dstY)
{
    if (manager == nullptr || surfaceIdx < 0 ||
        static_cast<unsigned long>(surfaceIdx) >= sizeof(manager->surfaces) / sizeof(manager->surfaces[0]))
        return;

    SDL_Surface *source = manager->surfaces[surfaceIdx];
    if (source == nullptr)
        return;

    th06_wrap_copy_surface_rect(manager, surfaceIdx, dstX, dstY, srcX, srcY,
                                 source->w - srcX, source->h - srcY);
}

extern "C" void th06_real_play_sfx(SoundPlayer *player, SoundIdx idx)
    asm("__real__ZN11SoundPlayer14PlaySoundByIdxE8SoundIdx");
extern "C" void th06_wrap_play_sfx(SoundPlayer *player, SoundIdx idx)
    asm("__wrap__ZN11SoundPlayer14PlaySoundByIdxE8SoundIdx");

extern "C" void th06_wrap_play_sfx(SoundPlayer *player, SoundIdx idx)
{
    const bool trace = BossDeathTracing();
    if (trace)
        th06_psp_boot_notef("BOSS SFX BEGIN I%d", static_cast<int>(idx));
    th06_real_play_sfx(player, idx);
    if (trace)
        th06_psp_boot_notef("BOSS SFX END I%d", static_cast<int>(idx));
}

extern "C" long th06_real_call_ecl(const EclManager *manager, EnemyEclContext *context, short subId)
    asm("__real__ZNK10EclManager10CallEclSubEP15EnemyEclContexts");
extern "C" long th06_wrap_call_ecl(const EclManager *manager, EnemyEclContext *context, short subId)
    asm("__wrap__ZNK10EclManager10CallEclSubEP15EnemyEclContexts");

extern "C" long th06_wrap_call_ecl(const EclManager *manager, EnemyEclContext *context, short subId)
{
    const bool trace = BossDeathTracing();
    if (trace)
        th06_psp_boot_notef("BOSS ECL BEGIN S%d", static_cast<int>(subId));
    const long result = th06_real_call_ecl(manager, context, subId);
    if (trace)
        th06_psp_boot_notef("BOSS ECL END R%ld", result);
    return result;
}

extern "C" void th06_real_reset_enemy_effects(Enemy *enemy)
    asm("__real__ZN5Enemy16ResetEffectArrayEPS_");
extern "C" void th06_wrap_reset_enemy_effects(Enemy *enemy)
    asm("__wrap__ZN5Enemy16ResetEffectArrayEPS_");

extern "C" void th06_wrap_reset_enemy_effects(Enemy *enemy)
{
    const bool trace = BossDeathTracing();
    if (trace)
        th06_psp_boot_notef("BOSS OWN FX RESET BEGIN N%ld",
                            enemy ? static_cast<long>(enemy->effectIdx) : -1L);
    th06_real_reset_enemy_effects(enemy);
    if (trace)
        th06_psp_boot_note("BOSS OWN FX RESET END");
}

extern "C" void th06_real_update_enemy_effects(Enemy *enemy)
    asm("__real__ZN5Enemy13UpdateEffectsEPS_");
extern "C" void th06_wrap_update_enemy_effects(Enemy *enemy)
    asm("__wrap__ZN5Enemy13UpdateEffectsEPS_");

extern "C" void th06_wrap_update_enemy_effects(Enemy *enemy)
{
    const bool trace = BossDeathTracing();
    if (trace && enemy != nullptr && enemy->flags.isBoss)
        th06_psp_boot_notef("BOSS OWN FX UPDATE BEGIN N%ld", static_cast<long>(enemy->effectIdx));
    th06_real_update_enemy_effects(enemy);
    if (trace && enemy != nullptr && enemy->flags.isBoss)
        th06_psp_boot_note("BOSS OWN FX UPDATE END");
}

extern "C" unsigned int th06_real_enemy_update(EnemyManager *manager)
    asm("__real__ZN12EnemyManager8OnUpdateEPS_");
extern "C" unsigned int th06_wrap_enemy_update(EnemyManager *manager)
    asm("__wrap__ZN12EnemyManager8OnUpdateEPS_");

extern "C" unsigned int th06_wrap_enemy_update(EnemyManager *manager)
{
    if (BossDeathTracing())
        th06_psp_boot_note("BOSS ENEMY UPDATE BEGIN");
    const unsigned int result = th06_real_enemy_update(manager);
    if (BossDeathTracing())
        th06_psp_boot_notef("BOSS ENEMY UPDATE END R%u", result);
    return result;
}

extern "C" unsigned int th06_real_effects_update(EffectManager *manager)
    asm("__real__ZN13EffectManager8OnUpdateEPS_");
extern "C" unsigned int th06_wrap_effects_update(EffectManager *manager)
    asm("__wrap__ZN13EffectManager8OnUpdateEPS_");

extern "C" unsigned int th06_wrap_effects_update(EffectManager *manager)
{
    const bool trace = BossDeathTracing();
    if (trace)
        th06_psp_boot_notef("BOSS EFFECT UPDATE BEGIN A%ld", static_cast<long>(manager->activeEffects));
    const unsigned int result = th06_real_effects_update(manager);
    if (trace)
        th06_psp_boot_notef("BOSS EFFECT UPDATE END R%u A%ld", result,
                            static_cast<long>(manager->activeEffects));
    return result;
}

extern "C" unsigned int th06_real_effects_draw(EffectManager *manager)
    asm("__real__ZN13EffectManager6OnDrawEPS_");
extern "C" unsigned int th06_wrap_effects_draw(EffectManager *manager)
    asm("__wrap__ZN13EffectManager6OnDrawEPS_");

extern "C" unsigned int th06_wrap_effects_draw(EffectManager *manager)
{
    const bool trace = BossDeathTracing();
    if (trace)
        th06_psp_boot_notef("BOSS EFFECT DRAW BEGIN A%ld", static_cast<long>(manager->activeEffects));
    const unsigned int result = th06_real_effects_draw(manager);
    if (trace)
        th06_psp_boot_notef("BOSS EFFECT DRAW END R%u", result);
    return result;
}

extern "C" void th06_real_ascii_initialize(AsciiManager *manager)
    asm("__real__ZN12AsciiManager13InitializeVmsEv");
extern "C" void th06_wrap_ascii_initialize(AsciiManager *manager)
    asm("__wrap__ZN12AsciiManager13InitializeVmsEv");

extern "C" void th06_wrap_ascii_initialize(AsciiManager *manager)
{
    const bool trace = __atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0;
    if (trace)
    {
        th06_psp_runtime_progress();
        th06_psp_boot_notef("ASCII INIT BEGIN S%ldKB", CurrentStackFreeKb());
    }
    th06_real_ascii_initialize(manager);
    if (trace)
    {
        th06_psp_boot_notef("ASCII INIT END S%ldKB", CurrentStackFreeKb());
        th06_psp_runtime_progress();
    }
}

extern "C" long th06_real_game_manager_register() asm("__real__ZN11GameManager13RegisterChainEv");
extern "C" long th06_wrap_game_manager_register() asm("__wrap__ZN11GameManager13RegisterChainEv");

extern "C" long th06_wrap_game_manager_register()
{
    // Menu/ending still images use two persistent 512 KiB resample caches.
    // GameManager registration is a synchronous ownership boundary: retire
    // entries no live texture borrows before stage assets need the RAM.
    const unsigned int surfaceCacheFreed = th06_psp_ge_trim_surface_cache();
    const bool stageReinitAttempt = g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER_REINIT;
    const bool stageReinit = __atomic_load_n(&gStageReinitTrace, __ATOMIC_ACQUIRE) != 0;
    if (stageReinit)
        th06_psp_boot_notef("STAGE REINIT REGISTER BEGIN NEXT%ld S%ldKB", g_GameManager.currentStage + 1,
                            CurrentStackFreeKb());
    const bool beginTrace = __atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0;
    if (beginTrace)
        th06_psp_boot_notef("GM REGISTER BEGIN S%ldKB", CurrentStackFreeKb());
    const long result = th06_real_game_manager_register();
    if (__atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0)
    {
        th06_psp_boot_notef("GM REGISTER END R%ld S%ldKB", result, CurrentStackFreeKb());
        th06_psp_boot_notef("GM CAMERA RAW %08X %08X %08X", FloatBits(g_GameManager.stageCameraFacingDir.x),
                            FloatBits(g_GameManager.stageCameraFacingDir.y),
                            FloatBits(g_GameManager.stageCameraFacingDir.z));
    }

    // Stage::AddedCallback establishes (0, 0, 1) as the stage camera's
    // initial direction, but stores it only in Stage.  GameManager's calc job
    // has priority 4 and therefore runs before Stage's priority-6 job can copy
    // that value into g_GameManager.  The first frame otherwise normalizes a
    // zero direction in createViewMatrix.  Synchronize only that proven
    // all-zero initial state; later scripted camera values are untouched.
    if (result == 0 && g_GameManager.stageCameraFacingDir.x == 0.0f &&
        g_GameManager.stageCameraFacingDir.y == 0.0f && g_GameManager.stageCameraFacingDir.z == 0.0f)
    {
        g_GameManager.stageCameraFacingDir.z = 1.0f;
        th06_psp_boot_note("GM CAMERA INIT 0,0,1");
    }
    if (gDemoLoadStartUs != 0)
    {
        const unsigned int elapsedMs = (sceKernelGetSystemTimeLow() - gDemoLoadStartUs) / 1000u;
        const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
        th06_psp_boot_notef("DEMO LOAD END %ums A%u %uKB X%uKB H%u/%uKB", elapsedMs, gDemoAssetCount,
                            gDemoAssetBytes / 1024u, surfaceCacheFreed / 1024u,
                            heap.usedBytes / 1024u, heap.freeBytes / 1024u);
        gDemoLoadStartUs = 0;
    }
    if (stageReinit)
    {
        th06_psp_boot_notef("STAGE REINIT REGISTER END R%ld NOW%ld S%ldKB RC%uKB", result,
                            g_GameManager.currentStage, CurrentStackFreeKb(), gReplayCompactedBytes / 1024u);
        gReplayCompactedBytes = 0;
        __atomic_store_n(&gStageReinitTrace, 0, __ATOMIC_RELEASE);
    }
    else if (stageReinitAttempt)
    {
        // One durable result is enough in the normal build.  The dozens of
        // phase records are available only with TH06PSP_DIAG.ON so ordinary
        // stage changes do not fight BGM reads on the Memory Stick.
        const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
        th06_psp_boot_notef("STAGE REINIT RESULT R%ld NOW%ld H%u/%uKB RC%uKB", result,
                            g_GameManager.currentStage, heap.usedBytes / 1024u, heap.freeBytes / 1024u,
                            gReplayCompactedBytes / 1024u);
        gReplayCompactedBytes = 0;
    }
    return result;
}

// Stage-to-stage teardown and construction run synchronously from
// Supervisor::OnUpdate.  A real-PSP crash therefore used to leave only the
// preceding PERF line.  Arm a short trace at the proven REINIT boundary and
// put a begin/end pair around every destructive or allocating phase.  The
// last surviving line identifies the failing subsystem without changing src/.
extern "C" void th06_real_game_manager_cut() asm("__real__ZN11GameManager8CutChainEv");
extern "C" void th06_wrap_game_manager_cut() asm("__wrap__ZN11GameManager8CutChainEv");

extern "C" void th06_wrap_game_manager_cut()
{
    const bool stageReinitAttempt = g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER_REINIT;
    const bool stageReinit = stageReinitAttempt && DetailedStageTransitionTracing();
    const bool resultTransition = g_Supervisor.curState == SUPERVISOR_STATE_RESULTSCREEN_FROMGAME;
    if (resultTransition)
        th06_psp_audio_diag_checkpoint("CONTNO", 1);
    else if (stageReinitAttempt)
        th06_psp_audio_diag_checkpoint("STAGE", 0);
    if (stageReinit)
    {
        __atomic_store_n(&gStageReinitTrace, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&gTransitionTraceMarkers, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&gTransitionTraceBudget, 3, __ATOMIC_RELEASE);
        th06_psp_runtime_progress();
        th06_psp_boot_notef("STAGE REINIT CUT BEGIN OLD%ld S%ldKB", g_GameManager.currentStage,
                            CurrentStackFreeKb());
    }
    else if (resultTransition)
    {
        // A game-over reaches this boundary once, after BGM shutdown. Durable
        // phase pairs are safe here and identify a hardware-only teardown
        // fault without requiring another manual run to stage 5.
        __atomic_store_n(&gResultTransitionTrace, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&gTransitionTraceMarkers, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&gTransitionTraceBudget, 3, __ATOMIC_RELEASE);
        th06_psp_runtime_progress();
        th06_psp_boot_notef("RESULT CUT BEGIN ST%ld S%ldKB", g_GameManager.currentStage,
                            CurrentStackFreeKb());
    }
    th06_real_game_manager_cut();
    if (stageReinit)
    {
        th06_psp_boot_notef("STAGE REINIT CUT END S%ldKB", CurrentStackFreeKb());
        th06_psp_runtime_progress();
    }
    else if (resultTransition)
    {
        th06_psp_boot_notef("RESULT CUT END S%ldKB", CurrentStackFreeKb());
        th06_psp_runtime_progress();
    }
}

extern "C" long th06_real_result_screen_register(long fromGame)
    asm("__real__ZN12ResultScreen13RegisterChainEl");
extern "C" long th06_wrap_result_screen_register(long fromGame)
    asm("__wrap__ZN12ResultScreen13RegisterChainEl");

extern "C" long th06_wrap_result_screen_register(long fromGame)
{
    const bool trace = __atomic_load_n(&gResultTransitionTrace, __ATOMIC_ACQUIRE) != 0;
    // Result registration is another synchronous surface-ownership boundary.
    // Retire only cache entries whose SDL surfaces are already gone before
    // result.jpg and its ANM atlases are allocated.
    const unsigned int cacheFreed = th06_psp_ge_trim_surface_cache();
    if (trace)
    {
        const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
        th06_psp_boot_notef("RESULT REGISTER BEGIN F%ld X%uKB H%u/%uKB S%ldKB", fromGame,
                            cacheFreed / 1024u, heap.usedBytes / 1024u, heap.freeBytes / 1024u,
                            CurrentStackFreeKb());
        th06_psp_runtime_progress();
    }

    const long result = th06_real_result_screen_register(fromGame);
    if (trace)
    {
        const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
        th06_psp_boot_notef("RESULT REGISTER END R%ld H%u/%uKB S%ldKB", result,
                            heap.usedBytes / 1024u, heap.freeBytes / 1024u, CurrentStackFreeKb());
        th06_psp_runtime_progress();
        __atomic_store_n(&gResultTransitionTrace, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&gTransitionTraceBudget, 0, __ATOMIC_RELEASE);
    }
    return result;
}

extern "C" long th06_real_anm_load_surface(AnmManager *manager, long index, const char *path)
    asm("__real__ZN10AnmManager11LoadSurfaceElPKc");
extern "C" long th06_wrap_anm_load_surface(AnmManager *manager, long index, const char *path)
    asm("__wrap__ZN10AnmManager11LoadSurfaceElPKc");
extern "C" SDL_Surface *th06_psp_img_load_jpeg_rgb24(const unsigned char *data, std::size_t size);

extern "C" long th06_wrap_anm_load_surface(AnmManager *manager, long index, const char *path)
{
    const bool trace = __atomic_load_n(&gResultTransitionTrace, __ATOMIC_ACQUIRE) != 0;
    if (trace)
        th06_psp_boot_notef("RESULT SURFACE BEGIN I%ld %s", index, path ? path : "?");
    long result;
    u8 *encoded = path ? FileSystem::OpenPath(path, 0) : nullptr;
    if (encoded && g_LastFileSize >= 2 && encoded[0] == 0xff && encoded[1] == 0xd8)
    {
        if (manager->surfaces[index] != nullptr)
            manager->ReleaseSurface(index);
        manager->surfaces[index] = th06_psp_img_load_jpeg_rgb24(encoded, g_LastFileSize);
        std::free(encoded);
        result = manager->surfaces[index] ? 0 : -1;
    }
    else
    {
        std::free(encoded);
        result = th06_real_anm_load_surface(manager, index, path);
    }
    if (trace)
    {
        const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
        th06_psp_boot_notef("RESULT SURFACE END R%ld H%u/%uKB", result,
                            heap.usedBytes / 1024u, heap.freeBytes / 1024u);
    }
    return result;
}

extern "C" long th06_real_anm_load_anm(AnmManager *manager, long index, const char *path, long spriteOffset)
    asm("__real__ZN10AnmManager7LoadAnmElPKcl");
extern "C" long th06_wrap_anm_load_anm(AnmManager *manager, long index, const char *path, long spriteOffset)
    asm("__wrap__ZN10AnmManager7LoadAnmElPKcl");

extern "C" long th06_wrap_anm_load_anm(AnmManager *manager, long index, const char *path, long spriteOffset)
{
    const bool trace = __atomic_load_n(&gResultTransitionTrace, __ATOMIC_ACQUIRE) != 0;
    if (trace)
        th06_psp_boot_notef("RESULT ANM BEGIN I%ld O%ld %s", index, spriteOffset, path ? path : "?");
    const long result = th06_real_anm_load_anm(manager, index, path, spriteOffset);
    if (trace)
    {
        const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
        th06_psp_boot_notef("RESULT ANM END I%ld R%ld H%u/%uKB", index, result,
                            heap.usedBytes / 1024u, heap.freeBytes / 1024u);
    }
    return result;
}

#define TH06_WRAP_STAGE_VOID(tag, id)                                                                                 \
    extern "C" void th06_real_##id() asm("__real_" tag);                                                            \
    extern "C" void th06_wrap_##id() asm("__wrap_" tag);                                                           \
    extern "C" void th06_wrap_##id()                                                                                \
    {                                                                                                                 \
        const bool trace = GameTeardownTracing();                                                                     \
        if (trace)                                                                                                    \
            th06_psp_boot_note(#id " BEGIN");                                                                        \
        th06_real_##id();                                                                                             \
        if (trace)                                                                                                    \
            th06_psp_boot_notef(#id " END S%ldKB", CurrentStackFreeKb());                                            \
    }

TH06_WRAP_STAGE_VOID("_ZN5Stage8CutChainEv", cut_stage)
TH06_WRAP_STAGE_VOID("_ZN13BulletManager8CutChainEv", cut_bullets)
TH06_WRAP_STAGE_VOID("_ZN6Player8CutChainEv", cut_player)
TH06_WRAP_STAGE_VOID("_ZN12EnemyManager8CutChainEv", cut_enemies)
TH06_WRAP_STAGE_VOID("_ZN13EffectManager8CutChainEv", cut_effects)
TH06_WRAP_STAGE_VOID("_ZN3Gui8CutChainEv", cut_gui)

#undef TH06_WRAP_STAGE_VOID

extern "C" void th06_real_stop_replay() asm("__real__ZN13ReplayManager13StopRecordingEv");
extern "C" void th06_wrap_stop_replay() asm("__wrap__ZN13ReplayManager13StopRecordingEv");

extern "C" void th06_wrap_stop_replay()
{
    const bool trace = GameTeardownTracing();
    if (trace)
        th06_psp_boot_note("stop_replay BEGIN");

    gReplayCompactedBytes = 0;
    ReplayManager *manager = g_ReplayManager;
    const int stageIndex = g_GameManager.currentStage - 1;
    StageReplayData *stageBefore = nullptr;
    if (manager != nullptr && !manager->IsDemo() && manager->replayData != nullptr && stageIndex >= 0 &&
        stageIndex < 7)
    {
        stageBefore = manager->replayData->stageReplayData[stageIndex];
        // Game teardown terminates this stage before the result screen.
        // SaveReplay terminates it again after name entry. The first PSP call
        // has already realloc-shrunk the buffer, so the second terminator pair
        // would be written beyond the real allocation while the engine's
        // sizeof-based bound still appears valid.
        if (stageBefore != nullptr && gReplayStoppedStages[stageIndex] == stageBefore)
        {
            th06_psp_boot_notef("REPLAY STOP SKIP ST%d", stageIndex + 1);
            return;
        }
    }

    th06_real_stop_replay();

    if (manager != nullptr && !manager->IsDemo() && manager->replayData != nullptr && stageIndex >= 0 &&
        stageIndex < 7)
    {
        StageReplayData *stage = manager->replayData->stageReplayData[stageIndex];
        const ReplayDataInput *bookmark = manager->replayInputStageBookmarks[stageIndex];
        if (stage != nullptr && bookmark != nullptr)
        {
            const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(stage);
            const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(bookmark);
            const std::uintptr_t replayInput = reinterpret_cast<std::uintptr_t>(manager->replayInputs);
            const std::size_t minimumBytes = offsetof(StageReplayData, replayInputs);
            if (end >= begin + minimumBytes && end <= begin + sizeof(StageReplayData) &&
                replayInput >= begin && replayInput < end)
            {
                // SaveReplay() calls StopRecording() a second time after the
                // player names the replay.  Calls within ReplayManager.cpp
                // bypass the linker wrapper, so a previously realloc-shrunk
                // record would receive that second terminator past its true
                // allocation.  Retain the original allocation until the
                // save completes; the final cleanup then releases it.
                gReplayStoppedStages[stageIndex] = stage;
            }
        }
    }

    if (trace)
        th06_psp_boot_notef("stop_replay END S%ldKB RC%uKB", CurrentStackFreeKb(),
                            gReplayCompactedBytes / 1024u);
}

extern "C" void th06_wrap_ecl_unload(EclManager *manager) asm("__wrap__ZN10EclManager6UnloadEv");

extern "C" void th06_wrap_ecl_unload(EclManager *manager)
{
    const bool trace = GameTeardownTracing();
    if (trace)
        th06_psp_boot_note("unload_ecl BEGIN");

    // The storage is static.  Only revoke every engine-visible pointer; the
    // next Load() overwrites the same arena and rebuilds the pointer table.
    manager->eclFile = nullptr;
    manager->timelinePtrs[0] = nullptr;
    manager->timelinePtrs[1] = nullptr;
    manager->timelinePtrs[2] = nullptr;
    manager->subTable = nullptr;
    manager->timeline = nullptr;
    gPspEclLoadedBytes = 0;

    if (trace)
        th06_psp_boot_notef("unload_ecl END S%ldKB", CurrentStackFreeKb());
}

extern "C" void th06_real_supervisor_stop_audio(Supervisor *supervisor)
    asm("__real__ZN10Supervisor9StopAudioEv");
extern "C" void th06_wrap_supervisor_stop_audio(Supervisor *supervisor)
    asm("__wrap__ZN10Supervisor9StopAudioEv");

extern "C" void th06_wrap_supervisor_stop_audio(Supervisor *supervisor)
{
    const bool trace = GameTeardownTracing();
    if (trace)
        th06_psp_boot_note("STOP AUDIO BEGIN");
    th06_real_supervisor_stop_audio(supervisor);
    if (trace)
        th06_psp_boot_notef("STOP AUDIO END S%ldKB", CurrentStackFreeKb());
}

#define TH06_TRACE_STAGE_REGISTER_BEGIN(name)                                                                         \
    const bool trace = GameTeardownTracing();                                                                         \
    if (trace)                                                                                                        \
        th06_psp_boot_note(name " BEGIN")
#define TH06_TRACE_STAGE_REGISTER_END(name, result)                                                                   \
    if (trace)                                                                                                        \
        th06_psp_boot_notef(name " END R%ld S%ldKB", static_cast<long>(result), CurrentStackFreeKb())

extern "C" long th06_real_stage_register(unsigned long stage) asm("__real__ZN5Stage13RegisterChainEm");
extern "C" long th06_wrap_stage_register(unsigned long stage) asm("__wrap__ZN5Stage13RegisterChainEm");
extern "C" long th06_wrap_stage_register(unsigned long stage)
{
    TH06_TRACE_STAGE_REGISTER_BEGIN("ADD STAGE");
    const long result = th06_real_stage_register(stage);
    TH06_TRACE_STAGE_REGISTER_END("ADD STAGE", result);
    return result;
}

extern "C" long th06_real_player_register(unsigned char value) asm("__real__ZN6Player13RegisterChainEh");
extern "C" long th06_wrap_player_register(unsigned char value) asm("__wrap__ZN6Player13RegisterChainEh");
extern "C" long th06_wrap_player_register(unsigned char value)
{
    TH06_TRACE_STAGE_REGISTER_BEGIN("ADD PLAYER");
    const long result = th06_real_player_register(value);
    TH06_TRACE_STAGE_REGISTER_END("ADD PLAYER", result);
    return result;
}

extern "C" long th06_real_bullets_register(const char *path) asm("__real__ZN13BulletManager13RegisterChainEPKc");
extern "C" long th06_wrap_bullets_register(const char *path) asm("__wrap__ZN13BulletManager13RegisterChainEPKc");
extern "C" long th06_wrap_bullets_register(const char *path)
{
    TH06_TRACE_STAGE_REGISTER_BEGIN("ADD BULLETS");
    const long result = th06_real_bullets_register(path);
    TH06_TRACE_STAGE_REGISTER_END("ADD BULLETS", result);
    return result;
}

extern "C" long th06_real_enemies_register(const char *first, const char *second)
    asm("__real__ZN12EnemyManager13RegisterChainEPKcS1_");
extern "C" long th06_wrap_enemies_register(const char *first, const char *second)
    asm("__wrap__ZN12EnemyManager13RegisterChainEPKcS1_");
extern "C" long th06_wrap_enemies_register(const char *first, const char *second)
{
    TH06_TRACE_STAGE_REGISTER_BEGIN("ADD ENEMIES");
    const long result = th06_real_enemies_register(first, second);
    TH06_TRACE_STAGE_REGISTER_END("ADD ENEMIES", result);
    return result;
}

extern "C" long th06_wrap_ecl_load(EclManager *manager, const char *path) asm("__wrap__ZN10EclManager4LoadEPKc");
extern "C" long th06_wrap_ecl_load(EclManager *manager, const char *path)
{
    TH06_TRACE_STAGE_REGISTER_BEGIN("ADD ECL");

    manager->eclFile = nullptr;
    manager->timelinePtrs[0] = nullptr;
    manager->timelinePtrs[1] = nullptr;
    manager->timelinePtrs[2] = nullptr;
    manager->subTable = nullptr;
    manager->timeline = nullptr;
    gPspEclLoadedBytes = 0;

    unsigned char *source = FileSystem::OpenPath(path, false);
    const unsigned int sourceBytes = g_LastFileSize;
    long result = ZUN_ERROR;

    if (source != nullptr && sourceBytes >= sizeof(EclRawHeader) && sourceBytes <= kPspEclArenaBytes)
    {
        const EclRawHeader *sourceHeader = reinterpret_cast<const EclRawHeader *>(source);
        const int subCount = sourceHeader->subCount;
        const std::size_t headerBytes = sizeof(EclRawHeader) + static_cast<std::size_t>(subCount > 0 ? subCount : 0) *
                                                                   sizeof(LE<u32>);

        bool valid = subCount > 0 && subCount <= kPspEclMaxSubs && headerBytes <= sourceBytes;
        for (int i = 0; valid && i < 3; ++i)
            valid = static_cast<unsigned int>(sourceHeader->timelineOffsets[i]) < sourceBytes;
        for (int i = 0; valid && i < subCount; ++i)
            valid = static_cast<unsigned int>(sourceHeader->subOffsets[i]) < sourceBytes;

        if (valid)
        {
            std::memcpy(gPspEclArena, source, sourceBytes);
            EclRawHeader *header = reinterpret_cast<EclRawHeader *>(gPspEclArena);
            manager->eclFile = header;
            for (int i = 0; i < 3; ++i)
            {
                manager->timelinePtrs[i] = reinterpret_cast<EclTimelineInstr *>(
                    gPspEclArena + static_cast<unsigned int>(header->timelineOffsets[i]));
            }
            for (int i = 0; i < subCount; ++i)
            {
                gPspEclSubTable[i] = reinterpret_cast<EclRawInstr *>(
                    gPspEclArena + static_cast<unsigned int>(header->subOffsets[i]));
            }
            manager->subTable = gPspEclSubTable;
            manager->timeline = manager->timelinePtrs[0];
            gPspEclLoadedBytes = sourceBytes;
            result = ZUN_SUCCESS;
            th06_psp_boot_notef("ECL ARENA LOAD B%u N%d", sourceBytes, subCount);
        }
    }

    std::free(source);
    if (result != ZUN_SUCCESS)
        th06_psp_boot_notef("ECL ARENA REJECT B%u", sourceBytes);

    TH06_TRACE_STAGE_REGISTER_END("ADD ECL", result);
    return result;
}

extern "C" long th06_real_effects_register() asm("__real__ZN13EffectManager13RegisterChainEv");
extern "C" long th06_wrap_effects_register() asm("__wrap__ZN13EffectManager13RegisterChainEv");
extern "C" long th06_wrap_effects_register()
{
    TH06_TRACE_STAGE_REGISTER_BEGIN("ADD EFFECTS");
    const long result = th06_real_effects_register();
    TH06_TRACE_STAGE_REGISTER_END("ADD EFFECTS", result);
    return result;
}

extern "C" long th06_real_replay_register(long demo, const char *path)
    asm("__real__ZN13ReplayManager13RegisterChainElPKc");
extern "C" long th06_wrap_replay_register(long demo, const char *path)
    asm("__wrap__ZN13ReplayManager13RegisterChainElPKc");
extern "C" long th06_wrap_replay_register(long demo, const char *path)
{
    const bool newRecordingSession = g_ReplayManager == nullptr && demo == 0;
    TH06_TRACE_STAGE_REGISTER_BEGIN("ADD REPLAY");
    const long result = th06_real_replay_register(demo, path);
    if (newRecordingSession)
        std::memset(gReplayStoppedStages, 0, sizeof(gReplayStoppedStages));
    TH06_TRACE_STAGE_REGISTER_END("ADD REPLAY", result);
    return result;
}

extern "C" void th06_real_save_replay(const char *path, char *name)
    asm("__real__ZN13ReplayManager10SaveReplayEPKcPc");
extern "C" void th06_wrap_save_replay(const char *path, char *name)
    asm("__wrap__ZN13ReplayManager10SaveReplayEPKcPc");

extern "C" void th06_wrap_save_replay(const char *path, char *name)
{
    th06_psp_boot_notef("REPLAY SAVE BEGIN %s", path != nullptr ? path : "(null)");
    th06_real_save_replay(path, name);
    th06_psp_boot_note("REPLAY SAVE END");
}

#undef TH06_TRACE_STAGE_REGISTER_BEGIN
#undef TH06_TRACE_STAGE_REGISTER_END

// Controller::GetInput inherited the PS3-only refresh guard.  On PSP,
// ResetKeyboard retained a pointer to SDL's static state but nothing ever
// polled the pad into that array again.  Refresh it once immediately before
// every engine input sample, then let the unchanged engine mapping consume it.
extern "C" unsigned short th06_real_controller_get_input() asm("__real__ZN10Controller8GetInputEv");
extern "C" unsigned short th06_wrap_controller_get_input() asm("__wrap__ZN10Controller8GetInputEv");

extern "C" unsigned short th06_wrap_controller_get_input()
{
    SDL_GetKeyboardState(nullptr);
    return th06_real_controller_get_input();
}

// The development build deliberately has no PSP MIDI synthesizer or external
// MIDI device.  The inherited options menu still exposes MIDI and immediately
// asks Supervisor to parse/play it; letting that unsupported path proceed can
// fault before MidiDefault reports that no device exists.  Keep the choice
// visible for future implementation work, but make every attempted selection
// fall back to the proven WAV streamer instead of crashing or persisting an
// unbootable setting.
extern "C" int th06_real_supervisor_load_config(Supervisor *supervisor, const char *path)
    asm("__real__ZN10Supervisor10LoadConfigEPKc");
extern "C" int th06_wrap_supervisor_load_config(Supervisor *supervisor, const char *path)
    asm("__wrap__ZN10Supervisor10LoadConfigEPKc");

extern "C" int th06_wrap_supervisor_load_config(Supervisor *supervisor, const char *path)
{
    const int result = th06_real_supervisor_load_config(supervisor, path);
    if (supervisor->cfg.musicMode == MIDI)
    {
        supervisor->cfg.musicMode = WAV;
        th06_psp_boot_note("PSP MIDI UNSUPPORTED -> WAV");
    }
    return result;
}

extern "C" int th06_real_supervisor_play_audio(Supervisor *supervisor, const char *path)
    asm("__real__ZN10Supervisor9PlayAudioEPKc");
extern "C" int th06_wrap_supervisor_play_audio(Supervisor *supervisor, const char *path)
    asm("__wrap__ZN10Supervisor9PlayAudioEPKc");

extern "C" int th06_wrap_supervisor_play_audio(Supervisor *supervisor, const char *path)
{
    if (supervisor->cfg.musicMode == MIDI)
    {
        supervisor->cfg.musicMode = WAV;
        th06_psp_boot_note("PSP MIDI SELECT -> WAV");
    }
    return th06_real_supervisor_play_audio(supervisor, path);
}

extern "C" unsigned int th06_real_main_menu_update_options(MainMenu *menu)
    asm("__real__ZN8MainMenu19OnUpdateOptionsMenuEv");
extern "C" unsigned int th06_wrap_main_menu_update_options(MainMenu *menu)
    asm("__wrap__ZN8MainMenu19OnUpdateOptionsMenuEv");

extern "C" unsigned int th06_wrap_main_menu_update_options(MainMenu *menu)
{
    const unsigned int result = th06_real_main_menu_update_options(menu);

    // The inherited bitmap labels say FULLSCREEN/WINDOWED, which describe a
    // PC window rather than the two PSP output layouts.  Add the actual mode
    // name beside that row while preserving the original option mechanics.
    ZunVec3 position = menu->vm[75].pos + menu->vm[75].posOffset;
    position.y += 18.0f;
    position.z = 0.1f;
    const ZunColor savedColor = g_AsciiManager.color;
    const ZunVec2 savedScale = g_AsciiManager.scale;
    const bool savedSelected = g_AsciiManager.isSelected;
    g_AsciiManager.color = menu->cursor == CURSOR_OPTIONS_POS_SCREENMODE ? 0xffffff80u : 0xffffffffu;
    g_AsciiManager.scale = ZunVec2(0.75f, 0.75f);
    g_AsciiManager.isSelected = false;
    g_AsciiManager.AddString(&position, menu->windowed ? "4:3 FIT" : "FULL STRETCH");
    g_AsciiManager.color = savedColor;
    g_AsciiManager.scale = savedScale;
    g_AsciiManager.isSelected = savedSelected;
    return result;
}

extern "C" long th06_real_main_menu_register(unsigned long isDemo)
    asm("__real__ZN8MainMenu13RegisterChainEm");
extern "C" long th06_wrap_main_menu_register(unsigned long isDemo)
    asm("__wrap__ZN8MainMenu13RegisterChainEm");

extern "C" long th06_wrap_main_menu_register(unsigned long isDemo)
{
    const bool runtime = !th06_psp_boot_phase();
    const unsigned int start = sceKernelGetSystemTimeLow();
    const long result = th06_real_main_menu_register(isDemo);
    if (runtime)
        th06_psp_boot_notef("TITLE LOAD END %ums R%ld", (sceKernelGetSystemTimeLow() - start) / 1000u, result);
    return result;
}

extern "C" unsigned int th06_psp_real_main_menu_update(MainMenu *menu)
    asm("_ZN8MainMenu8OnUpdateEPS_");
extern "C" unsigned int th06_psp_real_player_update(Player *player)
    asm("_ZN6Player8OnUpdateEPS_");

extern "C" unsigned int th06_psp_player_update(Player *player)
{
    StabilizeReimuAHomingBullets(player);
    const int playerStateBefore =
        player != nullptr ? static_cast<int>(player->playerState) : static_cast<int>(PLAYER_STATE_ALIVE);
#if defined(TH06_PSP_REPLAY_TRACE)
    // Bounded snapshots make the captured input timeline comparable between
    // PPSSPP and hardware without turning the 60 Hz path into FAT I/O.
    static unsigned int nextSnapshot = 600;
    if (g_ReplayManager != nullptr && static_cast<unsigned int>(g_ReplayManager->frameId) >= nextSnapshot)
    {
        const unsigned int replayFrame = static_cast<unsigned int>(g_ReplayManager->frameId);
        Enemy *boss = nullptr;
        for (int idx = 0; idx < 8 && boss == nullptr; ++idx)
            boss = g_EnemyManager.bosses[idx];
        th06_psp_boot_notef("REPLAY F%u ST%ld P%d B%d BL%ld/%ld", replayFrame,
                            static_cast<long>(g_GameManager.currentStage),
                            player ? static_cast<int>(player->playerState) : -1,
                            player && player->bombInfo.isInUse ? 1 : 0,
                            boss ? static_cast<long>(boss->life) : -1L,
                            boss ? static_cast<long>(boss->maxLife) : -1L);
        nextSnapshot += 600;
    }
#endif
#if defined(TH06_PSP_STAGE_SOAK) || defined(TH06_PSP_RECORD_SOAK) || defined(TH06_PSP_RESULT_SOAK)
    static int observedStage = -1;
    static unsigned int soakStageFrames;
    if (g_GameManager.currentStage != observedStage)
    {
        observedStage = g_GameManager.currentStage;
        soakStageFrames = 0;
        th06_psp_boot_notef("SOAK STAGE %d ENTER", observedStage);
    }
    ++soakStageFrames;
    // The original replay can desync when the port cannot sustain the exact
    // source frame cadence.  Preserve its movement/shooting stream but prevent
    // a timing-only collision from ending this transition test.
    if (player != nullptr && player->playerState != PLAYER_STATE_DEAD &&
        player->playerState != PLAYER_STATE_SPAWNING)
    {
        player->playerState = PLAYER_STATE_INVULNERABLE;
        player->invulnerabilityTimer.SetCurrent(2);
    }
#if defined(TH06_PSP_RECORD_SOAK)
    // Exercise the normal recording/compaction path without making the test
    // wait for three complete stage scripts. GameManager::RegisterChain does
    // the ordinary stage increment; only the test transition trigger is fake.
    if (g_ReplayManager != nullptr && g_GameManager.currentStage <= 3 && soakStageFrames >= 300 &&
        g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER)
    {
        th06_psp_boot_notef("RECORD SOAK FORCE END S%ld", g_GameManager.currentStage);
        g_Supervisor.curState = SUPERVISOR_STATE_GAMEMANAGER_REINIT;
    }
#endif
#if defined(TH06_PSP_RESULT_SOAK)
    // Enter the exact normal-game game-over transition without waiting for a
    // five-stage manual session. This diagnostic executable is PPSSPP-only.
    if (soakStageFrames >= 300 && g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER)
    {
        th06_psp_boot_notef("RESULT SOAK FORCE GAME OVER ST%ld", g_GameManager.currentStage);
        g_Supervisor.curState = SUPERVISOR_STATE_RESULTSCREEN_FROMGAME;
    }
#endif
#endif
    const unsigned int result = th06_psp_real_player_update(player);

    // A surviving player always respawns with the configured bomb stock.
    // Reassert that engine invariant at the PSP boundary and force the HUD to
    // redraw it; this is one event per death, not a frame-loop log.
    if (player != nullptr && playerStateBefore == PLAYER_STATE_DEAD &&
        player->playerState == PLAYER_STATE_SPAWNING && !g_GameManager.isInRetryMenu)
    {
        const int restoredBombs =
            (g_GameManager.difficulty < EXTRA && !g_GameManager.isInPracticeMode)
                ? static_cast<int>(g_Supervisor.defaultConfig.bombCount)
                : 3;
        const int bombsBeforeRepair = g_GameManager.bombsRemaining;
        g_GameManager.bombsRemaining = static_cast<i8>(restoredBombs);
        g_Gui.flags.flag1 = 2;
        th06_psp_boot_notef("RESPAWN BOMBS %d>%d", bombsBeforeRepair, restoredBombs);
    }

    return result;
}

#if defined(TH06_PSP_STAGE_SOAK) || defined(TH06_PSP_RECORD_SOAK) || defined(TH06_PSP_RESULT_SOAK) || \
    defined(TH06_PSP_REPLAY_TRACE) || defined(TH06_PSP_PS3_REPLAY)
extern "C" unsigned int th06_psp_stage_soak_menu_update(MainMenu *)
{
    static bool started;
    if (!started)
    {
        started = true;
#if defined(TH06_PSP_PS3_REPLAY)
        // Exact header of the sole replay in the installed PS3 build:
        // Marisa A / Extra, with only the stage-7 offset populated.  Seed the
        // fields MainMenu normally sets; ReplayManager validates/deobfuscates
        // the file and restores the recorded stage state afterwards.
        g_GameManager.difficulty = EXTRA;
        g_GameManager.character = CHARA_MARISA;
        g_GameManager.shotType = SHOT_TYPE_A;
        g_GameManager.currentStage = 6;
#else
        // th6_01.rpy is a 1.02 Normal/ReimuA replay containing stages 1--3.
        // Seed the fields consumed before ReplayManager opens the header;
        // ReplayManager then restores all per-stage state and drives input.
        g_GameManager.difficulty = NORMAL;
        g_GameManager.character = CHARA_REIMU;
        g_GameManager.shotType = SHOT_TYPE_A;
        g_GameManager.currentStage = 0;
#endif
        g_GameManager.livesRemaining = 2;
        g_GameManager.bombsRemaining = 3;
#if defined(TH06_PSP_RECORD_SOAK) || defined(TH06_PSP_RESULT_SOAK)
        g_GameManager.isInReplay = 0;
#else
        g_GameManager.isInReplay = 1;
#endif
        g_GameManager.isInPracticeMode = 0;
        g_GameManager.demoMode = 0;
#if defined(TH06_PSP_RESULT_SOAK)
        // Replacing MainMenu::OnUpdate deliberately skips its STATE_STARTUP
        // call to LoadTitleAnm(), which normally leaves the TL archive open
        // for the later result screen. Recreate that archive precondition so
        // this test reaches the same AddedCallback as a real menu-started run.
        if (g_Supervisor.LoadPbg3(3, TH_TL_DAT_FILE) != 0)
            th06_psp_boot_note("RESULT SOAK TL LOAD FAILED");
#endif
#if defined(TH06_PSP_RECORD_SOAK) || defined(TH06_PSP_RESULT_SOAK)
        g_GameManager.replayFile[0] = '\0';
#else
        std::strcpy(reinterpret_cast<char *>(g_GameManager.replayFile), "replay/th6_01.rpy");
#endif
        g_Supervisor.framerateMultiplier = 1.0f;
        g_Supervisor.curState = SUPERVISOR_STATE_GAMEMANAGER;
#if defined(TH06_PSP_RECORD_SOAK)
        th06_psp_boot_note("RECORD SOAK NORMAL REIMUA");
#elif defined(TH06_PSP_RESULT_SOAK)
        th06_psp_boot_note("RESULT SOAK NORMAL REIMUA");
#elif defined(TH06_PSP_REPLAY_TRACE)
        th06_psp_boot_note("EXACT REPLAY TRACE th6_01 STAGE1");
#elif defined(TH06_PSP_PS3_REPLAY)
        th06_psp_boot_note("PS3 REPLAY AUTO th6_01 EXTRA MARISAA");
#else
        th06_psp_boot_note("STAGE SOAK REPLAY th6_01 STAGES1-3");
#endif
    }
    return CHAIN_CALLBACK_RESULT_CONTINUE_AND_REMOVE_JOB;
}
#endif

extern "C" int th06_real_add_calc_chain(Chain *chain, ChainElem *elem, int priority)
    asm("__real__ZN5Chain14AddToCalcChainEP9ChainElemi");
extern "C" int th06_wrap_add_calc_chain(Chain *chain, ChainElem *elem, int priority)
    asm("__wrap__ZN5Chain14AddToCalcChainEP9ChainElemi");

extern "C" int th06_wrap_add_calc_chain(Chain *chain, ChainElem *elem, int priority)
{
    // Static chain elements are initialized with callback addresses inside
    // their defining object file.  GNU ld --wrap cannot rewrite those local
    // address constants, so redirect the three callbacks that need PSP
    // instrumentation as they enter the global chain.
    if (elem != nullptr)
    {
        const ChainCallback callback = elem->callback;
        if (callback == reinterpret_cast<ChainCallback>(th06_real_gui_update))
            elem->callback = reinterpret_cast<ChainCallback>(th06_wrap_gui_update);
        else if (callback == reinterpret_cast<ChainCallback>(th06_real_enemy_update))
            elem->callback = reinterpret_cast<ChainCallback>(th06_wrap_enemy_update);
        else if (callback == reinterpret_cast<ChainCallback>(th06_real_effects_update))
            elem->callback = reinterpret_cast<ChainCallback>(th06_wrap_effects_update);
        else if (callback == reinterpret_cast<ChainCallback>(th06_psp_real_player_update))
            elem->callback = reinterpret_cast<ChainCallback>(th06_psp_player_update);
#if defined(TH06_PSP_STAGE_SOAK) || defined(TH06_PSP_RECORD_SOAK) || defined(TH06_PSP_RESULT_SOAK) || \
    defined(TH06_PSP_REPLAY_TRACE) || defined(TH06_PSP_PS3_REPLAY)
        else if (callback == reinterpret_cast<ChainCallback>(th06_psp_real_main_menu_update))
            elem->callback = reinterpret_cast<ChainCallback>(th06_psp_stage_soak_menu_update);
#endif
    }
    const bool beginTrace = __atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0;
    if (beginTrace)
        th06_psp_boot_notef("ADD CALC BEGIN P%d", priority);
    const int result = th06_real_add_calc_chain(chain, elem, priority);
    if (__atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0)
        th06_psp_boot_notef("ADD CALC END P%d R%d S%ldKB", priority, result, CurrentStackFreeKb());
    return result;
}

extern "C" int th06_real_add_draw_chain(Chain *chain, ChainElem *elem, int priority)
    asm("__real__ZN5Chain14AddToDrawChainEP9ChainElemi");
extern "C" int th06_wrap_add_draw_chain(Chain *chain, ChainElem *elem, int priority)
    asm("__wrap__ZN5Chain14AddToDrawChainEP9ChainElemi");

extern "C" ChainCallbackResult th06_real_bullet_draw(BulletManager *manager)
    asm("__real__ZN13BulletManager6OnDrawEPS_");
extern "C" ChainCallbackResult th06_wrap_bullet_draw(BulletManager *manager)
    asm("__wrap__ZN13BulletManager6OnDrawEPS_");

extern "C" ChainCallbackResult th06_wrap_bullet_draw(BulletManager *manager)
{
    __atomic_store_n(&gBulletRenderMath, 1u, __ATOMIC_RELEASE);
    if (manager == nullptr)
    {
        __atomic_store_n(&gBulletRenderMath, 0u, __ATOMIC_RELEASE);
        return th06_real_bullet_draw(manager);
    }

    g_AnmManager->SetDepthFunc(DEPTH_FUNC_ALWAYS);

    // Preserve the laser path exactly, but use the VFPU's paired operation
    // instead of two scalar libm calls while the angle is in the proven game
    // range.  Lasers are few; all bulk work below is the bullet path.
    for (Laser &laser : manager->lasers)
    {
        if (!laser.inUse)
            continue;
        float sine;
        float cosine;
        if (std::isfinite(laser.angle) && laser.angle >= -16.0f * ZUN_PI && laser.angle <= 16.0f * ZUN_PI)
            vfpu_sincos(laser.angle, &sine, &cosine);
        else
            fsincos_wrapper(&sine, &cosine, laser.angle);
        const float laserOffset = (laser.endOffset - laser.startOffset) * 0.5f + laser.startOffset;
        laser.vm0.pos.x = cosine * laserOffset + laser.pos.x;
        laser.vm0.pos.y = sine * laserOffset + laser.pos.y;
        laser.vm0.pos.z = 0.0f;
        laser.color = COLOR_COMBINE_ALPHA(COLOR_WHITE, laser.color);
        g_AnmManager->Draw3(&laser.vm0);

        if (laser.startOffset < 16.0f || laser.speed == 0.0f)
        {
            laser.vm1.pos.x = cosine * laser.startOffset + laser.pos.x;
            laser.vm1.pos.y = sine * laser.startOffset + laser.pos.y;
            laser.vm1.pos.z = 0.0f;
            laser.vm1.color = laser.vm0.color;
            laser.vm1.flags.colorOp = AnmVmColorOp_Add;
            laser.vm1.color = COLOR_SET_ALPHA2(laser.vm1.color, 0xff);
            laser.vm1.scaleX = (laser.width / 10.0f) * ((16.0f - laser.startOffset) / 16.0f);
            laser.vm1.scaleY = laser.vm1.scaleX;
            if (laser.vm1.scaleY < 0.0f)
                laser.vm1.scaleX = laser.vm1.scaleY = laser.width / 10.0f;
            g_AnmManager->Draw3(&laser.vm1);
        }
    }

    g_ItemManager.OnDraw();

    // The inherited renderer walks all 640 slots four times to establish its
    // four visual layers.  Classify once, retain the original order inside
    // each layer (important for alpha blending), then draw each bucket.
    unsigned int bucketCounts[4] = {};
    for (Bullet &bullet : manager->bullets)
    {
        if (bullet.state == 0)
            continue;
        int bucket = -1;
        if (bullet.sprites.bulletHeight > 16)
            bucket = 0;
        else if (bullet.sprites.bulletHeight == 16)
            bucket = (bullet.sprites.spriteBullet.anmFileIndex == ANM_SCRIPT_BULLET3_RING_BALL ||
                      bullet.sprites.spriteBullet.anmFileIndex == ANM_SCRIPT_BULLET3_BALL)
                         ? 1
                         : 2;
        else if (bullet.sprites.bulletHeight == 8)
            bucket = 3;
        if (bucket >= 0)
            gPspBulletDrawBuckets[bucket][bucketCounts[bucket]++] = &bullet;
    }

    for (int bucket = 0; bucket < 4; ++bucket)
    {
        for (unsigned int i = 0; i < bucketCounts[bucket]; ++i)
        {
            Bullet *bullet = gPspBulletDrawBuckets[bucket][i];
            if (g_Supervisor.hasD3dHardwareVertexProcessing)
                BulletManager::DrawBullet(bullet);
            else
                BulletManager::DrawBulletNoHwVertex(bullet);
        }
    }

    g_AnmManager->SetDepthFunc(DEPTH_FUNC_LEQUAL);
    __atomic_store_n(&gBulletRenderMath, 0u, __ATOMIC_RELEASE);
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

extern "C" int th06_wrap_add_draw_chain(Chain *chain, ChainElem *elem, int priority)
{
    if (elem != nullptr && elem->callback == reinterpret_cast<ChainCallback>(th06_real_effects_draw))
        elem->callback = reinterpret_cast<ChainCallback>(th06_wrap_effects_draw);
    else if (elem != nullptr && elem->callback == reinterpret_cast<ChainCallback>(th06_real_bullet_draw))
        elem->callback = reinterpret_cast<ChainCallback>(th06_wrap_bullet_draw);
    const bool trace = __atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0;
    if (trace)
        th06_psp_boot_notef("ADD DRAW BEGIN P%d", priority);
    const int result = th06_real_add_draw_chain(chain, elem, priority);
    if (trace)
        th06_psp_boot_notef("ADD DRAW END P%d R%d S%ldKB", priority, result, CurrentStackFreeKb());
    return result;
}
