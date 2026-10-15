// Actual MidiPsp MeRender/RunMeJob and audio_me finish/sleep functions, with
// only the job/clock/cache hardware boundary replaced by deterministic hooks.
#include "audio_me.h"
#include "midi/midi_me_protocol.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
using u32 = uint32_t;
using u8 = uint8_t;
namespace tdls { using u64 = uint64_t; }
#include "actual_midi_wait_types.hpp"

enum { ME_STAT_IDLE = 0, ME_STAT_DONE = 1, ME_CMD_MIDI = 3 };
static MeSharedMailbox gBox{};
static volatile MeSharedMailbox *gMeMailboxUncached = &gBox;
static volatile unsigned gMeMaxWaitUs, gMeJobs;
static uint64_t gNowUs, gCompleteAt;
static unsigned gWakeDelay, gReleaseCalls, gPinCalls, gQueuedFrames;
static unsigned gMainOpportunities, gLateWakeUs;
static unsigned gUnderruns, gDacTicks, gEscapePreemptions, gDacSequence;
static bool gPoisoned, gClaimFail, gLateWake, gPrimed, gEscapeSeen;
static uint64_t gOriginUs;
static std::vector<unsigned> gSleeps;
static u32 gMeLastElapsed;
static short gMidiMeOut[2048];
static u8 *gBlob;
struct Borrowed { unsigned appliedCalls = 0; void applied() { ++appliedCalls; } };
static Borrowed gMeBorrowed;
static void HostMemoryBarrier() {}
// Constructed scheduler model: output(0x10)/escape(0x0f) can interrupt the
// producer(0x18). Voluntary sleep grants main(0x20) execution opportunities,
// but an adversarial wake delay of 70 ms can still starve the fixed ring.
// These are explicit model assumptions, not measured PSP scheduling/FPS.
static void Advance(uint64_t target)
{
    while (gOriginUs + uint64_t(gDacSequence) * 512000000ull / 44100ull <= target)
    {
        ++gDacSequence;
        ++gDacTicks;
        if (gPrimed)
        {
            if (gQueuedFrames >= 512) gQueuedFrames -= 512;
            else { ++gUnderruns; gPrimed = false; }
        }
    }
    if (!gEscapeSeen && target >= gOriginUs + 3000)
    {
        gEscapeSeen = true;
        ++gEscapePreemptions;
    }
    gNowUs = target;
    if (gBox.command == ME_CMD_MIDI && gNowUs >= gCompleteAt) gBox.status = ME_STAT_DONE;
}
[[maybe_unused]] static void HostSpinTick() { Advance(gNowUs + 10); }
static void HostAfterPublish(volatile MeSharedMailbox *box)
{
    box->midi.result = TH06_MIDI_RESULT_OK;
    box->midi.elapsedMs = 1;
    Advance(gNowUs + gWakeDelay);
}
static unsigned sceKernelGetSystemTimeLow() { return static_cast<unsigned>(gNowUs); }
static void sceKernelDelayThread(unsigned us)
{
    gSleeps.push_back(us);
    ++gMainOpportunities;
    Advance(gLateWake ? std::max(gNowUs + us, gOriginUs + gLateWakeUs) : gNowUs + us);
}
static void release_me() { ++gReleaseCalls; }
static void poison_me() { gPoisoned = true; }
static void PinUnacknowledgedMePayloads() { ++gPinCalls; }
static unsigned Phys(const void *) { return 1; }
static void sceKernelDcacheInvalidateRange(const void *, unsigned) {}
extern "C" int th06_psp_me_available() { return !gPoisoned; }
extern "C" int th06_psp_me_poisoned() { return gPoisoned; }
extern "C" volatile Th06MidiMeBox *th06_psp_me_midi_begin()
{
    return gClaimFail ? nullptr : &gBox.midi;
}
extern "C" unsigned th06_psp_audio_queued_bytes() { return gQueuedFrames * 4u; }
#include "actual_midi_wait.hpp"

static unsigned gChecks, gFailures;
static void Check(bool ok, const std::string &label)
{
    ++gChecks;
    if (!ok) { ++gFailures; std::fprintf(stderr, "FAIL %s\n", label.c_str()); }
}
static void Reset(unsigned computeUs, unsigned wakeUs = 0)
{
    std::memset(&gBox, 0, sizeof(gBox));
    gMeMailboxUncached = &gBox;
    gNowUs = 100000;
    gOriginUs = gNowUs;
    gCompleteAt = gNowUs + computeUs;
    gWakeDelay = wakeUs;
    gReleaseCalls = gPinCalls = 0;
    gMainOpportunities = 0;
    gLateWakeUs = 71621;
    gPoisoned = gClaimFail = false;
    gLateWake = gEscapeSeen = false;
    gPrimed = true;
    gUnderruns = gDacTicks = gEscapePreemptions = gDacSequence = 0;
    gMeJobs = gMeMaxWaitUs = 0;
    gSleeps.clear();
    gQueuedFrames = 3072;
    gMeLastElapsed = 0;
    gMeBorrowed.appliedCalls = 0;
}

int main()
{
    for (unsigned wake : {35000u, 39000u, 71621u})
    {
        Reset(1500, wake);
        MeRenderInfo info{};
        unsigned waited = 0;
        Check(MeRender(512, info, &waited) == 0, "late SC wake still acknowledged");
        Check(gSleeps.empty(), "already-DONE skips estimated sleep and polling");
        Check(waited == wake, "already acknowledged late wake preserves actual elapsed");
        Check(gReleaseCalls == 1 && gPinCalls == 0 && gMeBorrowed.appliedCalls == 1,
              "late SC wake releases and records actual ACK");
    }
    Reset(39000);
    MeRenderInfo slow{};
    unsigned waited = 0;
    Check(MeRender(1024, slow, &waited) == 0, "real slow job finishes below unchanged timeout");
    Check(waited >= 39000 && waited < 39050, "slow job returns within one cooperative poll interval");
    Check(!gSleeps.empty() && gMainOpportunities > 0, "slow render yields SC time to main");
    Reset(1000);
    MeRenderInfo normal{};
    Check(MeRender(512, normal, &waited) == 0, "normal job completes");
    Check(waited >= 1000 && waited < 1050 && !gSleeps.empty(), "normal render uses cooperative completion");
    Check(std::all_of(gSleeps.begin(), gSleeps.end(), [](unsigned us) { return us == 50; }) &&
          gMainOpportunities > 0, "MeRender initial0 yields in 50-us polls and permits main execution");
    for (unsigned queued : {0u, 511u, 512u})
    {
        Reset(1000);
        gQueuedFrames = queued;
        Check(MeRender(512, normal, &waited) == 0, "low PCM job completes");
        Check(!gSleeps.empty(), "low PCM also keeps cooperative completion contract");
    }
    Reset(100000);
    Check(MeRender(512, normal, &waited) == 2, "timeout remains fatal fallback reason");
    Check(gPoisoned && gPinCalls == 1 && gReleaseCalls == 1, "unacknowledged payload pin retained");
    Check(gMeBorrowed.appliedCalls == 0, "timeout cannot forge ACK");
    Check(gNowUs - 100000 >= 8000 + 512 * 34 && gNowUs - 100000 < 8000 + 512 * 34 + 50,
          "original timeout boundary remains within one 50-us poll");
    Reset(1000);
    gClaimFail = true;
    Check(MeRender(512, normal, &waited) == 1 && gPinCalls == 0 && gReleaseCalls == 0,
          "unclaimed job does not acquire a fake read-set");
    Reset(500);
    gNowUs = uint64_t(std::numeric_limits<u32>::max()) - 200;
    gOriginUs = gNowUs;
    gCompleteAt = gNowUs + 500;
    Check(MeRender(512, normal, &waited) == 0 && waited < 2000,
          "microsecond clock rollover preserves bounded wait");
    // First-sleep arguments work uniformly; runtime MeRender passes zero.
    for (unsigned op : {TH06_MIDI_OP_RENDER, TH06_MIDI_OP_INFO, TH06_MIDI_OP_LOAD, TH06_MIDI_OP_INIT})
    {
        Reset(5000);
        gBox.midi.op = op;
        Check(th06_psp_me_midi_finish(200000, 750, &waited) == 1, "job with explicit first sleep completes");
        Check(!gSleeps.empty() && gSleeps.front() == 750, "cooperative first sleep retained for all opcodes");
    }
    Reset(0);
    gBox.midi.op = TH06_MIDI_OP_INFO;
    Check(th06_psp_me_midi_finish(200000, 5000, &waited) == 1 && gSleeps.empty() && waited == 0,
          "already-DONE returns immediately without first sleep");
#if WAIT_HAS_LEGACY
    for (unsigned firstSleep : {0u, 750u})
    {
        Reset(5000);
        gQueuedFrames = 2560;
        gLateWake = true;
        gBox.midi.op = TH06_MIDI_OP_RENDER;
        Check(LegacyMidiFinish(8000 + 512 * 34, firstSleep, &waited) == 1, "legacy delayed ACK still succeeds");
        Check(waited == 71621 && gUnderruns == 1, "legacy coarse or 50-us sleep reproduces modeled starvation");
        std::printf("MODEL legacy_sleep%u ACK%uus UND%u NOT_HARDWARE\n", firstSleep, waited, gUnderruns);
    }
#endif
    for (unsigned compute : {1000u, 5000u, 15000u})
    {
        Reset(compute);
        gQueuedFrames = 2560;
        Check(MeRender(512, normal, &waited) == 0, "normal cooperative render acknowledged");
        Check(!gSleeps.empty() && gMainOpportunities > 0 && waited >= compute && waited < compute + 50,
              "normal render yields while waiting and returns within one poll interval");
        gQueuedFrames += 512; // The now-published block; no new lookahead.
        Check(gUnderruns == 0 && gQueuedFrames <= 3072, "same-size ring receives PCM before modeled empty");
        if (compute >= 3000) Check(gEscapePreemptions == 1, "higher-priority escape remains schedulable");
        if (compute >= 12000) Check(gDacTicks >= 2, "higher-priority output continues during cooperative wait");
        std::printf("MODEL cooperative COMPUTE%uus ACK%uus UND%u MAIN_OPPORTUNITIES%u NOT_HARDWARE\n",
                    compute, waited, gUnderruns, gMainOpportunities);
    }
    for (unsigned late : {70000u, 71621u})
    {
        Reset(5000);
        gQueuedFrames = 2560;
        gLateWake = true;
        gLateWakeUs = late;
        Check(MeRender(512, normal, &waited) == 0 && waited == late,
              "late-woken but acknowledged job retains old successful ACK behavior");
        Check(gUnderruns == 1 && gMainOpportunities > 0,
              "expected limitation: 70-ms wake delay can still drain the unchanged ring");
        std::printf("EXPECTED_LIMITATION delayed_cooperative_wake ACK%uus UND%u NOT_FIXED NOT_HARDWARE\n",
                    waited, gUnderruns);
    }
    std::printf("MIDI_WAIT\t%s\tCHECKS%u\tFAILURES%u\tACTUAL_POLICY_FAKE_CLOCK\tNOT_HARDWARE\n",
                gFailures ? "FAIL" : "PASS", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
