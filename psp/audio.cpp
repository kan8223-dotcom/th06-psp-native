#include <pspaudio.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <cstring>

#include "audio_me.h"
#include "midi/MidiPsp.hpp"
#include "diag.hpp"

namespace {
// A 1024-frame hardware block is 23.2 ms at 44.1 kHz.  The old output path
// discarded a complete block whenever the software ring held even one frame
// less than that, which turned ordinary producer jitter into an audible 23 ms
// hole.  512 frames keeps the PSP channel comfortably amortised while cutting
// the recovery granularity in half.
constexpr unsigned int kFramesPerBlock = 512;
constexpr unsigned int kRingFrames = 8192;
constexpr unsigned int kPrefillFrames = 3072;
constexpr unsigned int kChannels = 2;
alignas(64) short gRing[kRingFrames * kChannels];
volatile unsigned int gReadFrame;
volatile unsigned int gWriteFrame;
volatile unsigned int gUnderruns;
volatile unsigned int gDroppedFrames;
volatile unsigned int gMinQueuedFrames;
volatile bool gAlive;
bool gStarted;
int gChannel = -1;
SceUID gOutputThread = -1;

unsigned int RingCount()
{
    const unsigned int write = __atomic_load_n(&gWriteFrame, __ATOMIC_ACQUIRE);
    const unsigned int read = __atomic_load_n(&gReadFrame, __ATOMIC_RELAXED);
    return write >= read ? write - read : kRingFrames - read + write;
}

void RecordMinimumQueued(unsigned int frames)
{
    unsigned int old = __atomic_load_n(&gMinQueuedFrames, __ATOMIC_RELAXED);
    while (frames < old &&
           !__atomic_compare_exchange_n(&gMinQueuedFrames, &old, frames, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
    {
    }
}

int AudioOutputThread(SceSize, void *)
{
    alignas(64) short block[kFramesPerBlock * kChannels];
    bool primed = false;
    while (__atomic_load_n(&gAlive, __ATOMIC_ACQUIRE))
    {
        const unsigned int available = RingCount();
        if (!primed)
        {
            // Do not start the DAC with an empty ring.  The former loop sent
            // silence continuously during boot and alternated audio/silence
            // after every starvation.  Waiting here is not an audio-thread
            // stall: no playable PCM exists yet, and the producer can refill
            // independently.
            if (available < kPrefillFrames)
            {
                sceKernelDelayThread(1000);
                continue;
            }
            primed = true;
        }

        if (available < kFramesPerBlock)
        {
            RecordMinimumQueued(available);
            __atomic_fetch_add(&gUnderruns, 1, __ATOMIC_RELAXED);
            primed = false;
            continue;
        }

        unsigned int read = __atomic_load_n(&gReadFrame, __ATOMIC_RELAXED);
        for (unsigned int frame = 0; frame < kFramesPerBlock; ++frame)
        {
            const unsigned int src = ((read + frame) % kRingFrames) * kChannels;
            block[frame * kChannels] = gRing[src];
            block[frame * kChannels + 1] = gRing[src + 1];
        }
        __atomic_store_n(&gReadFrame, (read + kFramesPerBlock) % kRingFrames, __ATOMIC_RELEASE);
        RecordMinimumQueued(available - kFramesPerBlock);
        sceAudioOutputBlocking(gChannel, PSP_AUDIO_VOLUME_MAX, block);
    }
    sceKernelExitDeleteThread(0);
    return 0;
}
} // namespace

extern "C" int th06_psp_audio_init(void)
{
    if (gStarted)
        return 0;
    std::memset(gRing, 0, sizeof(gRing));
    gReadFrame = gWriteFrame = gUnderruns = 0;
    gDroppedFrames = 0;
    gMinQueuedFrames = kRingFrames;
    gChannel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, kFramesPerBlock, PSP_AUDIO_FORMAT_STEREO);
    if (gChannel < 0)
        return -1;
    th06_psp_me_audio_init();
    if (th06_psp_me_poisoned())
        th06_psp_require_power_off("ME BOOT SELFTEST NOT ACKNOWLEDGED");
    // MIDI players (psp/midi): needs the ME worker state decided above and
    // runs before the BGM producer thread exists.
    th06_midi_audio_attach();
    __atomic_store_n(&gAlive, true, __ATOMIC_RELEASE);
    gOutputThread = sceKernelCreateThread("th06_audio_out", AudioOutputThread, 0x10, 0x4000,
                                          PSP_THREAD_ATTR_USER, nullptr);
    if (gOutputThread < 0 || sceKernelStartThread(gOutputThread, 0, nullptr) < 0)
    {
        __atomic_store_n(&gAlive, false, __ATOMIC_RELEASE);
        th06_psp_me_audio_shutdown();
        sceAudioChRelease(gChannel);
        gChannel = -1;
        return -1;
    }
    gStarted = true;
    return 0;
}

extern "C" void th06_psp_audio_queue(const short *pcm, unsigned int bytes)
{
    if (!gStarted || !pcm)
        return;
    const unsigned int frames = bytes / (sizeof(short) * kChannels);
    unsigned int write = __atomic_load_n(&gWriteFrame, __ATOMIC_RELAXED);
    const unsigned int read = __atomic_load_n(&gReadFrame, __ATOMIC_ACQUIRE);
    const unsigned int used = write >= read ? write - read : kRingFrames - read + write;
    const unsigned int free = kRingFrames - 1 - used;
    const unsigned int copyFrames = frames < free ? frames : free;
    if (copyFrames < frames)
        __atomic_fetch_add(&gDroppedFrames, frames - copyFrames, __ATOMIC_RELAXED);
    for (unsigned int frame = 0; frame < copyFrames; ++frame)
    {
        const unsigned int dst = ((write + frame) % kRingFrames) * kChannels;
        gRing[dst] = pcm[frame * kChannels];
        gRing[dst + 1] = pcm[frame * kChannels + 1];
    }
    __atomic_store_n(&gWriteFrame, (write + copyFrames) % kRingFrames, __ATOMIC_RELEASE);
}

extern "C" unsigned int th06_psp_audio_queued_bytes(void)
{
    if (!gStarted)
        return 0;
    // SDL_GetQueuedAudioSize is the amount still owned by the software queue.
    // The old extra-block estimate made the producer believe it had 23 ms more
    // PCM than it really did and stop feeding the ring too early.
    return RingCount() * kChannels * sizeof(short);
}

extern "C" void th06_psp_audio_diag(unsigned int *queuedFrames, unsigned int *underruns,
                                      unsigned int *readFrame, unsigned int *writeFrame)
{
    if (queuedFrames)
        *queuedFrames = gStarted ? RingCount() : 0;
    if (underruns)
        *underruns = __atomic_load_n(&gUnderruns, __ATOMIC_RELAXED);
    if (readFrame)
        *readFrame = __atomic_load_n(&gReadFrame, __ATOMIC_RELAXED);
    if (writeFrame)
        *writeFrame = __atomic_load_n(&gWriteFrame, __ATOMIC_RELAXED);
}

extern "C" void th06_psp_audio_diag_window(unsigned int *minQueuedFrames, unsigned int *droppedFrames)
{
    const unsigned int current = gStarted ? RingCount() : 0;
    unsigned int minimum = __atomic_exchange_n(&gMinQueuedFrames, current, __ATOMIC_ACQ_REL);
    if (minimum == kRingFrames)
        minimum = current;
    if (minQueuedFrames)
        *minQueuedFrames = minimum;
    if (droppedFrames)
        *droppedFrames = __atomic_exchange_n(&gDroppedFrames, 0, __ATOMIC_ACQ_REL);
}

extern "C" void th06_psp_audio_shutdown(void)
{
    if (!gStarted)
        return;
    __atomic_store_n(&gAlive, false, __ATOMIC_RELEASE);
    if (gOutputThread >= 0)
        sceKernelWaitThreadEnd(gOutputThread, nullptr);
    th06_psp_me_audio_shutdown();
    sceAudioChRelease(gChannel);
    gOutputThread = -1;
    gChannel = -1;
    gStarted = false;
}
