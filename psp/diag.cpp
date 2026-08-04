// Boot overlay, boot watchdog and escape thread.
//
// Background: first-light testing on hardware produced a single blue screen
// with no way to tell a crash from a hang, no way to read a stage without
// unplugging the device, and no way back to the XMB except a forced shutdown.
// The three facilities here remove all of that, and all three stop mattering
// once the engine presents a frame.
#include "diag.hpp"

#include <pspctrl.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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
extern "C" void th06_psp_me_audio_diag_window(unsigned int *jobs, unsigned int *fallbacks,
                                                unsigned int *timeouts, unsigned int *maxWaitUs);

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
    for (;;)
    {
        sceKernelDelayThread(250 * 1000);
        if (!gWatchdogArmed)
            continue;
        const unsigned int now = sceKernelGetSystemTimeLow();
        if (static_cast<int>(now - gWatchdogDeadlineUs) < 0)
            continue;
        gWatchdogArmed = 0;
        th06_psp_boot_notef("WATCHDOG TIMEOUT %us AFTER: %s", gWatchdogTimeoutUs / 1000000u, gLastNote);
        ExitToXmbAfterNotice("BOOT WATCHDOG", gLastNote);
        return 0;
    }
}

int EscapeThread(SceSize, void *)
{
    const unsigned int combo = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_START;
    int heldTicks = 0;
    unsigned int lastFrames = __atomic_load_n(&gFramesPresented, __ATOMIC_RELAXED);
    unsigned int lastRuntimeProgress = __atomic_load_n(&gRuntimeProgress, __ATOMIC_RELAXED);
    unsigned int lastHeartbeatFrames = lastFrames;
    unsigned int lastAudioUnderruns = 0;
    unsigned int lastProgressUs = sceKernelGetSystemTimeLow();
    unsigned int lastHeartbeatUs = lastProgressUs;
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

        if (gBootPhaseEnded && static_cast<unsigned int>(now - lastHeartbeatUs) >= kRuntimeHeartbeatUs)
        {
            unsigned int queued = 0;
            unsigned int underruns = 0;
            unsigned int readFrame = 0;
            unsigned int writeFrame = 0;
            th06_psp_audio_diag(&queued, &underruns, &readFrame, &writeFrame);
            unsigned int minQueued = 0;
            unsigned int droppedFrames = 0;
            unsigned int maxProducerGapUs = 0;
            unsigned int maxMixUs = 0;
            unsigned int maxBgmReadUs = 0;
            unsigned int mixCalls = 0;
            unsigned int meJobs = 0;
            unsigned int meFallbacks = 0;
            unsigned int meTimeouts = 0;
            unsigned int meMaxWaitUs = 0;
            th06_psp_audio_diag_window(&minQueued, &droppedFrames);
            th06_psp_sound_diag_window(&maxProducerGapUs, &maxMixUs, &maxBgmReadUs, &mixCalls);
            th06_psp_me_audio_diag_window(&meJobs, &meFallbacks, &meTimeouts, &meMaxWaitUs);
            const unsigned int underrunDelta = underruns - lastAudioUnderruns;
            lastAudioUnderruns = underruns;
            const unsigned int deltaFrames = frames - lastHeartbeatFrames;
            if (th06_psp_periodic_diag_enabled())
            {
                th06_psp_boot_notef("RUN F%u +%u Q%u U%u RW%u/%u MEM%u/%uKB", frames, deltaFrames, queued,
                                    underruns, readFrame, writeFrame,
                                    static_cast<unsigned int>(sceKernelTotalFreeMemSize() / 1024),
                                    static_cast<unsigned int>(sceKernelMaxFreeMemSize() / 1024));
                th06_psp_boot_notef("AUD U+%u MIN%u DROP%u GAP%uus MIX%uus IO%uus N%u", underrunDelta,
                                    minQueued, droppedFrames, maxProducerGapUs, maxMixUs, maxBgmReadUs, mixCalls);
                th06_psp_boot_notef("ME AUD J%u SC%u TO%u W%uus", meJobs, meFallbacks, meTimeouts,
                                    meMaxWaitUs);
            }
            lastHeartbeatFrames = frames;
            lastHeartbeatUs = now;
        }

        if (gBootPhaseEnded && static_cast<unsigned int>(now - lastProgressUs) >= kRuntimeStallUs &&
            __atomic_load_n(&gExitRequested, __ATOMIC_ACQUIRE) == 0)
        {
            th06_psp_boot_notef("RUNTIME STALL 8s AT F%u", frames);
            th06_psp_request_exit(3);
        }

        const int exitReason = __atomic_exchange_n(&gExitRequested, 0, __ATOMIC_ACQ_REL);
        if (exitReason != 0)
        {
            const char *name = exitReason == 1 ? "HOME" : (exitReason == 2 ? "COMBO" : "STALL");
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
