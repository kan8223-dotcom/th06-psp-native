#include "audio_me.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

// Exact MECC baseline shipped by the public PSPPMD project.  The application
// uses only its documented init/cache/halt surface; no raw kcall path lives in
// TH06 code.
void meLibOnProcess(void);
#include "me-core.h"

extern void th06_psp_boot_note(const char *message);
extern void th06_psp_boot_notef(const char *fmt, ...);
extern const char *th06_psp_game_dir(void);

enum
{
    ME_AUDIO_CMD_NONE = 0,
    ME_AUDIO_CMD_MIX = 1,
    ME_AUDIO_CMD_STOP = 0xff,
    ME_AUDIO_STAT_IDLE = 0,
    ME_AUDIO_STAT_DONE = 1,
    ME_AUDIO_WAIT_US = 8000,

    // PSPPMD commit 18fb0b1 uses exactly 2 KiB from ME local eDRAM starting at
    // 0x00000400.  Process a larger TH06 job in 256-frame chunks so our 32-bit
    // stereo accumulator stays inside that same proven byte range.
    ME_AUDIO_EDRAM_ACCUM_BASE = 0x00000400,
    ME_AUDIO_EDRAM_ACCUM_FRAMES = 256,
    ME_AUDIO_EDRAM_ACCUM_BYTES = ME_AUDIO_EDRAM_ACCUM_FRAMES * 2 * sizeof(int),
    ME_AUDIO_EDRAM_ACCUM_END = ME_AUDIO_EDRAM_ACCUM_BASE + ME_AUDIO_EDRAM_ACCUM_BYTES,
    ME_AUDIO_EDRAM_FAT_STACK_TOP = 0x00200000
};

_Static_assert(ME_AUDIO_EDRAM_ACCUM_END == 0x00000c00,
               "ME audio eDRAM footprint changed; re-audit PSPPMD/MECC layout");
_Static_assert(ME_AUDIO_EDRAM_ACCUM_END < ME_AUDIO_EDRAM_FAT_STACK_TOP,
               "ME audio accumulator collides with the smallest MECC local stack");

typedef struct MeMixInput
{
    unsigned int sourcePhys;
    unsigned int frames;
    unsigned int destinationFrame;
    unsigned int channels;
    float gain;
} MeMixInput;

typedef struct MeAudioMailbox
{
    volatile unsigned int command;
    volatile unsigned int status;
    unsigned int frames;
    unsigned int inputCount;
    unsigned int mixDivisor;
    unsigned int outputPhys;
    volatile unsigned int completedJobs;
    unsigned int reserved;
    MeMixInput inputs[TH06_PSP_ME_MAX_MIX_INPUTS];
} MeAudioMailbox;

static volatile MeAudioMailbox gMeMailbox __attribute__((aligned(64), section(".uncached")));
static volatile MeAudioMailbox *gMeMailboxUncached;
static short gMeOutput[TH06_PSP_ME_MAX_MIX_FRAMES * 2] __attribute__((aligned(64)));
static int gScWide[TH06_PSP_ME_MAX_MIX_FRAMES * 2] __attribute__((aligned(64)));
static volatile int gMeAudioActive;
static volatile unsigned int gMeJobs;
static volatile unsigned int gMeFallbacks;
static volatile unsigned int gMeTimeouts;
static volatile unsigned int gMeMaxWaitUs;

static int running_under_ppsspp(void)
{
    // PPSSPP's standard memstick layout owns this configuration file.  A
    // copied marker can only cause the safe SC path; it can never enable ME on
    // an unsupported machine.  Do this before meLibDefaultInit(): PPSSPP v1.20
    // can block inside the embedded kernel bridge instead of returning an
    // error that our normal fallback could consume.
    SceIoStat stat;
    return sceIoGetstat("ms0:/PSP/SYSTEM/ppsspp.ini", &stat) >= 0;
}

static int me_disabled_marker_present(void)
{
    const char *gameDir = th06_psp_game_dir();
    char path[256];
    SceIoStat stat;
    if (!gameDir || snprintf(path, sizeof(path), "%s/TH06PSP_ME.OFF", gameDir) < 0)
        return 0;
    return sceIoGetstat(path, &stat) >= 0;
}

static void record_max(volatile unsigned int *value, unsigned int sample)
{
    unsigned int old = __atomic_load_n(value, __ATOMIC_RELAXED);
    while (sample > old &&
           !__atomic_compare_exchange_n(value, &old, sample, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    {
    }
}

static void mix_on_sc(const Th06PspMixJob *job, short *output)
{
    const unsigned int frames = job->frames <= TH06_PSP_ME_MAX_MIX_FRAMES
                                    ? job->frames
                                    : TH06_PSP_ME_MAX_MIX_FRAMES;
    const unsigned int samples = frames * 2;
    memset(gScWide, 0, samples * sizeof(gScWide[0]));

    unsigned int inputCount = job->inputCount;
    if (inputCount > TH06_PSP_ME_MAX_MIX_INPUTS)
        inputCount = TH06_PSP_ME_MAX_MIX_INPUTS;
    for (unsigned int inputIndex = 0; inputIndex < inputCount; ++inputIndex)
    {
        const Th06PspMixInput *input = &job->inputs[inputIndex];
        if (!input->samples || input->destinationFrame >= frames ||
            (input->channels != 1 && input->channels != 2))
            continue;
        unsigned int inputFrames = input->frames;
        if (inputFrames > frames - input->destinationFrame)
            inputFrames = frames - input->destinationFrame;
        int *destination = gScWide + input->destinationFrame * 2;
        if (input->channels == 1)
        {
            for (unsigned int frame = 0; frame < inputFrames; ++frame)
            {
                const int value = (int)(input->samples[frame] * input->gain);
                destination[frame * 2] += value;
                destination[frame * 2 + 1] += value;
            }
        }
        else
        {
            for (unsigned int frame = 0; frame < inputFrames; ++frame)
            {
                destination[frame * 2] += (int)(input->samples[frame * 2] * input->gain);
                destination[frame * 2 + 1] += (int)(input->samples[frame * 2 + 1] * input->gain);
            }
        }
    }

    const int divisor = job->mixDivisor ? (int)job->mixDivisor : 1;
    for (unsigned int sample = 0; sample < samples; ++sample)
        output[sample] = (short)(gScWide[sample] / divisor);
}

// Runs on the Media Engine.  Like PSPPMD, use local eDRAM for the wide
// accumulation buffer and write the finished block back to aligned main RAM.
void meLibOnProcess(void)
{
    volatile MeAudioMailbox *box =
        (volatile MeAudioMailbox *)(0x40000000u | (unsigned int)&gMeMailbox);
    // Keep PSPPMD's proven 1 KiB base.  Materialize it through a register so
    // the compiler does not treat this hardware address as a C null object.
    unsigned int wideAddress = ME_AUDIO_EDRAM_ACCUM_BASE;
    __asm__ volatile("" : "+r"(wideAddress));
    int *wide = (int *)wideAddress;

    meLibDcacheWritebackInvalidateAll();
    meLibIcacheInvalidateAll();

    for (;;)
    {
        while (box->command == ME_AUDIO_CMD_NONE)
            __asm__ volatile("nop; nop; nop; nop;");

        if (box->command == ME_AUDIO_CMD_STOP)
        {
            box->status = ME_AUDIO_STAT_DONE;
            meLibHalt();
            return;
        }

        if (box->command != ME_AUDIO_CMD_MIX)
        {
            box->command = ME_AUDIO_CMD_NONE;
            continue;
        }

        unsigned int frames = box->frames;
        if (frames > TH06_PSP_ME_MAX_MIX_FRAMES)
            frames = TH06_PSP_ME_MAX_MIX_FRAMES;
        unsigned int inputCount = box->inputCount;
        if (inputCount > TH06_PSP_ME_MAX_MIX_INPUTS)
            inputCount = TH06_PSP_ME_MAX_MIX_INPUTS;
        short *output = (short *)(0x80000000u | box->outputPhys);
        const int divisor = box->mixDivisor ? (int)box->mixDivisor : 1;
        for (unsigned int chunkStart = 0; chunkStart < frames;
             chunkStart += ME_AUDIO_EDRAM_ACCUM_FRAMES)
        {
            unsigned int chunkFrames = frames - chunkStart;
            if (chunkFrames > ME_AUDIO_EDRAM_ACCUM_FRAMES)
                chunkFrames = ME_AUDIO_EDRAM_ACCUM_FRAMES;
            const unsigned int chunkEnd = chunkStart + chunkFrames;
            const unsigned int chunkSamples = chunkFrames * 2;
            for (unsigned int sample = 0; sample < chunkSamples; ++sample)
                wide[sample] = 0;

            for (unsigned int inputIndex = 0; inputIndex < inputCount; ++inputIndex)
            {
                const MeMixInput *input = (const MeMixInput *)&box->inputs[inputIndex];
                if (!input->sourcePhys || input->destinationFrame >= frames ||
                    (input->channels != 1 && input->channels != 2))
                    continue;
                unsigned int inputFrames = input->frames;
                if (inputFrames > frames - input->destinationFrame)
                    inputFrames = frames - input->destinationFrame;
                const unsigned int inputStart = input->destinationFrame;
                const unsigned int inputEnd = inputStart + inputFrames;
                const unsigned int overlapStart = inputStart > chunkStart ? inputStart : chunkStart;
                const unsigned int overlapEnd = inputEnd < chunkEnd ? inputEnd : chunkEnd;
                if (overlapStart >= overlapEnd)
                    continue;

                const unsigned int sourceFrame = overlapStart - inputStart;
                const unsigned int mixFrames = overlapEnd - overlapStart;
                const short *source = (const short *)(0x80000000u | input->sourcePhys) +
                                      sourceFrame * input->channels;
                const unsigned int sourceStart = (unsigned int)source & ~63u;
                const unsigned int sourceEnd =
                    ((unsigned int)source + mixFrames * input->channels * sizeof(short) + 63u) & ~63u;
                meLibDcacheInvalidateRange(sourceStart, sourceEnd - sourceStart);
                int *destination = wide + (overlapStart - chunkStart) * 2;
                if (input->channels == 1)
                {
                    for (unsigned int frame = 0; frame < mixFrames; ++frame)
                    {
                        const int value = (int)(source[frame] * input->gain);
                        destination[frame * 2] += value;
                        destination[frame * 2 + 1] += value;
                    }
                }
                else
                {
                    for (unsigned int frame = 0; frame < mixFrames; ++frame)
                    {
                        destination[frame * 2] += (int)(source[frame * 2] * input->gain);
                        destination[frame * 2 + 1] += (int)(source[frame * 2 + 1] * input->gain);
                    }
                }
            }

            short *chunkOutput = output + chunkStart * 2;
            for (unsigned int sample = 0; sample < chunkSamples; ++sample)
                chunkOutput[sample] = (short)(wide[sample] / divisor);
        }
        meLibDcacheWritebackRange((unsigned int)output, frames * 2 * sizeof(short));
        box->completedJobs++;
        __asm__ volatile("sync");
        box->command = ME_AUDIO_CMD_NONE;
        __asm__ volatile("sync");
        // Publish DONE last.  SC seeing DONE now proves ME has returned the
        // command slot and cannot erase a newly dispatched job with a late
        // command=NONE store.
        box->status = ME_AUDIO_STAT_DONE;
    }
}

static int dispatch_mix(const Th06PspMixJob *job, short *output)
{
    volatile MeAudioMailbox *box = gMeMailboxUncached;
    box->frames = job->frames;
    box->inputCount = job->inputCount;
    box->mixDivisor = job->mixDivisor;
    box->outputPhys = (unsigned int)gMeOutput & 0x1fffffffu;
    unsigned int inputCount = job->inputCount;
    if (inputCount > TH06_PSP_ME_MAX_MIX_INPUTS)
        inputCount = TH06_PSP_ME_MAX_MIX_INPUTS;
    for (unsigned int index = 0; index < inputCount; ++index)
    {
        box->inputs[index].sourcePhys = (unsigned int)job->inputs[index].samples & 0x1fffffffu;
        box->inputs[index].frames = job->inputs[index].frames;
        box->inputs[index].destinationFrame = job->inputs[index].destinationFrame;
        box->inputs[index].channels = job->inputs[index].channels;
        box->inputs[index].gain = job->inputs[index].gain;
    }
    box->status = ME_AUDIO_STAT_IDLE;
    __asm__ volatile("sync");
    box->command = ME_AUDIO_CMD_MIX;

    const unsigned int startUs = sceKernelGetSystemTimeLow();
    while (box->status != ME_AUDIO_STAT_DONE)
    {
        if (sceKernelGetSystemTimeLow() - startUs >= ME_AUDIO_WAIT_US)
        {
            __atomic_store_n(&gMeAudioActive, 0, __ATOMIC_RELEASE);
            __atomic_fetch_add(&gMeTimeouts, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&gMeFallbacks, 1, __ATOMIC_RELAXED);
            th06_psp_boot_note("ME AUDIO TIMEOUT -> SC");
            mix_on_sc(job, output);
            return 0;
        }
        sceKernelDelayThread(50);
    }

    const unsigned int waitUs = sceKernelGetSystemTimeLow() - startUs;
    record_max(&gMeMaxWaitUs, waitUs);
    sceKernelDcacheInvalidateRange(gMeOutput, job->frames * 2 * sizeof(short));
    memcpy(output, gMeOutput, job->frames * 2 * sizeof(short));
    __atomic_fetch_add(&gMeJobs, 1, __ATOMIC_RELAXED);
    return 1;
}

int th06_psp_me_audio_mix(const Th06PspMixJob *job, short *output)
{
    if (!job || !output || job->frames == 0 || job->frames > TH06_PSP_ME_MAX_MIX_FRAMES ||
        job->inputCount > TH06_PSP_ME_MAX_MIX_INPUTS)
        return 0;
    if (__atomic_load_n(&gMeAudioActive, __ATOMIC_ACQUIRE))
        return dispatch_mix(job, output);
    __atomic_fetch_add(&gMeFallbacks, 1, __ATOMIC_RELAXED);
    mix_on_sc(job, output);
    return 0;
}

int th06_psp_me_audio_init(void)
{
    static short testStereo[TH06_PSP_ME_MAX_MIX_FRAMES * 2] __attribute__((aligned(64)));
    static short testMono[TH06_PSP_ME_MAX_MIX_FRAMES] __attribute__((aligned(64)));
    short expected[TH06_PSP_ME_MAX_MIX_FRAMES * 2] __attribute__((aligned(64)));
    short actual[TH06_PSP_ME_MAX_MIX_FRAMES * 2] __attribute__((aligned(64)));
    Th06PspMixJob test;

    gMeMailboxUncached =
        (volatile MeAudioMailbox *)(0x40000000u | (unsigned int)&gMeMailbox);
    memset((void *)&gMeMailbox, 0, sizeof(gMeMailbox));
    memset(gMeOutput, 0, sizeof(gMeOutput));
    sceKernelDcacheWritebackInvalidateAll();

    if (running_under_ppsspp())
    {
        th06_psp_boot_note("ME AUDIO OFF (PPSSPP -> SC)");
        return 0;
    }

    if (me_disabled_marker_present())
    {
        th06_psp_boot_note("ME AUDIO OFF (MARKER -> SC)");
        return 0;
    }

    th06_psp_boot_note("ME AUDIO INIT BEGIN (PSPPMD)");
    const int mapper = meLibDefaultInit();
    th06_psp_boot_notef("ME AUDIO INIT R%d", mapper);
    if (mapper < 0)
    {
        th06_psp_boot_note("ME AUDIO OFF (SC FALLBACK)");
        return 0;
    }

    sceKernelDelayThread(50000);
    for (unsigned int frame = 0; frame < TH06_PSP_ME_MAX_MIX_FRAMES; ++frame)
    {
        testStereo[frame * 2] = (short)((int)(frame % 127u) * 97 - 6000);
        testStereo[frame * 2 + 1] = (short)(5000 - (int)(frame % 113u) * 83);
        testMono[frame] = (short)((int)(frame % 61u) * 71 - 2000);
    }
    memset(&test, 0, sizeof(test));
    test.frames = TH06_PSP_ME_MAX_MIX_FRAMES;
    test.inputCount = 2;
    test.mixDivisor = 8;
    test.inputs[0].samples = testStereo;
    test.inputs[0].frames = TH06_PSP_ME_MAX_MIX_FRAMES;
    test.inputs[0].channels = 2;
    test.inputs[0].gain = 1.0f;
    test.inputs[1].samples = testMono;
    test.inputs[1].frames = 700;
    test.inputs[1].destinationFrame = 200;
    test.inputs[1].channels = 1;
    test.inputs[1].gain = 0.75f;
    sceKernelDcacheWritebackRange((void *)testStereo, sizeof(testStereo));
    sceKernelDcacheWritebackRange((void *)testMono, sizeof(testMono));
    mix_on_sc(&test, expected);
    __atomic_store_n(&gMeAudioActive, 1, __ATOMIC_RELEASE);
    if (!dispatch_mix(&test, actual) || memcmp(expected, actual, sizeof(expected)) != 0)
    {
        __atomic_store_n(&gMeAudioActive, 0, __ATOMIC_RELEASE);
        th06_psp_boot_note("ME AUDIO SELFTEST NG -> SC");
        return 0;
    }

    // Do not include the boot proof in runtime diagnostics.
    gMeJobs = gMeFallbacks = gMeTimeouts = gMeMaxWaitUs = 0;
    th06_psp_boot_notef("ME AUDIO ON MAP%d", mapper);
    return 1;
}

void th06_psp_me_audio_shutdown(void)
{
    if (!__atomic_load_n(&gMeAudioActive, __ATOMIC_ACQUIRE))
        return;
    gMeMailboxUncached->status = ME_AUDIO_STAT_IDLE;
    __asm__ volatile("sync");
    gMeMailboxUncached->command = ME_AUDIO_CMD_STOP;
    const unsigned int startUs = sceKernelGetSystemTimeLow();
    while (gMeMailboxUncached->status != ME_AUDIO_STAT_DONE &&
           sceKernelGetSystemTimeLow() - startUs < ME_AUDIO_WAIT_US)
        sceKernelDelayThread(50);
    __atomic_store_n(&gMeAudioActive, 0, __ATOMIC_RELEASE);
}

void th06_psp_me_audio_diag_window(unsigned int *jobs, unsigned int *fallbacks,
                                    unsigned int *timeouts, unsigned int *maxWaitUs)
{
    if (jobs)
        *jobs = __atomic_exchange_n(&gMeJobs, 0, __ATOMIC_ACQ_REL);
    if (fallbacks)
        *fallbacks = __atomic_exchange_n(&gMeFallbacks, 0, __ATOMIC_ACQ_REL);
    if (timeouts)
        *timeouts = __atomic_exchange_n(&gMeTimeouts, 0, __ATOMIC_ACQ_REL);
    if (maxWaitUs)
        *maxWaitUs = __atomic_exchange_n(&gMeMaxWaitUs, 0, __ATOMIC_ACQ_REL);
}
