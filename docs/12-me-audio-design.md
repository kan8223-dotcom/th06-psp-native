# TH06 PSP: ME audio worker design

Status: implementation contract, 2026-08-04.

## Proven baseline

The ME integration baseline is the public PSPPMD implementation at
`kan8223-dotcom/psp-pmdvis`, commit `18fb0b1`.  Vendor its published MECC
library and use its `meLibDefaultInit` entry point.  Do not invent a raw kcall
path, require a manually installed seplugin, or modify the ME mapper tables.

The useful PSPPMD division of labour is retained:

- ME performs only deterministic PCM arithmetic.
- A high-priority SC thread remains the sole owner of `sceAudioOutputBlocking`.
- SC and ME communicate through one 64-byte-aligned uncached mailbox.
- PCM output is passed through an aligned ring.  The game/render thread never
  calls an audio syscall.
- ME initialization, self-test, or runtime timeout failure permanently selects
  the existing SC mixer for that boot.

This is an optional accelerator and isolation layer.  Audio must remain fully
functional when ME is absent (including PPSSPP).

PPSSPP v1.20.4 does not reliably return an error from MECC initialization: it
can stop after the embedded bridge is extracted and before
`meLibDefaultInit()` returns.  Therefore the standard
`ms0:/PSP/SYSTEM/ppsspp.ini` marker selects the SC fallback before touching
MECC.  A stale copied marker only disables the optional worker, which is the
safe failure direction.  Real hardware logs `ME AUDIO INIT BEGIN (PSPPMD)`
immediately before entering the public PSPPMD initialization path.

## Measurement boundary

The real-hardware log before this work shows that mixer maxima almost equal
the 64 KiB WAV read maxima: typical `MIX 18--21 ms / IO 17--20 ms`, with a
worst observed `MIX 242 ms / IO 242 ms`.  Therefore ME mixing cannot repair
memory-stick latency.  The BGM read-ahead window must be enlarged separately;
claiming ME fixed an I/O underrun is forbidden.

The intended benefit is narrower: sample mixing remains independent of SC
render load, and the same worker can later absorb a decoder without changing
the output thread.

## Job format and ownership

Each job contains at most 1024 stereo output frames and a fixed array of input
descriptors.  A descriptor identifies an aligned physical source address,
frame count, destination frame offset, mono/stereo layout, and single-precision
gain.  SC resolves BGM block/loop boundaries and advances engine state; ME only
adds samples and clamps/divides the final mix.

Source ownership rules:

- SFX samples are immutable after load and are written back once at load time.
- A BGM cache block is written back after every refill and is not reused until
  the outstanding job completes.
- ME invalidates each cached source range before reading it.
- ME writes to a dedicated output buffer and writes that range back.  SC
  invalidates it before queueing to the existing software ring.
- ME uses local eDRAM for the temporary 32-bit accumulation buffer.  It never
  touches GE eDRAM or calls GU/GE APIs.

The fixed local-eDRAM address is audited, not guessed.  PSPPMD commit
`18fb0b1` uses `0x00000400` for its 2 KiB PCM work buffer.  TH06 retains both
the base and the exact proven byte footprint: a larger job is processed as
256-frame stereo 32-bit chunks, repeatedly using `[0x00000400, 0x00000c00)`.
MECC executes application code from main RAM and initializes the local stack
at `0x00200000` on PSP-1000 or `0x00400000` on later models, growing downward.
Compile-time assertions pin the work range; changing chunk size requires a new
layout audit.

## Failure containment

Before enabling the worker, SC and ME mix the same deterministic vector and
the PCM results are compared byte-for-byte.  A mismatch leaves ME disabled.

Every dispatched job has a bounded wait.  On timeout:

1. mark ME unavailable for the remainder of the boot;
2. mix the unchanged job on SC;
3. queue the SC-owned output buffer;
4. never reuse the possibly late ME output buffer.

This makes a late or wedged ME harmless to audio data and to HOME/watchdog
handling.  Shutdown also has a bounded wait; application exit may not wait
forever for ME.

Diagnostics record `ME AUDIO ON/OFF`, mapper result, self-test result, ME job
count, fallback count, timeout count, and maximum job wait.  PPSSPP validates
only the SC fallback.  ME acceptance requires real PSP output and logs.

## Delivery sequence

1. Keep the already deployed DRAW-writeback/stage-transition diagnostic build
   unchanged until its real run is collected.
2. Enlarge BGM read-ahead and verify underrun count independently.
3. Integrate the PSPPMD ME worker with SC fallback enabled by default.
4. Verify PPSSPP SC fallback, then real PSP ME self-test and continuous audio.
5. Compare identical five-second diagnostics before/after.  Do not infer an
   FPS gain from the presence of `ME AUDIO ON`.
