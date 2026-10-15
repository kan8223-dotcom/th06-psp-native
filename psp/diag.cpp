// Boot overlay, boot watchdog and escape thread.
//
// Background: first-light testing on hardware produced a single blue screen
// with no way to tell a crash from a hang, no way to read a stage without
// unplugging the device, and no way back to the XMB except a forced shutdown.
// The three facilities here remove all of that, and all three stop mattering
// once the engine presents a frame.
#include "diag.hpp"

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "midi/MidiPsp.hpp"
#include "audio_me.h"
#include "video.hpp"

namespace
{

constexpr int kLines = 12;
constexpr int kLineChars = 44;
constexpr int kScale = 2;
constexpr int kLineHeight = 7 * kScale;
constexpr unsigned int kBackground = 0xff101010u;
constexpr unsigned int kForeground = 0xffe0e0e0u;
constexpr unsigned int kHighlight = 0xff40ff40u;

// The engine presents during initialisation, so a single presented frame only
// means "the screen is the engine's now".  The record stays on until the game
// is genuinely running.
constexpr unsigned int kBootFrames = 60;

char gLines[kLines][kLineChars];
int gLineCount;
int gLineHead; // oldest line
bool gOverlayStopped;
volatile bool gBootPhaseEnded;
volatile unsigned int gFramesPresented;
volatile unsigned int gRuntimeProgress;

char gLastNote[kLineChars];

// -------------------------------------------------------------- watchdog
volatile unsigned int gWatchdogDeadlineUs;
volatile unsigned int gWatchdogTimeoutUs;
volatile int gWatchdogArmed;
SceUID gWatchdogThread = -1;

// ---------------------------------------------------------------- escape
SceUID gEscapeThread = -1;
volatile int gExitRequested;

constexpr unsigned int kRuntimeHeartbeatUs = 5u * 1000u * 1000u;
constexpr unsigned int kRuntimeStallUs = 8u * 1000u * 1000u;

extern "C" void th06_psp_audio_diag(unsigned int *queuedFrames, unsigned int *underruns,
                                     unsigned int *readFrame, unsigned int *writeFrame);
extern "C" void th06_psp_audio_diag_window(unsigned int *minQueuedFrames, unsigned int *droppedFrames);
extern "C" void th06_psp_sound_diag_window(unsigned int *maxProducerGapUs, unsigned int *maxMixUs,
                                            unsigned int *maxBgmReadUs, unsigned int *mixCalls);
extern "C" void th06_psp_bgm_diag_window(unsigned int *reads, unsigned int *maxReadUs,
                                          unsigned int *missPasses, unsigned int *missFrames);
extern "C" void th06_psp_me_audio_diag_window(unsigned int *jobs, unsigned int *fallbacks,
                                                unsigned int *timeouts, unsigned int *maxWaitUs);

// The periodic counters are deliberately collected in RAM even in a normal
// build.  Keeping the last few seconds only in the individual subsystems was
// not enough: their window APIs reset every five seconds, so a later freeze or
// HOME exit lost the evidence.  This aggregate is owned by the diagnostic
// thread and is only serialized at an explicit transition/failure checkpoint.
struct AudioFlightRecord
{
    unsigned int lastUnderruns;
    unsigned int underruns;
    unsigned int minQueued;
    unsigned int droppedFrames;
    unsigned int maxProducerGapUs;
    unsigned int maxMixUs;
    unsigned int maxBgmReadUs;
    unsigned int bgmReads;
    unsigned int bgmMissPasses;
    unsigned int bgmMissFrames;
    unsigned int meJobs;
    unsigned int meFallbacks;
    unsigned int meTimeouts;
    unsigned int meMaxWaitUs;
    unsigned int anomalyBits;
};

enum AudioAnomaly : unsigned int
{
    AUDIO_ANOMALY_DAC_UNDERRUN = 1u << 0,
    AUDIO_ANOMALY_PRODUCER_GAP = 1u << 1,
    AUDIO_ANOMALY_BGM_IO_STALL = 1u << 2,
    AUDIO_ANOMALY_BGM_CACHE_MISS = 1u << 3,
    AUDIO_ANOMALY_ME_TIMEOUT = 1u << 4,
    AUDIO_ANOMALY_QUEUE_DROP = 1u << 5,
};

// A healthy producer normally revisits the 3072-frame watermark every 2 ms.
// 20 ms is diagnostic, while still far below the 69.7 ms PCM cushion.  A
// 100 ms Memory Stick read is recorded independently: asynchronous I/O can be
// slow without being audible, and the cache-miss counters prove whether that
// slow read actually reached the mixer.
constexpr unsigned int kProducerGapDiagnosticUs = 20u * 1000u;
constexpr unsigned int kBgmIoStallDiagnosticUs = 100u * 1000u;

AudioFlightRecord gAudioFlight{};
volatile int gAudioFlightSampling;

extern "C" void th06_psp_frame_pacing(unsigned int *frames, unsigned int *misses, unsigned int *extraVblanks,
                                      unsigned int *worstUs);

// MIDI synthesis telemetry kept across the 5-second windows (diag thread
// only) and written once at a checkpoint, like the audio flight record.
struct MidiFlightRecord
{
    unsigned int windows;
    unsigned int loadMaxPermille;
    unsigned int waitMaxUs;
    unsigned long long frames;
    unsigned long long waitTotalUs;
    unsigned int activeMax;
};
MidiFlightRecord gMidiFlight{};
unsigned int gLastPacingFrames, gLastPacingMisses, gLastPacingExtra;

const char *MidiModeName(unsigned int mode)
{
#if defined(TH06_PSP_MIDI_MEMSIM)
    if (mode == 3)
        return "MEMSIM";
#endif
    return mode == 2 ? "ME" : (mode == 1 ? "SC" : "OFF");
}

struct MidiDiagWindow
{
    Th06MidiDiag midi;
    unsigned int frames, misses, extra, worst;
    unsigned int windowFrames, windowMisses, windowExtra;
};

// The caller holds gAudioFlightSampling across both audio and MIDI RAM
// updates. No Memory Stick writes occur while this try-lock is held.
void SampleMidiWindow(MidiDiagWindow &window)
{
    Th06MidiDiag &midi = window.midi;
    th06_midi_diag_window(&midi);
    th06_psp_frame_pacing(&window.frames, &window.misses, &window.extra, &window.worst);
    window.windowFrames = window.frames - gLastPacingFrames;
    window.windowMisses = window.misses - gLastPacingMisses;
    window.windowExtra = window.extra - gLastPacingExtra;
    gLastPacingFrames = window.frames;
    gLastPacingMisses = window.misses;
    gLastPacingExtra = window.extra;
    if (midi.frames != 0)
    {
        ++gMidiFlight.windows;
        gMidiFlight.frames += midi.frames;
        gMidiFlight.waitTotalUs += midi.waitTotalUs;
        if (midi.loadPermille > gMidiFlight.loadMaxPermille)
            gMidiFlight.loadMaxPermille = midi.loadPermille;
        if (midi.waitMaxUs > gMidiFlight.waitMaxUs)
            gMidiFlight.waitMaxUs = midi.waitMaxUs;
        if (midi.activeMax > gMidiFlight.activeMax)
            gMidiFlight.activeMax = midi.activeMax;
    }
}

void EmitMidiWindow(const MidiDiagWindow &window)
{
    const Th06MidiDiag &midi = window.midi;
    // MISS: presented frames in this window whose interval missed a vblank
    // (the 60 Hz sim ran slow), and how many vblanks were lost in total.
    th06_psp_boot_notef("PACE F+%u MISS+%u VB+%u TOTAL %u/%u WORST%uus", window.windowFrames,
                        window.windowMisses, window.windowExtra, window.misses, window.frames, window.worst);
    if (midi.mode != 0)
        th06_psp_boot_notef("MIDI %s J%u/%u F%u QD%u W%u/%uus L%u.%u%% CY%uK V%u LOST%u SCR%u/%u/%u FB%u/%u S%u/%u C%u",
                            MidiModeName(midi.mode), midi.meJobs, midi.scJobs, midi.frames, midi.queueDrops, midi.waitTotalUs,
                            midi.waitMaxUs, midi.loadPermille / 10u, midi.loadPermille % 10u,
                            midi.renderCyclesMax / 1000u, midi.activeMax, midi.notesLost, midi.scrubPasses,
                            midi.scrubBad, midi.scrubRepaired, midi.fallbacks, midi.fallbackReason,
                            midi.songsLoaded, midi.songsFreed, midi.commands);
}

struct AudioDiagWindow
{
    unsigned int queued;
    unsigned int underruns;
    unsigned int underrunDelta;
    unsigned int readFrame;
    unsigned int writeFrame;
    unsigned int minQueued;
    unsigned int droppedFrames;
    unsigned int maxProducerGapUs;
    unsigned int maxMixUs;
    unsigned int maxBgmReadUs;
    unsigned int mixCalls;
    unsigned int bgmReads;
    unsigned int bgmMissPasses;
    unsigned int bgmMissFrames;
    unsigned int meJobs;
    unsigned int meFallbacks;
    unsigned int meTimeouts;
    unsigned int meMaxWaitUs;
};

void RecordMaximum(unsigned int &destination, unsigned int sample)
{
    if (sample > destination)
        destination = sample;
}

bool CollectAudioDiagnostics(AudioDiagWindow &window, bool keepLocked = false)
{
    int expected = 0;
    if (!__atomic_compare_exchange_n(&gAudioFlightSampling, &expected, 1, false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
        return false;

    th06_psp_audio_diag(&window.queued, &window.underruns, &window.readFrame, &window.writeFrame);
    th06_psp_audio_diag_window(&window.minQueued, &window.droppedFrames);
    // BGM owns the read maximum now.  Passing nullptr here prevents the older
    // sound window from consuming it before cache-miss telemetry is sampled.
    th06_psp_sound_diag_window(&window.maxProducerGapUs, &window.maxMixUs, nullptr, &window.mixCalls);
    th06_psp_bgm_diag_window(&window.bgmReads, &window.maxBgmReadUs, &window.bgmMissPasses,
                             &window.bgmMissFrames);
    th06_psp_me_audio_diag_window(&window.meJobs, &window.meFallbacks, &window.meTimeouts,
                                  &window.meMaxWaitUs);

    const unsigned int underrunDelta = window.underruns - gAudioFlight.lastUnderruns;
    window.underrunDelta = underrunDelta;
    gAudioFlight.lastUnderruns = window.underruns;
    gAudioFlight.underruns += underrunDelta;
    if (window.minQueued < gAudioFlight.minQueued)
        gAudioFlight.minQueued = window.minQueued;
    gAudioFlight.droppedFrames += window.droppedFrames;
    RecordMaximum(gAudioFlight.maxProducerGapUs, window.maxProducerGapUs);
    RecordMaximum(gAudioFlight.maxMixUs, window.maxMixUs);
    RecordMaximum(gAudioFlight.maxBgmReadUs, window.maxBgmReadUs);
    gAudioFlight.bgmReads += window.bgmReads;
    gAudioFlight.bgmMissPasses += window.bgmMissPasses;
    gAudioFlight.bgmMissFrames += window.bgmMissFrames;
    gAudioFlight.meJobs += window.meJobs;
    gAudioFlight.meFallbacks += window.meFallbacks;
    gAudioFlight.meTimeouts += window.meTimeouts;
    RecordMaximum(gAudioFlight.meMaxWaitUs, window.meMaxWaitUs);

    if (underrunDelta != 0)
        gAudioFlight.anomalyBits |= AUDIO_ANOMALY_DAC_UNDERRUN;
    if (window.maxProducerGapUs >= kProducerGapDiagnosticUs)
        gAudioFlight.anomalyBits |= AUDIO_ANOMALY_PRODUCER_GAP;
    if (window.maxBgmReadUs >= kBgmIoStallDiagnosticUs)
        gAudioFlight.anomalyBits |= AUDIO_ANOMALY_BGM_IO_STALL;
    if (window.bgmMissPasses != 0 || window.bgmMissFrames != 0)
        gAudioFlight.anomalyBits |= AUDIO_ANOMALY_BGM_CACHE_MISS;
    if (window.meTimeouts != 0)
        gAudioFlight.anomalyBits |= AUDIO_ANOMALY_ME_TIMEOUT;
    if (window.droppedFrames != 0)
        gAudioFlight.anomalyBits |= AUDIO_ANOMALY_QUEUE_DROP;

    if (!keepLocked)
        __atomic_store_n(&gAudioFlightSampling, 0, __ATOMIC_RELEASE);
    return true;
}

void ResetAudioFlightInterval()
{
    const unsigned int lastUnderruns = gAudioFlight.lastUnderruns;
    std::memset(&gAudioFlight, 0, sizeof(gAudioFlight));
    gAudioFlight.lastUnderruns = lastUnderruns;
    gAudioFlight.minQueued = ~0u;
}

void FillRect(int y0, int y1, unsigned int color)
{
    volatile unsigned int *const fb = th06_psp_video_pixels();
    if (fb == nullptr)
        return;
    const int stride = th06_psp_video_stride();
    const int width = th06_psp_video_width();
    const int height = th06_psp_video_height();
    if (y1 > height)
        y1 = height;
    for (int y = y0; y < y1; ++y)
    {
        volatile unsigned int *row = fb + y * stride;
        for (int x = 0; x < width; ++x)
            row[x] = color;
    }
}

// Leaves the device on a legible screen for a few seconds before it goes back
// to the XMB, so a report survives even when nobody has a USB cable to hand.
void ExitToXmbAfterNotice(const char *title, const char *detail)
{
    th06_psp_video_init();
    FillRect(0, th06_psp_video_height(), 0xff202060u);
    th06_psp_text_draw(8, 60, 3, 0xffffffffu, title);
    th06_psp_text_draw(8, 100, 2, kForeground, "LAST STAGE:");
    th06_psp_text_draw(8, 120, 2, kHighlight, detail);
    th06_psp_text_draw(8, 200, 2, kForeground, "RETURNING TO XMB");
    sceKernelDelayThread(6 * 1000 * 1000);
    sceKernelExitGame();
}

int WatchdogThread(SceSize, void *)
{
    unsigned int lastHeartbeatFrames = __atomic_load_n(&gFramesPresented, __ATOMIC_RELAXED);
    unsigned int lastHeartbeatUs = sceKernelGetSystemTimeLow();
    for (;;)
    {
        sceKernelDelayThread(250 * 1000);
        const unsigned int now = sceKernelGetSystemTimeLow();
        // Boot protection always runs at the original 0x12 priority, before
        // any optional runtime telemetry. Disarming boot must not disable
        // the runtime RAM flight recorder.
        if (gWatchdogArmed && static_cast<int>(now - gWatchdogDeadlineUs) >= 0)
        {
            gWatchdogArmed = 0;
            th06_psp_boot_notef("WATCHDOG TIMEOUT %us AFTER: %s", gWatchdogTimeoutUs / 1000000u, gLastNote);
            ExitToXmbAfterNotice("BOOT WATCHDOG", gLastNote);
            return 0;
        }
        if (!gBootPhaseEnded || static_cast<unsigned int>(now - lastHeartbeatUs) < kRuntimeHeartbeatUs)
            continue;

        const unsigned int frames = __atomic_load_n(&gFramesPresented, __ATOMIC_RELAXED);
        AudioDiagWindow audio{};
        MidiDiagWindow midi{};
        const bool sampled = CollectAudioDiagnostics(audio, true);
        if (sampled)
        {
            SampleMidiWindow(midi);
            __atomic_store_n(&gAudioFlightSampling, 0, __ATOMIC_RELEASE);
        }
        if (sampled && th06_psp_periodic_diag_enabled())
        {
            // FAT appends can take tens of ms. Only the optional write batch
            // yields to the 0x18 PCM producer; output (0x10), boot control
            // (0x12 between batches), and HOME/combo (separate 0x0f thread)
            // keep their priorities. No new thread, buffer or PCM lookahead.
            const int originalPriority = sceKernelGetThreadCurrentPriority();
            const SceUID self = sceKernelGetThreadId();
            if (originalPriority > 0 && sceKernelChangeThreadPriority(self, 0x1c) == 0)
            {
                th06_psp_boot_notef("RUN F%u +%u Q%u U%u RW%u/%u MEM%u/%uKB", frames,
                                    frames - lastHeartbeatFrames, audio.queued, audio.underruns,
                                    audio.readFrame, audio.writeFrame,
                                    static_cast<unsigned int>(sceKernelTotalFreeMemSize() / 1024),
                                    static_cast<unsigned int>(sceKernelMaxFreeMemSize() / 1024));
                th06_psp_boot_notef("AUD U+%u MIN%u DROP%u GAP%uus MIX%uus IO%uus R%u CM%u/%u",
                                    audio.underrunDelta, audio.minQueued, audio.droppedFrames,
                                    audio.maxProducerGapUs, audio.maxMixUs, audio.maxBgmReadUs,
                                    audio.bgmReads, audio.bgmMissPasses, audio.bgmMissFrames);
                th06_psp_boot_notef("ME AUD J%u SC%u TO%u W%uus", audio.meJobs, audio.meFallbacks,
                                    audio.meTimeouts, audio.meMaxWaitUs);
                EmitMidiWindow(midi);
                // There are no early returns inside the demoted batch. A
                // restore failure must not silently leave control demoted:
                // the untouched 0x0f escape thread performs the safe exit.
                if (sceKernelChangeThreadPriority(self, originalPriority) < 0 &&
                    sceKernelGetThreadCurrentPriority() != originalPriority)
                {
                    th06_psp_request_exit(4);
                    return 0;
                }
            }
            // Failed demotion skips only durable periodic writes; the RAM
            // samples above remain available to transition/failure capture.
        }
        lastHeartbeatFrames = frames;
        lastHeartbeatUs = now;
    }
}

int EscapeThread(SceSize, void *)
{
    const unsigned int combo = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_START;
    int heldTicks = 0;
    unsigned int lastFrames = __atomic_load_n(&gFramesPresented, __ATOMIC_RELAXED);
    unsigned int lastRuntimeProgress = __atomic_load_n(&gRuntimeProgress, __ATOMIC_RELAXED);
    unsigned int lastProgressUs = sceKernelGetSystemTimeLow();
    unsigned int lastPowerTickUs = lastProgressUs;
    th06_psp_boot_note("DIAG MONITOR READY");
    for (;;)
    {
        sceKernelDelayThread(100 * 1000);
        const unsigned int now = sceKernelGetSystemTimeLow();
        if (static_cast<unsigned int>(now - lastPowerTickUs) >= 1000u * 1000u)
        {
            // Replays and title demos can legitimately run without controller
            // input for minutes.  Keep both display and suspend idle timers
            // alive independently of engine progress.
            scePowerTick(PSP_POWER_TICK_ALL);
            lastPowerTickUs = now;
        }
        const unsigned int frames = __atomic_load_n(&gFramesPresented, __ATOMIC_RELAXED);
        const unsigned int runtimeProgress = __atomic_load_n(&gRuntimeProgress, __ATOMIC_RELAXED);
        if (frames != lastFrames || runtimeProgress != lastRuntimeProgress)
        {
            lastFrames = frames;
            lastRuntimeProgress = runtimeProgress;
            lastProgressUs = now;
        }

        SceCtrlData pad{};
        if (sceCtrlPeekBufferPositive(&pad, 1) <= 0)
        {
            heldTicks = 0;
            continue;
        }
        if ((pad.Buttons & combo) == combo)
        {
            if (++heldTicks >= 8) // ~800 ms, long enough not to fire by accident
            {
                th06_psp_request_exit(2);
                heldTicks = 0;
            }
        }
        else
        {
            heldTicks = 0;
        }

        if (gBootPhaseEnded && static_cast<unsigned int>(now - lastProgressUs) >= kRuntimeStallUs &&
            __atomic_load_n(&gExitRequested, __ATOMIC_ACQUIRE) == 0)
        {
            th06_psp_audio_diag_checkpoint("STALL", 1);
            th06_psp_boot_notef("RUNTIME STALL 8s AT F%u", frames);
            th06_psp_request_exit(3);
        }

        const int exitReason = __atomic_exchange_n(&gExitRequested, 0, __ATOMIC_ACQ_REL);
        if (exitReason != 0)
        {
            const char *name = exitReason == 1 ? "HOME" :
                (exitReason == 2 ? "COMBO" : (exitReason == 4 ? "DIAG PRIORITY" :
                 (exitReason == 5 ? "AUDIO PRIORITY" : "STALL")));
            // STALL was captured immediately above.  HOME/COMBO need the same
            // last-moment RAM snapshot before the process leaves.
            if (exitReason != 3)
            {
                th06_psp_ge_diag_checkpoint(name);
                th06_psp_audio_diag_checkpoint(name, 1);
            }
            th06_psp_boot_notef("EXIT REQUEST %s F%u", name, frames);
            sceKernelExitGame();
            th06_psp_boot_notef("EXIT CALL RETURNED %s", name);
            // If a CFW rejects the first request, retain evidence and retry.
            sceKernelDelayThread(500 * 1000);
            __atomic_store_n(&gExitRequested, exitReason, __ATOMIC_RELEASE);
        }
    }
}

} // namespace

extern "C" void th06_psp_audio_diag_checkpoint(const char *tag, int always)
{
    AudioDiagWindow latest{};
    if (!CollectAudioDiagnostics(latest, true))
        return;

    // Copy and retire the interval before touching FAT.  The five-second
    // collector can resume while the one durable line is being appended.
    const AudioFlightRecord record = gAudioFlight;
    const MidiFlightRecord midiRecord = gMidiFlight;
    ResetAudioFlightInterval();
    __atomic_store_n(&gAudioFlightSampling, 0, __ATOMIC_RELEASE);

    if (!always && record.anomalyBits == 0)
        return;

    const unsigned int minimum = record.minQueued == ~0u ? latest.queued : record.minQueued;
    // C bitfield: 01 DAC underrun, 02 producer gap, 04 slow BGM read,
    // 08 BGM cache miss, 10 ME timeout, 20 PCM queue drop.  This single line
    // is enough to distinguish the four suspected audio failure classes and
    // avoids a burst of competing Memory Stick writes at the checkpoint.
    if (midiRecord.windows != 0)
    {
        const unsigned long long audioUs = midiRecord.frames * 1000000ull / 44100ull;
        const unsigned int avgPermille =
            audioUs ? static_cast<unsigned int>(midiRecord.waitTotalUs * 1000ull / audioUs) : 0u;
        Th06MidiDiag midi{};
        th06_midi_diag_window(&midi);
        unsigned int frames = 0, misses = 0, extra = 0, worst = 0;
        th06_psp_frame_pacing(&frames, &misses, &extra, &worst);
        th06_psp_boot_notef("MIDI! %s %s QD%u LOAD AVG%u.%u%% MAX%u.%u%% WMAX%uus V%u FB%u/%u SCR%u/%u MISS%u/%u",
                            tag ? tag : "?", MidiModeName(midi.mode), midi.queueDrops, avgPermille / 10u, avgPermille % 10u,
                            midiRecord.loadMaxPermille / 10u, midiRecord.loadMaxPermille % 10u,
                            midiRecord.waitMaxUs, midiRecord.activeMax, midi.fallbacks, midi.fallbackReason,
                            midi.scrubBad, midi.scrubRepaired, misses, frames);
    }
    th06_psp_boot_notef("AUD! %s C%X U%u Q%u G%u I%u CM%u/%u ME%u/%u/%u D%u M%u R%u",
                        tag ? tag : "?", record.anomalyBits, record.underruns, minimum,
                        record.maxProducerGapUs, record.maxBgmReadUs, record.bgmMissPasses,
                        record.bgmMissFrames, record.meJobs, record.meFallbacks,
                        record.meTimeouts, record.droppedFrames, record.maxMixUs,
                        record.bgmReads);
}

// -------------------------------------------------------------------- overlay
extern "C" void th06_psp_overlay_push(const char *message)
{
    if (message == nullptr)
        return;
    std::snprintf(gLastNote, sizeof(gLastNote), "%s", message);

    if (gOverlayStopped)
        return;

    int slot;
    if (gLineCount < kLines)
    {
        slot = gLineCount++;
    }
    else
    {
        slot = gLineHead;
        gLineHead = (gLineHead + 1) % kLines;
    }
    std::snprintf(gLines[slot], sizeof(gLines[slot]), "%s", message);
    th06_psp_overlay_redraw();
}

extern "C" void th06_psp_overlay_redraw()
{
    if (gOverlayStopped || th06_psp_video_pixels() == nullptr)
        return;

    FillRect(0, 4 + kLines * kLineHeight, kBackground);
    for (int i = 0; i < gLineCount; ++i)
    {
        const int index = (gLineHead + i) % kLines;
        const bool newest = (i == gLineCount - 1);
        th06_psp_text_draw(4, 4 + i * kLineHeight, kScale, newest ? kHighlight : kForeground, gLines[index]);
    }
}

extern "C" void th06_psp_overlay_stop()
{
    if (gOverlayStopped)
        return;
    gOverlayStopped = true;
    th06_psp_boot_note("SCREEN HANDED TO ENGINE");
}

extern "C" void th06_psp_diag_frame_presented()
{
    const unsigned int frames = __atomic_add_fetch(&gFramesPresented, 1, __ATOMIC_RELAXED);
    if (frames == 1)
        th06_psp_overlay_stop();
    if (gBootPhaseEnded)
        return;
    th06_psp_watchdog_pet();
    if (frames >= kBootFrames)
    {
        th06_psp_boot_notef("BOOT PHASE END: %u FRAMES PRESENTED", frames);
        gBootPhaseEnded = true;
        th06_psp_watchdog_disarm();
    }
}

extern "C" void th06_psp_runtime_progress()
{
    __atomic_add_fetch(&gRuntimeProgress, 1, __ATOMIC_RELAXED);
}

extern "C" int th06_psp_boot_phase()
{
    return gBootPhaseEnded ? 0 : 1;
}

extern "C" const char *th06_psp_boot_last_note()
{
    return gLastNote;
}

// ------------------------------------------------------------------- watchdog
extern "C" void th06_psp_watchdog_arm(unsigned int seconds)
{
    gWatchdogTimeoutUs = seconds * 1000u * 1000u;
    gWatchdogDeadlineUs = sceKernelGetSystemTimeLow() + gWatchdogTimeoutUs;
    gWatchdogArmed = 1;
    if (gWatchdogThread >= 0)
        return;
    // Above the engine's priority (32) so a spinning engine cannot starve it.
    gWatchdogThread = sceKernelCreateThread("th06_watchdog", WatchdogThread, 0x12, 0x2000,
                                            PSP_THREAD_ATTR_USER, nullptr);
    if (gWatchdogThread >= 0)
        sceKernelStartThread(gWatchdogThread, 0, nullptr);
}

extern "C" void th06_psp_watchdog_pet()
{
    if (gWatchdogArmed)
        gWatchdogDeadlineUs = sceKernelGetSystemTimeLow() + gWatchdogTimeoutUs;
}

extern "C" void th06_psp_watchdog_disarm()
{
    gWatchdogArmed = 0;
}

// --------------------------------------------------------------------- escape
extern "C" void th06_psp_escape_start()
{
    if (gEscapeThread >= 0)
        return;
    ResetAudioFlightInterval();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    // Above the audio output thread (0x10). A saturated audio path must never
    // be able to starve HOME handling or the emergency combo again.
    gEscapeThread = sceKernelCreateThread("th06_escape", EscapeThread, 0x0f, 0x2000,
                                          PSP_THREAD_ATTR_USER, nullptr);
    if (gEscapeThread >= 0)
        sceKernelStartThread(gEscapeThread, 0, nullptr);
}

extern "C" void th06_psp_request_exit(int reason)
{
    if (reason <= 0)
        return;
    int expected = 0;
    __atomic_compare_exchange_n(&gExitRequested, &expected, reason, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

extern "C" void th06_psp_fatal_exit(const char *title, const char *detail)
{
    ExitToXmbAfterNotice(title ? title : "FATAL ERROR", detail ? detail : "UNKNOWN");
}

extern "C" void th06_psp_require_power_off(const char *detail)
{
    th06_psp_watchdog_disarm();
    th06_psp_boot_notef("POWER OFF REQUIRED: %s", detail ? detail : "ME UNACKNOWLEDGED");
    th06_psp_video_init();
    for (;;)
    {
        // Failure path only. The application and all borrowed memory remain
        // loaded; HOME/COMBO/watchdog must not free code beneath a late ME.
        FillRect(0, th06_psp_video_height(), 0xff202060u);
        th06_psp_text_draw(8, 60, 3, 0xffffffffu, "POWER OFF REQUIRED");
        th06_psp_text_draw(8, 110, 2, kHighlight, detail ? detail : "ME UNACKNOWLEDGED");
        th06_psp_text_draw(8, 190, 2, kForeground, "DO NOT START ANOTHER APP");
        sceDisplaySetFrameBuf(const_cast<unsigned int *>(th06_psp_video_pixels()), th06_psp_video_stride(),
                              PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
        scePowerTick(PSP_POWER_TICK_ALL);
        sceKernelDelayThread(250000);
    }
}

extern "C" void __real_sceKernelExitGame(void);
extern "C" void __wrap_sceKernelExitGame(void)
{
    static volatile int exiting;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&exiting, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        for (;;) sceKernelDelayThread(250000);
#if !defined(TH06_PSP_BOOT_PROBE)
#if defined(TH06_PSP_RECORD_SOAK)
    // A normal unattended exit follows the final title, after the last
    // GameManager checkpoint. Preserve cumulative command-drop/failure
    // counts even if no periodic window was printed in that last interval.
    // This is diagnostic exit-only; it cannot serve as game-time ME proof.
    Th06MidiDiag midi{};
    th06_midi_diag_window(&midi);
    th06_psp_boot_notef("RECORD SOAK MIDI FINAL BACKEND %s J%u/%u F%u QD%u FB%u/%u SCR%u/%u/%u PIN%u/%u",
                        MidiModeName(midi.mode), midi.meJobs, midi.scJobs, midi.frames, midi.queueDrops,
                        midi.fallbacks, midi.fallbackReason, midi.scrubPasses, midi.scrubBad,
                        midi.scrubRepaired, midi.pinnedSmfBytes, midi.pinnedBlobBytes);
#endif
    // Exit-only guard, not a branch in the normal audio/render paths.
    th06_psp_me_audio_shutdown();
    if (th06_psp_me_poisoned())
        th06_psp_require_power_off("ME STOP NOT ACKNOWLEDGED");
#endif
    __real_sceKernelExitGame();
    __atomic_store_n(&exiting, 0, __ATOMIC_RELEASE);
}
