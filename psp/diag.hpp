#pragma once

// PSP bring-up diagnostics.
//
// Everything declared here is *boot scoped*: the on-screen overlay, the fopen
// trace and the watchdog all stop the moment the engine presents its first
// frame, so the running game never pays for them.  The one permanent resident
// is the escape thread, which only reads the pad every 100 ms at a priority
// above the engine so the device can always be returned to the XMB.

extern "C"
{

    // ---------------------------------------------------------------- record
    // Every note is timestamped, appended to <storage>:/TH06PSP_BOOT.LOG, kept
    // in a small ring for the crash/watchdog reports, and drawn on screen while
    // the boot overlay is active.
    void th06_psp_boot_note(const char *message);
    void th06_psp_boot_notef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    const char *th06_psp_boot_last_note();

    // ------------------------------------------------------------------ text
    // 4x6 bitmap glyphs written straight into the display buffer.  No GU, no
    // allocation and no kernel call, so it is also usable from an exception.
    void th06_psp_text_draw(int x, int y, int scale, unsigned int color, const char *text);
    void th06_psp_text_hex(int x, int y, int scale, unsigned int color, unsigned int value);

    // --------------------------------------------------------------- overlay
    // Two separate lifetimes, because the engine presents its first frame while
    // it is still initialising: the overlay must yield the screen immediately,
    // but the record has to keep running long enough to catch sound, input and
    // chain setup.
    void th06_psp_overlay_push(const char *message);
    void th06_psp_overlay_redraw();
    void th06_psp_overlay_stop(); // screen only; first presented frame
    void th06_psp_diag_frame_presented();
    // Runtime work that legitimately takes place without a present (archive
    // decode, image conversion, texture allocation) resets the stall clock.
    // Heartbeat/log writes themselves must not call this.
    void th06_psp_runtime_progress();
    int th06_psp_boot_phase(); // 1 until the engine has presented steadily

    // Five-second PERF/audio telemetry is intentionally RAM-only in the
    // normal build.  Creating/opening/closing the FAT log beside a streaming
    // WAV can serialize the Memory Stick driver and manufacture an underrun.
    // Put TH06PSP_DIAG.ON beside EBOOT.PBP only for a dedicated measurement
    // run; boot, transition and failure notes remain recorded regardless.
    int th06_psp_periodic_diag_enabled();

    // Audio telemetry is sampled into a RAM flight recorder.  Normal gameplay
    // never writes it to the Memory Stick.  Transition/failure wrappers may
    // emit one compact checkpoint; `always == 0` suppresses the write when the
    // interval contains no underrun, producer/I/O stall, cache miss, drop or
    // ME timeout.
    void th06_psp_audio_diag_checkpoint(const char *tag, int always);

    // GE timing is likewise retained in RAM and written only while leaving
    // the application, never in the rendering or streaming hot path.
    void th06_psp_ge_diag_checkpoint(const char *tag);

    // -------------------------------------------------------------- watchdog
    // Boot only.  A boot that stops making progress reports its last note and
    // returns to the XMB instead of forcing a hardware shutdown.
    void th06_psp_watchdog_arm(unsigned int seconds);
    void th06_psp_watchdog_pet();
    void th06_psp_watchdog_disarm();

    // ---------------------------------------------------------------- escape
    // L + R + START held for ~1 s exits to the XMB even if the engine's own
    // thread is wedged.  Runs above the engine's priority so it always gets
    // scheduled.
    void th06_psp_escape_start();

    // HOME callback cannot safely do filesystem work itself. It publishes an
    // in-memory request which the high-priority diagnostic thread records and
    // executes outside callback context.
    void th06_psp_request_exit(int reason);

    // Fatal paths that still have a functioning kernel can leave a readable
    // screen and return to XMB without relying on the engine or GE.
    void th06_psp_fatal_exit(const char *title, const char *detail);

}

// Why there is no CPU exception dump here: pspDebugInstallErrorHandler needs
// sceKernelRegisterDefaultExceptionHandler, which lives in libpspkernel and is
// a kernel-mode import.  Linking it into this user-mode EBOOT would fail module
// load outright (0x8002013C), which is the IMPORT_START scar all over again.
// Crash and hang are told apart by the log instead: a hang always leaves a
// WATCHDOG TIMEOUT line, a crash leaves the log ending mid-stage with no such
// line and the device back at the XMB.
