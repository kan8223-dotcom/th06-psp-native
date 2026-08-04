// PSP platform entry.  It deliberately contains no original TH06 data and no
// CD/UMD access; the engine main is compiled unchanged under a private name.
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsdk.h>
#include <pspsysmem.h>
#include <cstdio>

#include "diag.hpp"
#include "fileio.hpp"
#include "memory.hpp"
#include "video.hpp"

PSP_MODULE_INFO("TH06 PSP bootstrap", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
// PSPPMD's proven MECC layout leaves 2 MiB outside newlib's heap for the ME
// bridge/stack and kernel allocations.  A negative size means "all but".
PSP_HEAP_SIZE_KB(-2048);

// Boot has to clear config, window, renderer, sound, input and chain setup.
// Thirty seconds is far longer than any of those legitimately take, so the
// watchdog only ever fires on a real stall.
static const unsigned int kBootWatchdogSeconds = 30;

namespace {
volatile int gRunning = 1;

unsigned int ReadFcr31()
{
    unsigned int value = 0;
    asm volatile("cfc1 %0, $31" : "=r"(value));
    return value;
}

int ExitCallback(int, int, void *)
{
    // Do not perform filesystem I/O or sceKernelExitGame in callback context.
    // Publish the request to the higher-priority diagnostic thread, which can
    // record whether the callback actually arrived before exiting.
    gRunning = 0;
    th06_psp_request_exit(1);
    return 0;
}

int CallbackThread(SceSize, void *)
{
    const int callback = sceKernelCreateCallback("exit_cb", ExitCallback, nullptr);
    const int registered = callback >= 0 ? sceKernelRegisterExitCallback(callback) : callback;
    th06_psp_boot_notef("HOME CB READY C%d R%d", callback, registered);
    const int sleepResult = registered >= 0 ? sceKernelSleepThreadCB() : registered;
    th06_psp_boot_notef("HOME CB WAKE %d", sleepResult);
    return 0;
}

void InstallCallbacks()
{
    // Exact Moonlight H.264 proven callback-thread shape.
    const SceUID thread = sceKernelCreateThread("cb_thread", CallbackThread, 0x0f, 0x1000,
                                                PSP_THREAD_ATTR_USER, nullptr);
    const int started = thread >= 0 ? sceKernelStartThread(thread, 0, nullptr) : thread;
    th06_psp_boot_notef("HOME THREAD C%d S%d", thread, started);
}

// The build stamp is on screen and in the log because a stale EBOOT that
// silently failed to rebuild has already cost us one hardware run.
void RecordBootHeader()
{
    th06_psp_boot_notef("BUILD %s %s", __DATE__, __TIME__);
    th06_psp_boot_notef("CLOCK %dMHz BUS %dMHz", scePowerGetCpuClockFrequency(),
                        scePowerGetBusClockFrequency());
    // Read after newlib has already taken its heap, so this is what is left for
    // the kernel and for any later partition allocation, not the heap size.
    th06_psp_boot_notef("SYSMEM LEFT %uKB MAXBLOCK %uKB",
                        static_cast<unsigned int>(sceKernelTotalFreeMemSize() / 1024),
                        static_cast<unsigned int>(sceKernelMaxFreeMemSize() / 1024));
    const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
    th06_psp_boot_notef("HEAP A%uKB U%uKB F%uKB TOP%uKB B%u", heap.arenaBytes / 1024u,
                        heap.usedBytes / 1024u, heap.freeBytes / 1024u, heap.topFreeBytes / 1024u,
                        heap.freeBlocks);
}

#if !defined(TH06_PSP_BOOT_PROBE)
// Keeps a terminal screen readable instead of dropping straight back to the
// XMB, while still leaving on its own so nobody has to pull the battery.
void HoldResultScreen(unsigned int seconds)
{
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    for (unsigned int frame = 0; gRunning && frame < seconds * 60; ++frame)
    {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & (PSP_CTRL_START | PSP_CTRL_CROSS))
            break;
        sceDisplayWaitVblankStart();
    }
}
#endif

} // namespace

#if !defined(TH06_PSP_BOOT_PROBE)
extern int th06_engine_main(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
    // TH06 was authored for the Windows default floating-point environment,
    // where invalid/divide-by-zero/overflow exceptions are masked.  A real
    // PSP starts this thread with those three enables set (FCR31 0x00000e00).
    // PPSSPP reproduces that register value but does not raise the corresponding
    // CPU traps, so an unmodified build behaves differently on hardware.  Use
    // PSPSDK's own compatibility helper before any engine/backend calculation
    // and retain the before/after values for the boot log.  Existing finite/zero
    // guards remain necessary: masking a trap must not allow NaNs into GE data.
    const unsigned int fcr31Before = ReadFcr31();
    pspSdkDisableFPUExceptions();
    const unsigned int fcr31After = ReadFcr31();

    // Bind relative assets and diagnostics to the EBOOT that XMB actually
    // launched before any subsystem opens a file.  This also keeps ms0/ef0
    // installs from shadowing one another on PSP Go.
    th06_psp_fileio_set_launch_path(argc > 0 ? argv[0] : nullptr);
    scePowerSetClockFrequency(333, 333, 166);

    // Video first: the boot overlay and the exception dump both write straight
    // into the display buffer, so everything after this point is visible on the
    // device itself even with no USB cable and no working HOME button.
    th06_psp_video_init();
    th06_psp_video_fill(0xff202020u);
    th06_psp_fileio_init();
    th06_psp_install_oom_handler();

    InstallCallbacks();
    th06_psp_escape_start();
    RecordBootHeader();
    th06_psp_boot_notef("FPU FCR31 %08X>%08X", fcr31Before, fcr31After);
    th06_psp_boot_note("HOLD L+R+START TO QUIT");
    th06_psp_watchdog_arm(kBootWatchdogSeconds);

#if defined(TH06_PSP_BOOT_PROBE)
    // Deliberately excludes every engine translation unit.  It proves whether
    // the device reaches main() before we investigate TH06 global ctors.
    //
    // TRIANGLE faults on purpose.  We cannot install our own exception handler
    // from user mode, so this is how we learn what this CFW actually does with
    // a bad store - dump screen, silent return to the XMB, or freeze - which is
    // what tells us how to read the body's behaviour later.
    th06_psp_boot_note("BOOT PROBE MAIN OK");

#if defined(TH06_PSP_SELFTEST)
    // make BOOT_PROBE=1 SELFTEST=1 — proves the watchdog can still preempt a
    // main thread that is spinning with no kernel calls left in it, which is
    // the exact situation it exists for.  Expected: WATCHDOG TIMEOUT in the log
    // and an automatic return to the XMB.
    th06_psp_boot_note("SELFTEST: DELIBERATE HANG");
    th06_psp_watchdog_arm(5);
    for (;;)
    {
    }
#endif

    th06_psp_boot_note("TRIANGLE = DELIBERATE FAULT TEST");
    th06_psp_watchdog_disarm();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    while (gRunning)
    {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_TRIANGLE)
        {
            th06_psp_boot_note("DELIBERATE FAULT");
            *reinterpret_cast<volatile unsigned int *>(0x00000000u) = 0xdeadbeefu;
        }
        if (pad.Buttons & (PSP_CTRL_START | PSP_CTRL_CROSS))
            break;
        sceDisplayWaitVblankStart();
    }
    sceKernelExitGame();
    return 0;
#else
    if (!th06_psp_original_data_ready())
    {
        th06_psp_boot_note("ORIGINAL TH6 1.02f DATA REQUIRED");
        th06_psp_boot_note("COPY PC TH6 FOLDER AS TH06PSP/th6");
        th06_psp_boot_note("NO GAME DATA IS INCLUDED");
        th06_psp_watchdog_disarm();
        HoldResultScreen(15);
        sceKernelExitGame();
        return 2;
    }
    th06_psp_boot_note("ENGINE ENTER");

    const int result = th06_engine_main(argc, argv);
    th06_psp_watchdog_disarm();
    th06_psp_boot_notef("ENGINE RETURN %d", result);

    // A development build must not vanish the instant a data file is missing.
    // If the engine never took over the screen the overlay still holds the full
    // boot record, so leave it up; otherwise say plainly that the run ended.
    if (th06_psp_video_present_count() != 0)
    {
        th06_psp_video_fill(0xff602020u);
        th06_psp_text_draw(8, 100, 3, 0xffffffffu, "ENGINE RETURNED");
        th06_psp_text_draw(8, 140, 2, 0xffd0d0d0u, "PRESS START");
    }
    HoldResultScreen(15);

    sceKernelExitGame();
    return result;
#endif
}
