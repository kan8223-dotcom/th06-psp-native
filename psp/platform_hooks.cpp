#include "platform_hooks.hpp"

#include "diag.hpp"

#include <pspkernel.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_surface.h>

#include "AnmManager.hpp"
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
#include "ItemManager.hpp"
#include "MainMenu.hpp"
#include "Player.hpp"
#include "ReplayManager.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"
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
volatile unsigned int gBossDeathTraceFrames;
unsigned long long gPerfCalcUs;
unsigned long long gPerfDrawUs;
unsigned int gPerfCalcCalls;
unsigned int gPerfDrawCalls;

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
#if defined(TH06_PSP_STAGE_SOAK)
    return true;
#else
    return th06_psp_periodic_diag_enabled() != 0;
#endif
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
    if (runtime && verbose)
    {
        th06_psp_runtime_progress();
        th06_psp_boot_notef("ARC BEGIN %s %luKB", filename ? filename : "?", expected / 1024u);
    }
    u8 *result = th06_real_pbg3_read(archive, entryIdx, filename);
    if (runtime)
        th06_psp_runtime_progress();
    if (runtime && verbose)
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
    gPerfCalcUs += sceKernelGetSystemTimeWide() - perfStartUs;
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
    gPerfDrawUs += sceKernelGetSystemTimeWide() - perfStartUs;
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
#if defined(TH06_PSP_STAGE_SOAK)
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
        th06_psp_boot_notef("STAGE END SIGNAL OLD%ld S%ldKB", g_GameManager.currentStage,
                            CurrentStackFreeKb());
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
    const unsigned int textureWidth = bitCeil(static_cast<unsigned int>(source->w));
    const unsigned int textureHeight = bitCeil(static_cast<unsigned int>(source->h));
    g_GfxBackend->SetTextureImage(textureWidth, textureHeight, PIXEL_RGB, PIXEL_UNSIGNED_BYTE, nullptr);
    unsigned char *surfaceData = AnmManager::ExtractSurfacePixels(source, 3);
    g_GfxBackend->SetTextureSubImage(0, 0, source->w, source->h, surfaceData);
    delete[] surfaceData;

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
        th06_psp_boot_notef("DEMO LOAD END %ums A%u %uKB", elapsedMs, gDemoAssetCount,
                            gDemoAssetBytes / 1024u);
        gDemoLoadStartUs = 0;
    }
    if (stageReinit)
    {
        th06_psp_boot_notef("STAGE REINIT REGISTER END R%ld NOW%ld S%ldKB", result,
                            g_GameManager.currentStage, CurrentStackFreeKb());
        __atomic_store_n(&gStageReinitTrace, 0, __ATOMIC_RELEASE);
    }
    else if (stageReinitAttempt)
    {
        // One durable result is enough in the normal build.  The dozens of
        // phase records are available only with TH06PSP_DIAG.ON so ordinary
        // stage changes do not fight BGM reads on the Memory Stick.
        th06_psp_boot_notef("STAGE REINIT RESULT R%ld NOW%ld", result, g_GameManager.currentStage);
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
    const bool stageReinit = g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER_REINIT &&
                             DetailedStageTransitionTracing();
    if (stageReinit)
    {
        __atomic_store_n(&gStageReinitTrace, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&gTransitionTraceMarkers, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&gTransitionTraceBudget, 3, __ATOMIC_RELEASE);
        th06_psp_runtime_progress();
        th06_psp_boot_notef("STAGE REINIT CUT BEGIN OLD%ld S%ldKB", g_GameManager.currentStage,
                            CurrentStackFreeKb());
    }
    th06_real_game_manager_cut();
    if (stageReinit)
    {
        th06_psp_boot_notef("STAGE REINIT CUT END S%ldKB", CurrentStackFreeKb());
        th06_psp_runtime_progress();
    }
}

#define TH06_WRAP_STAGE_VOID(tag, id)                                                                                 \
    extern "C" void th06_real_##id() asm("__real_" tag);                                                            \
    extern "C" void th06_wrap_##id() asm("__wrap_" tag);                                                           \
    extern "C" void th06_wrap_##id()                                                                                \
    {                                                                                                                 \
        const bool trace = __atomic_load_n(&gStageReinitTrace, __ATOMIC_ACQUIRE) != 0;                                \
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
TH06_WRAP_STAGE_VOID("_ZN13ReplayManager13StopRecordingEv", stop_replay)

#undef TH06_WRAP_STAGE_VOID

extern "C" void th06_wrap_ecl_unload(EclManager *manager) asm("__wrap__ZN10EclManager6UnloadEv");

extern "C" void th06_wrap_ecl_unload(EclManager *manager)
{
    const bool trace = __atomic_load_n(&gStageReinitTrace, __ATOMIC_ACQUIRE) != 0;
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
    const bool trace = __atomic_load_n(&gStageReinitTrace, __ATOMIC_ACQUIRE) != 0;
    if (trace)
        th06_psp_boot_note("STOP AUDIO BEGIN");
    th06_real_supervisor_stop_audio(supervisor);
    if (trace)
        th06_psp_boot_notef("STOP AUDIO END S%ldKB", CurrentStackFreeKb());
}

#define TH06_TRACE_STAGE_REGISTER_BEGIN(name)                                                                         \
    const bool trace = __atomic_load_n(&gStageReinitTrace, __ATOMIC_ACQUIRE) != 0;                                    \
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
    TH06_TRACE_STAGE_REGISTER_BEGIN("ADD REPLAY");
    const long result = th06_real_replay_register(demo, path);
    TH06_TRACE_STAGE_REGISTER_END("ADD REPLAY", result);
    return result;
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
#if defined(TH06_PSP_STAGE_SOAK)
    static int observedStage = -1;
    if (g_GameManager.currentStage != observedStage)
    {
        observedStage = g_GameManager.currentStage;
        th06_psp_boot_notef("SOAK STAGE %d ENTER", observedStage);
    }
    // The original replay can desync when the port cannot sustain the exact
    // source frame cadence.  Preserve its movement/shooting stream but prevent
    // a timing-only collision from ending this transition test.
    if (player != nullptr && player->playerState != PLAYER_STATE_DEAD &&
        player->playerState != PLAYER_STATE_SPAWNING)
    {
        player->playerState = PLAYER_STATE_INVULNERABLE;
        player->invulnerabilityTimer.SetCurrent(2);
    }
#endif
    return th06_psp_real_player_update(player);
}

#if defined(TH06_PSP_STAGE_SOAK) || defined(TH06_PSP_REPLAY_TRACE) || defined(TH06_PSP_PS3_REPLAY)
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
        g_GameManager.isInReplay = 1;
        g_GameManager.isInPracticeMode = 0;
        g_GameManager.demoMode = 0;
        std::strcpy(reinterpret_cast<char *>(g_GameManager.replayFile), "replay/th6_01.rpy");
        g_Supervisor.framerateMultiplier = 1.0f;
        g_Supervisor.curState = SUPERVISOR_STATE_GAMEMANAGER;
#if defined(TH06_PSP_REPLAY_TRACE)
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
#if defined(TH06_PSP_STAGE_SOAK) || defined(TH06_PSP_REPLAY_TRACE) || defined(TH06_PSP_PS3_REPLAY)
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

extern "C" int th06_wrap_add_draw_chain(Chain *chain, ChainElem *elem, int priority)
{
    if (elem != nullptr && elem->callback == reinterpret_cast<ChainCallback>(th06_real_effects_draw))
        elem->callback = reinterpret_cast<ChainCallback>(th06_wrap_effects_draw);
    const bool trace = __atomic_load_n(&gTransitionTraceBudget, __ATOMIC_ACQUIRE) != 0;
    if (trace)
        th06_psp_boot_notef("ADD DRAW BEGIN P%d", priority);
    const int result = th06_real_add_draw_chain(chain, elem, priority);
    if (trace)
        th06_psp_boot_notef("ADD DRAW END P%d R%d S%ldKB", priority, result, CurrentStackFreeKb());
    return result;
}
