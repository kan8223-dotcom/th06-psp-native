#include "audio_me.h"
#include "midi/midi_me_protocol.h"
#include "midi/MidiExitPolicy.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Exact MECC baseline shipped by the public PSPPMD project.  TH06 uses its
// documented auto-load entry and cache helpers only; application code never
// invokes the embedded kernel bridge directly.
void meLibOnProcess(void);
#include "me-core.h"

extern void th06_psp_boot_note(const char *message);
extern void th06_psp_boot_notef(const char *fmt, ...);
extern const char *th06_psp_game_dir(void);

enum
{
    ME_CMD_NONE = 0,
    ME_CMD_AUDIO_MIX = 1,
    ME_CMD_VERTEX_PACK = 2,
    ME_CMD_MIDI = 3,
    ME_CMD_STOP = 0xff,
    ME_STAT_IDLE = 0,
    ME_STAT_DONE = 1,

    ME_OWNER_NONE = 0,
    ME_OWNER_AUDIO = 1,
    ME_OWNER_VERTEX = 2,

    ME_ARENA_FREE = 0,
    ME_ARENA_INIT = 1,
    ME_ARENA_MIDI = 2,

    ME_AUDIO_WAIT_US = 8000,
    ME_VERTEX_WAIT_US = 6000,
    ME_VERTEX_ARENA_BYTES = 256 * 1024,

    // PSPPMD commit 18fb0b1 uses 2 KiB from local ME eDRAM at 0x400.
    // TH06 processes a 1024-frame block as four 256-frame chunks so the
    // stereo 32-bit accumulator remains inside that proven range.
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
// The MIDI state/blob layout (psp/midi/midi_me_protocol.h) is placed around
// this accumulator; both tables must describe the same 2 KiB.
_Static_assert(ME_AUDIO_EDRAM_ACCUM_BASE == TH06_MIDI_EDRAM_ACCUM_BASE &&
                   ME_AUDIO_EDRAM_ACCUM_END == TH06_MIDI_EDRAM_ACCUM_END,
               "MIDI eDRAM layout and the mix accumulator disagree");
_Static_assert(ME_AUDIO_EDRAM_FAT_STACK_TOP == TH06_MIDI_EDRAM_FAT_TOP,
               "MIDI eDRAM layout assumes the PSP-1000 stack top");

typedef struct MeVertexPosition
{
    uint32_t x, y, z;
} MeVertexPosition;

typedef struct MeVertexTexPosition
{
    uint32_t u, v, x, y, z;
} MeVertexTexPosition;

typedef struct MeVertexColorPosition
{
    uint32_t color, x, y, z;
} MeVertexColorPosition;

typedef struct MeVertexTexColorPosition
{
    uint32_t u, v, color, x, y, z;
} MeVertexTexColorPosition;

_Static_assert(sizeof(MeVertexPosition) == 12, "ME GE position layout changed");
_Static_assert(sizeof(MeVertexTexPosition) == 20, "ME GE texture layout changed");
_Static_assert(sizeof(MeVertexColorPosition) == 16, "ME GE color layout changed");
_Static_assert(sizeof(MeVertexTexColorPosition) == 24, "ME GE texture/color layout changed");

typedef struct MeMixInput
{
    uint32_t sourcePhys;
    uint32_t frames;
    uint32_t destinationFrame;
    uint32_t channels;
    uint32_t gainQ16;
} MeMixInput;

typedef struct MeSharedMailbox
{
    volatile uint32_t command;
    volatile uint32_t status;
    volatile uint32_t completedJobs;
    uint32_t reserved0;

    uint32_t audioFrames;
    uint32_t audioInputCount;
    uint32_t audioMixDivisor;
    uint32_t audioOutputPhys;
    MeMixInput audioInputs[TH06_PSP_ME_MAX_MIX_INPUTS];

    uint32_t positionPhys;
    uint32_t texcoordPhys;
    uint32_t diffusePhys;
    uint32_t vertexOutputPhys;
    uint32_t positionStride;
    uint32_t texcoordStride;
    uint32_t diffuseStride;
    uint32_t vertexCount;
    uint32_t textured;
    uint32_t colored;
    uint32_t vertexOutputBytes;
    uint32_t reserved1;

    // MIDI synthesis jobs (psp/midi).  Written by the SC only between
    // th06_psp_me_midi_begin() and th06_psp_me_midi_finish().
    Th06MidiMeBox midi;
} MeSharedMailbox;

static volatile MeSharedMailbox gMeMailbox __attribute__((aligned(64), section(".uncached")));
static volatile MeSharedMailbox *gMeMailboxUncached;
static short gMeAudioOutput[TH06_PSP_ME_MAX_MIX_FRAMES * 2] __attribute__((aligned(64)));
static int gScWide[TH06_PSP_ME_MAX_MIX_FRAMES * 2] __attribute__((aligned(64)));
static unsigned char gMeVertexArena[ME_VERTEX_ARENA_BYTES] __attribute__((aligned(64)));

static volatile int gMeActive;
static volatile int gMeStarted;
static volatile int gMePoisoned;
static volatile int gMeInitializing;
static volatile int gMeShuttingDown;
static volatile int gMeOwner;
static volatile unsigned int gMeAudioWanted;
static volatile unsigned int gMeVertexArenaOffset;
static volatile int gMeArenaOwner;
static volatile int gMeArenaReady;

static volatile unsigned int gMeJobs;
static volatile unsigned int gMeFallbacks;
static volatile unsigned int gMeTimeouts;
static volatile unsigned int gMeMaxWaitUs;

static int running_under_ppsspp(void)
{
    // Test before meLibDefaultInit(): PPSSPP v1.20 can block inside the MECC
    // bridge instead of returning an error that the fallback can consume.
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

static uint32_t vertex_bytes(uint32_t textured, uint32_t colored)
{
    if (textured)
        return colored ? sizeof(MeVertexTexColorPosition) : sizeof(MeVertexTexPosition);
    return colored ? sizeof(MeVertexColorPosition) : sizeof(MeVertexPosition);
}

// ME has no usable FPU/VFPU contract.  Vertex floats remain opaque IEEE-754
// bits.  Byte assembly prevents GCC from emitting lwc1/swc1 and also accepts
// independently-strided engine attributes that are not naturally aligned.
static uint32_t load_u32_bits(const unsigned char *source)
{
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
           ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint32_t gain_to_q16(float gain)
{
    if (!(gain > 0.0f))
        return 0;
    if (gain >= 1.0f)
        return 65536u;
    return (uint32_t)(gain * 65536.0f + 0.5f);
}

static int apply_gain_q16(short sample, uint32_t gainQ16)
{
    if (gainQ16 == 65536u)
        return sample;
    int product = (int)sample * (int)gainQ16;
    // Arithmetic shift rounds a negative product down.  Correct it to C's
    // truncation toward zero so the fixed-point ME result matches SC mixing.
    if (product < 0)
        product += 65535;
    return product >> 16;
}

/* Mixing at unity: the 32-bit sum is clipped to 16 bits like DirectSound did on
 * the original.  Shared by the SC fallback and the ME job so both stay bit-identical. */
static inline short saturate16(int v)
{
    return (short)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
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
        output[sample] = saturate16(gScWide[sample] / divisor);
}

static void me_invalidate_stream(uint32_t physical, uint32_t stride,
                                 uint32_t count, uint32_t elementBytes)
{
    if (!physical || !stride || !count)
        return;
    const uint32_t start = (0x80000000u | physical) & ~63u;
    const uint32_t last = (0x80000000u | physical) + (count - 1u) * stride + elementBytes;
    const uint32_t end = (last + 63u) & ~63u;
    meLibDcacheInvalidateRange(start, end - start);
}

static void sc_writeback_stream(const void *base, uint32_t stride,
                                uint32_t count, uint32_t elementBytes)
{
    if (!base || !stride || !count)
        return;
    const uintptr_t start = (uintptr_t)base & ~(uintptr_t)63u;
    const uintptr_t last = (uintptr_t)base + (count - 1u) * stride + elementBytes;
    const uintptr_t end = (last + 63u) & ~(uintptr_t)63u;
    sceKernelDcacheWritebackRange((void *)start, end - start);
}

static void finish_me_job(volatile MeSharedMailbox *box)
{
    box->completedJobs++;
    __asm__ volatile("sync");
    box->command = ME_CMD_NONE;
    __asm__ volatile("sync");
    box->status = ME_STAT_DONE;
}

static void process_audio_on_me(volatile MeSharedMailbox *box, volatile int *wide)
{
    uint32_t frames = box->audioFrames;
    if (frames > TH06_PSP_ME_MAX_MIX_FRAMES)
        frames = TH06_PSP_ME_MAX_MIX_FRAMES;
    uint32_t inputCount = box->audioInputCount;
    if (inputCount > TH06_PSP_ME_MAX_MIX_INPUTS)
        inputCount = TH06_PSP_ME_MAX_MIX_INPUTS;
    short *output = (short *)(0x80000000u | box->audioOutputPhys);
    const int divisor = box->audioMixDivisor ? (int)box->audioMixDivisor : 1;

    for (uint32_t chunkStart = 0; chunkStart < frames;
         chunkStart += ME_AUDIO_EDRAM_ACCUM_FRAMES)
    {
        uint32_t chunkFrames = frames - chunkStart;
        if (chunkFrames > ME_AUDIO_EDRAM_ACCUM_FRAMES)
            chunkFrames = ME_AUDIO_EDRAM_ACCUM_FRAMES;
        const uint32_t chunkEnd = chunkStart + chunkFrames;
        const uint32_t chunkSamples = chunkFrames * 2;
        for (uint32_t sample = 0; sample < chunkSamples; ++sample)
            wide[sample] = 0;

        for (uint32_t inputIndex = 0; inputIndex < inputCount; ++inputIndex)
        {
            const MeMixInput *input = (const MeMixInput *)&box->audioInputs[inputIndex];
            if (!input->sourcePhys || input->destinationFrame >= frames ||
                (input->channels != 1 && input->channels != 2))
                continue;
            uint32_t inputFrames = input->frames;
            if (inputFrames > frames - input->destinationFrame)
                inputFrames = frames - input->destinationFrame;
            const uint32_t inputStart = input->destinationFrame;
            const uint32_t inputEnd = inputStart + inputFrames;
            const uint32_t overlapStart = inputStart > chunkStart ? inputStart : chunkStart;
            const uint32_t overlapEnd = inputEnd < chunkEnd ? inputEnd : chunkEnd;
            if (overlapStart >= overlapEnd)
                continue;

            const uint32_t sourceFrame = overlapStart - inputStart;
            const uint32_t mixFrames = overlapEnd - overlapStart;
            const short *source = (const short *)(0x80000000u | input->sourcePhys) +
                                  sourceFrame * input->channels;
            const uint32_t sourceStart = (uint32_t)source & ~63u;
            const uint32_t sourceEnd =
                ((uint32_t)source + mixFrames * input->channels * sizeof(short) + 63u) & ~63u;
            meLibDcacheInvalidateRange(sourceStart, sourceEnd - sourceStart);
            volatile int *destination = wide + (overlapStart - chunkStart) * 2;
            if (input->channels == 1)
            {
                for (uint32_t frame = 0; frame < mixFrames; ++frame)
                {
                    const int value = apply_gain_q16(source[frame], input->gainQ16);
                    destination[frame * 2] += value;
                    destination[frame * 2 + 1] += value;
                }
            }
            else
            {
                for (uint32_t frame = 0; frame < mixFrames; ++frame)
                {
                    destination[frame * 2] += apply_gain_q16(source[frame * 2], input->gainQ16);
                    destination[frame * 2 + 1] += apply_gain_q16(source[frame * 2 + 1], input->gainQ16);
                }
            }
        }

        short *chunkOutput = output + chunkStart * 2;
        for (uint32_t sample = 0; sample < chunkSamples; ++sample)
            chunkOutput[sample] = saturate16(wide[sample] / divisor);
    }
    meLibDcacheWritebackRange((uint32_t)output, frames * 2 * sizeof(short));
}

static void process_vertices_on_me(volatile MeSharedMailbox *box)
{
    const uint32_t count = box->vertexCount;
    const int textured = box->textured != 0;
    const int colored = box->colored != 0;
    const unsigned char *positions =
        (const unsigned char *)(0x80000000u | box->positionPhys);
    const unsigned char *texcoords =
        textured ? (const unsigned char *)(0x80000000u | box->texcoordPhys) : 0;
    const unsigned char *diffuse =
        colored ? (const unsigned char *)(0x80000000u | box->diffusePhys) : 0;
    void *output = (void *)(0x80000000u | box->vertexOutputPhys);

    me_invalidate_stream(box->positionPhys, box->positionStride, count, 12);
    if (textured)
        me_invalidate_stream(box->texcoordPhys, box->texcoordStride, count, 8);
    if (colored)
        me_invalidate_stream(box->diffusePhys, box->diffuseStride, count, 4);

    if (textured && colored)
    {
        MeVertexTexColorPosition *out = (MeVertexTexColorPosition *)output;
        for (uint32_t i = 0; i < count; ++i)
        {
            const unsigned char *position = positions + i * box->positionStride;
            const unsigned char *uv = texcoords + i * box->texcoordStride;
            const unsigned char *color = diffuse + i * box->diffuseStride;
            out[i].u = load_u32_bits(uv);
            out[i].v = load_u32_bits(uv + 4);
            out[i].color = load_u32_bits(color);
            out[i].x = load_u32_bits(position);
            out[i].y = load_u32_bits(position + 4);
            out[i].z = load_u32_bits(position + 8);
        }
    }
    else if (textured)
    {
        MeVertexTexPosition *out = (MeVertexTexPosition *)output;
        for (uint32_t i = 0; i < count; ++i)
        {
            const unsigned char *position = positions + i * box->positionStride;
            const unsigned char *uv = texcoords + i * box->texcoordStride;
            out[i].u = load_u32_bits(uv);
            out[i].v = load_u32_bits(uv + 4);
            out[i].x = load_u32_bits(position);
            out[i].y = load_u32_bits(position + 4);
            out[i].z = load_u32_bits(position + 8);
        }
    }
    else if (colored)
    {
        MeVertexColorPosition *out = (MeVertexColorPosition *)output;
        for (uint32_t i = 0; i < count; ++i)
        {
            const unsigned char *position = positions + i * box->positionStride;
            const unsigned char *color = diffuse + i * box->diffuseStride;
            out[i].color = load_u32_bits(color);
            out[i].x = load_u32_bits(position);
            out[i].y = load_u32_bits(position + 4);
            out[i].z = load_u32_bits(position + 8);
        }
    }
    else
    {
        MeVertexPosition *out = (MeVertexPosition *)output;
        for (uint32_t i = 0; i < count; ++i)
        {
            const unsigned char *position = positions + i * box->positionStride;
            out[i].x = load_u32_bits(position);
            out[i].y = load_u32_bits(position + 4);
            out[i].z = load_u32_bits(position + 8);
        }
    }

    meLibDcacheWritebackRange((uint32_t)output, box->vertexOutputBytes);
}

// Runs only on ME.  One mailbox serializes audio and vertex jobs; SC audio
// announces intent before claiming it, while rendering uses a non-blocking
// claim and falls back immediately whenever audio needs the worker.
void meLibOnProcess(void)
{
    volatile MeSharedMailbox *box =
        (volatile MeSharedMailbox *)(0x40000000u | (uint32_t)&gMeMailbox);
    uint32_t wideAddress = ME_AUDIO_EDRAM_ACCUM_BASE;
    __asm__ volatile("" : "+r"(wideAddress));
    volatile int *wide = (volatile int *)wideAddress;

    meLibDcacheWritebackInvalidateAll();
    meLibIcacheInvalidateAll();

    for (;;)
    {
        while (box->command == ME_CMD_NONE)
            __asm__ volatile("nop; nop; nop; nop;");

        const uint32_t command = box->command;
        if (command == ME_CMD_STOP)
        {
            box->status = ME_STAT_DONE;
            meLibHalt();
            return;
        }
        if (command == ME_CMD_AUDIO_MIX)
            process_audio_on_me(box, wide);
        else if (command == ME_CMD_VERTEX_PACK)
            process_vertices_on_me(box);
        else if (command == ME_CMD_MIDI)
            th06_midi_me_execute(&box->midi);
        else
        {
            box->command = ME_CMD_NONE;
            continue;
        }
        finish_me_job(box);
    }
}

static void release_me(void)
{
    __atomic_store_n(&gMeOwner, ME_OWNER_NONE, __ATOMIC_RELEASE);
}

static int claim_me_for_audio(uint32_t startUs)
{
    __atomic_fetch_add(&gMeAudioWanted, 1u, __ATOMIC_ACQ_REL);
    for (;;)
    {
        int expected = ME_OWNER_NONE;
        if (__atomic_compare_exchange_n(&gMeOwner, &expected, ME_OWNER_AUDIO, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            __atomic_fetch_sub(&gMeAudioWanted, 1u, __ATOMIC_ACQ_REL);
            return 1;
        }
        if (sceKernelGetSystemTimeLow() - startUs >= ME_AUDIO_WAIT_US)
        {
            __atomic_fetch_sub(&gMeAudioWanted, 1u, __ATOMIC_ACQ_REL);
            return 0;
        }
        sceKernelDelayThread(20);
    }
}

static int claim_me_for_vertex(void)
{
    if (__atomic_load_n(&gMeAudioWanted, __ATOMIC_ACQUIRE) != 0)
        return 0;
    int expected = ME_OWNER_NONE;
    if (!__atomic_compare_exchange_n(&gMeOwner, &expected, ME_OWNER_VERTEX, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    if (__atomic_load_n(&gMeAudioWanted, __ATOMIC_ACQUIRE) != 0)
    {
        release_me();
        return 0;
    }
    return 1;
}

static void poison_me(void)
{
    __atomic_store_n(&gMeActive, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gMePoisoned, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&gMeTimeouts, 1u, __ATOMIC_RELAXED);
}

static int dispatch_audio(const Th06PspMixJob *job, short *output)
{
    const uint32_t startUs = sceKernelGetSystemTimeLow();
    if (!claim_me_for_audio(startUs))
    {
        __atomic_fetch_add(&gMeFallbacks, 1u, __ATOMIC_RELAXED);
        mix_on_sc(job, output);
        return 0;
    }

    volatile MeSharedMailbox *box = gMeMailboxUncached;
    if (!box || box->command != ME_CMD_NONE ||
        !__atomic_load_n(&gMeActive, __ATOMIC_ACQUIRE))
    {
        release_me();
        __atomic_fetch_add(&gMeFallbacks, 1u, __ATOMIC_RELAXED);
        mix_on_sc(job, output);
        return 0;
    }

    uint32_t inputCount = job->inputCount;
    if (inputCount > TH06_PSP_ME_MAX_MIX_INPUTS)
        inputCount = TH06_PSP_ME_MAX_MIX_INPUTS;
    for (uint32_t index = 0; index < inputCount; ++index)
    {
        const Th06PspMixInput *input = &job->inputs[index];
        const uint32_t sampleBytes = input->channels * sizeof(short);
        sc_writeback_stream(input->samples, sampleBytes, input->frames, sampleBytes);
        box->audioInputs[index].sourcePhys = (uint32_t)input->samples & 0x1fffffffu;
        box->audioInputs[index].frames = input->frames;
        box->audioInputs[index].destinationFrame = input->destinationFrame;
        box->audioInputs[index].channels = input->channels;
        box->audioInputs[index].gainQ16 = gain_to_q16(input->gain);
    }
    box->audioFrames = job->frames;
    box->audioInputCount = inputCount;
    box->audioMixDivisor = job->mixDivisor;
    box->audioOutputPhys = (uint32_t)gMeAudioOutput & 0x1fffffffu;
    box->status = ME_STAT_IDLE;
    __asm__ volatile("sync");
    box->command = ME_CMD_AUDIO_MIX;

    while (box->status != ME_STAT_DONE)
    {
        if (sceKernelGetSystemTimeLow() - startUs >= ME_AUDIO_WAIT_US)
        {
            poison_me();
            release_me();
            __atomic_fetch_add(&gMeFallbacks, 1u, __ATOMIC_RELAXED);
            mix_on_sc(job, output);
            return 0;
        }
        sceKernelDelayThread(20);
    }

    const uint32_t waitUs = sceKernelGetSystemTimeLow() - startUs;
    record_max(&gMeMaxWaitUs, waitUs);
    sceKernelDcacheInvalidateRange(gMeAudioOutput, job->frames * 2 * sizeof(short));
    memcpy(output, gMeAudioOutput, job->frames * 2 * sizeof(short));
    __atomic_fetch_add(&gMeJobs, 1u, __ATOMIC_RELAXED);
    release_me();
    return 1;
}

void th06_psp_me_vertex_frame_begin(void)
{
    if (!__atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&gMeArenaOwner, __ATOMIC_SEQ_CST) != ME_ARENA_MIDI)
        __atomic_store_n(&gMeVertexArenaOffset, 0u, __ATOMIC_RELEASE);
}

int th06_psp_me_vertex_pack(const Th06PspMeVertexPack *job, const void **output)
{
    if (output)
        *output = 0;
    if (__atomic_load_n(&gMeArenaOwner, __ATOMIC_SEQ_CST) == ME_ARENA_MIDI)
        return 0;
    if (!job || !output || !job->position || !job->positionStride || !job->count ||
        (job->textured && (!job->texcoord || !job->texcoordStride)) ||
        (job->colored && (!job->diffuse || !job->diffuseStride)) ||
        !__atomic_load_n(&gMeActive, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE))
        return 0;

    if (!claim_me_for_vertex())
    {
        __atomic_fetch_add(&gMeFallbacks, 1u, __ATOMIC_RELAXED);
        return 0;
    }

    volatile MeSharedMailbox *box = gMeMailboxUncached;
    if (__atomic_load_n(&gMeArenaOwner, __ATOMIC_SEQ_CST) == ME_ARENA_MIDI ||
        !box || box->command != ME_CMD_NONE ||
        !__atomic_load_n(&gMeActive, __ATOMIC_ACQUIRE))
    {
        release_me();
        __atomic_fetch_add(&gMeFallbacks, 1u, __ATOMIC_RELAXED);
        return 0;
    }

    const uint32_t bytesPerVertex = vertex_bytes(job->textured, job->colored);
    if (job->count > ME_VERTEX_ARENA_BYTES / bytesPerVertex)
    {
        release_me();
        __atomic_fetch_add(&gMeFallbacks, 1u, __ATOMIC_RELAXED);
        return 0;
    }
    const uint32_t outputBytes = job->count * bytesPerVertex;
    const uint32_t offset = (__atomic_load_n(&gMeVertexArenaOffset, __ATOMIC_ACQUIRE) + 63u) & ~63u;
    if (offset > ME_VERTEX_ARENA_BYTES || outputBytes > ME_VERTEX_ARENA_BYTES - offset)
    {
        release_me();
        __atomic_fetch_add(&gMeFallbacks, 1u, __ATOMIC_RELAXED);
        return 0;
    }
    void *const destination = gMeVertexArena + offset;

    sc_writeback_stream(job->position, job->positionStride, job->count, 12);
    if (job->textured)
        sc_writeback_stream(job->texcoord, job->texcoordStride, job->count, 8);
    if (job->colored)
        sc_writeback_stream(job->diffuse, job->diffuseStride, job->count, 4);

    box->positionPhys = (uint32_t)job->position & 0x1fffffffu;
    box->texcoordPhys = (uint32_t)job->texcoord & 0x1fffffffu;
    box->diffusePhys = (uint32_t)job->diffuse & 0x1fffffffu;
    box->vertexOutputPhys = (uint32_t)destination & 0x1fffffffu;
    box->positionStride = job->positionStride;
    box->texcoordStride = job->texcoordStride;
    box->diffuseStride = job->diffuseStride;
    box->vertexCount = job->count;
    box->textured = job->textured != 0;
    box->colored = job->colored != 0;
    box->vertexOutputBytes = outputBytes;
    box->status = ME_STAT_IDLE;
    __asm__ volatile("sync");
    box->command = ME_CMD_VERTEX_PACK;

    const uint32_t startUs = sceKernelGetSystemTimeLow();
    while (box->status != ME_STAT_DONE)
    {
        if (sceKernelGetSystemTimeLow() - startUs >= ME_VERTEX_WAIT_US)
        {
            // A late worker can only touch its dedicated arena.  The fallback
            // uses libGU list memory, so it cannot be corrupted by that write.
            poison_me();
            release_me();
            __atomic_fetch_add(&gMeFallbacks, 1u, __ATOMIC_RELAXED);
            return 0;
        }
        __asm__ volatile("nop; nop; nop; nop;");
    }

    const uint32_t waitUs = sceKernelGetSystemTimeLow() - startUs;
    record_max(&gMeMaxWaitUs, waitUs);
    __atomic_store_n(&gMeVertexArenaOffset, offset + outputBytes, __ATOMIC_RELEASE);
    *output = destination;
    __atomic_fetch_add(&gMeJobs, 1u, __ATOMIC_RELAXED);
    release_me();
    return 1;
}

int th06_psp_me_audio_mix_unity_nowait(const Th06PspMixJob *job, short *output)
{
    if (!job || !output || job->frames == 0 || job->frames > TH06_PSP_ME_MAX_MIX_FRAMES ||
        job->inputCount > TH06_PSP_ME_MAX_MIX_INPUTS || job->mixDivisor != 1u)
        return 0;
    // Validate before writing: fractional/NaN gains retain the original ME
    // path, whose Q16 gain can differ from the SC float fallback by one LSB.
    for (unsigned int index = 0; index < job->inputCount; ++index)
        if (job->inputs[index].gain != 1.0f)
            return 0;
    if (job->inputCount == 1u)
    {
        const Th06PspMixInput *input = &job->inputs[0];
        if (input->samples && input->channels == 2u && input->destinationFrame == 0u &&
            input->frames >= job->frames)
        {
            memmove(output, input->samples, job->frames * 2u * sizeof(short));
            return 1;
        }
    }
    // Unity multiplication is exact for every s16 value. At most 64 inputs
    // sum within signed 32 bits, then use the identical divisor-1 saturation.
    // This is intentional SC final addition, not MIDI synthesis fallback:
    // it neither changes ME ownership/read-set nor increments failure stats.
    mix_on_sc(job, output);
    return 1;
}

int th06_psp_me_audio_mix(const Th06PspMixJob *job, short *output)
{
    if (!job || !output || job->frames == 0 || job->frames > TH06_PSP_ME_MAX_MIX_FRAMES ||
        job->inputCount > TH06_PSP_ME_MAX_MIX_INPUTS)
        return 0;
    if (__atomic_load_n(&gMeActive, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE))
        return dispatch_audio(job, output);
    __atomic_fetch_add(&gMeFallbacks, 1u, __ATOMIC_RELAXED);
    mix_on_sc(job, output);
    return 0;
}

// ---------------------------------------------------------------------------
// MIDI synthesis jobs (psp/midi/MidiPsp.cpp).  The SC fills the MIDI section
// of the mailbox only after it owns the worker, exactly like an audio mix job.
// ---------------------------------------------------------------------------
int th06_psp_me_available(void)
{
    return __atomic_load_n(&gMeActive, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE);
}

volatile Th06MidiMeBox *th06_psp_me_midi_begin(void)
{
    if (!th06_psp_me_available())
        return 0;
    const uint32_t startUs = sceKernelGetSystemTimeLow();
    if (!claim_me_for_audio(startUs))
        return 0;
    volatile MeSharedMailbox *box = gMeMailboxUncached;
    if (!box || box->command != ME_CMD_NONE || !th06_psp_me_available())
    {
        release_me();
        return 0;
    }
    return &box->midi;
}

int th06_psp_me_midi_finish(unsigned int timeoutUs, unsigned int firstSleepUs, unsigned int *waitUs)
{
    volatile MeSharedMailbox *box = gMeMailboxUncached;
    const uint32_t startUs = sceKernelGetSystemTimeLow();
    if (waitUs)
        *waitUs = 0;
    if (!box)
    {
        release_me();
        return 0;
    }
    box->status = ME_STAT_IDLE;
    __asm__ volatile("sync");
    box->command = ME_CMD_MIDI;

    // The producer outranks the game thread. It must yield while the ME is
    // synthesizing, otherwise a busy completion poll consumes SC time too
    // and prevents the game from running in parallel with that synthesis.
    // Keep the original deadline/ACK/payload lifetime rules. A delayed wake
    // can still starve PCM during image loading; do not hide that separate
    // limitation by monopolizing the SC for every normal render job.
    if (firstSleepUs > 100u && box->status != ME_STAT_DONE)
        sceKernelDelayThread(firstSleepUs);
    while (box->status != ME_STAT_DONE)
    {
        if (sceKernelGetSystemTimeLow() - startUs >= timeoutUs)
        {
            // A late ME may still write its eDRAM, the job's output range and
            // the mailbox; the SC never reads those again after this point.
            poison_me();
            release_me();
            return 0;
        }
        sceKernelDelayThread(50);
    }
    const uint32_t elapsed = sceKernelGetSystemTimeLow() - startUs;
    if (waitUs)
        *waitUs = elapsed;
    record_max(&gMeMaxWaitUs, elapsed);
    __atomic_fetch_add(&gMeJobs, 1u, __ATOMIC_RELAXED);
    release_me();
    return 1;
}

static int selftest_audio(void)
{
    static short testStereo[TH06_PSP_ME_MAX_MIX_FRAMES * 2] __attribute__((aligned(64)));
    static short testMono[TH06_PSP_ME_MAX_MIX_FRAMES] __attribute__((aligned(64)));
    short expected[TH06_PSP_ME_MAX_MIX_FRAMES * 2] __attribute__((aligned(64)));
    short actual[TH06_PSP_ME_MAX_MIX_FRAMES * 2] __attribute__((aligned(64)));
    Th06PspMixJob test;

    for (uint32_t frame = 0; frame < TH06_PSP_ME_MAX_MIX_FRAMES; ++frame)
    {
        testStereo[frame * 2] = (short)((int)(frame % 127u) * 97 - 6000);
        testStereo[frame * 2 + 1] = (short)(5000 - (int)(frame % 113u) * 83);
        testMono[frame] = (short)((int)(frame % 61u) * 71 - 2000);
    }
    memset(&test, 0, sizeof(test));
    test.frames = TH06_PSP_ME_MAX_MIX_FRAMES;
    test.inputCount = 2;
    test.mixDivisor = 1;
    test.inputs[0].samples = testStereo;
    test.inputs[0].frames = TH06_PSP_ME_MAX_MIX_FRAMES;
    test.inputs[0].channels = 2;
    test.inputs[0].gain = 1.0f;
    test.inputs[1].samples = testMono;
    test.inputs[1].frames = 700;
    test.inputs[1].destinationFrame = 200;
    test.inputs[1].channels = 1;
    test.inputs[1].gain = 0.75f;
    mix_on_sc(&test, expected);
    if (!dispatch_audio(&test, actual))
        return 0;
    return memcmp(expected, actual, sizeof(expected)) == 0;
}

static int selftest_vertices(void)
{
    typedef struct TestSource
    {
        float x, y, z, w;
        float u, v;
        uint32_t color;
    } TestSource;
    static TestSource source[4] __attribute__((aligned(64)));
    MeVertexTexColorPosition expected[4] __attribute__((aligned(64)));
    Th06PspMeVertexPack test;
    const void *actual = 0;

    memset(&test, 0, sizeof(test));
    for (uint32_t i = 0; i < 4; ++i)
    {
        source[i].x = (float)i + 0.25f;
        source[i].y = (float)i * -2.0f;
        source[i].z = 0.5f;
        source[i].w = 1.0f;
        source[i].u = (float)i / 4.0f;
        source[i].v = 1.0f - source[i].u;
        source[i].color = 0x80402010u + i;
        expected[i].u = float_bits(source[i].u);
        expected[i].v = float_bits(source[i].v);
        expected[i].color = source[i].color;
        expected[i].x = float_bits(source[i].x);
        expected[i].y = float_bits(source[i].y);
        expected[i].z = float_bits(source[i].z);
    }
    test.position = &source[0].x;
    test.texcoord = &source[0].u;
    test.diffuse = &source[0].color;
    test.positionStride = sizeof(source[0]);
    test.texcoordStride = sizeof(source[0]);
    test.diffuseStride = sizeof(source[0]);
    test.count = 4;
    test.textured = 1;
    test.colored = 1;

    if (!th06_psp_me_vertex_pack(&test, &actual))
        return 0;
    sceKernelDcacheInvalidateRange((void *)actual, sizeof(expected));
    const int matched = memcmp(actual, expected, sizeof(expected)) == 0;
    // Drop the cached aliases loaded by memcmp.  Runtime output is thereafter
    // written by ME and consumed only by GE.
    sceKernelDcacheInvalidateRange(gMeVertexArena, sizeof(gMeVertexArena));
    return matched;
}

static int actual_me_audio_init(void)
{
    // A late worker may still use the old mailbox/output after a timeout.
    if (__atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&gMeShuttingDown, __ATOMIC_SEQ_CST))
        return 0;
    gMeMailboxUncached =
        (volatile MeSharedMailbox *)(0x40000000u | (uint32_t)&gMeMailbox);
    memset((void *)&gMeMailbox, 0, sizeof(gMeMailbox));
    memset(gMeAudioOutput, 0, sizeof(gMeAudioOutput));
    memset(gMeVertexArena, 0, sizeof(gMeVertexArena));
    __atomic_store_n(&gMeOwner, ME_OWNER_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&gMeAudioWanted, 0u, __ATOMIC_RELEASE);
    // Poison is permanent for the boot, including a shutdown/init race.
    __atomic_store_n(&gMeVertexArenaOffset, 0u, __ATOMIC_RELEASE);
    sceKernelDcacheWritebackInvalidateAll();

    if (running_under_ppsspp())
    {
        th06_psp_boot_note("ME AUDIO+VTX OFF (PPSSPP -> SC)");
        return 0;
    }
    if (me_disabled_marker_present())
    {
        th06_psp_boot_note("ME AUDIO+VTX OFF (MARKER -> SC)");
        return 0;
    }

    th06_psp_boot_note("ME AUDIO+VTX INIT BEGIN (PSPPMD)");
    __atomic_store_n(&gMeInitializing, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&gMeShuttingDown, __ATOMIC_SEQ_CST))
    {
        __atomic_store_n(&gMeInitializing, 0, __ATOMIC_RELEASE);
        return 0;
    }
    const int mapper = meLibDefaultInit();
    th06_psp_boot_notef("ME AUDIO+VTX INIT R%d", mapper);
    if (mapper < 0)
    {
        __atomic_store_n(&gMeInitializing, 0, __ATOMIC_RELEASE);
        th06_psp_boot_note("ME AUDIO+VTX OFF (SC FALLBACK)");
        return 0;
    }
    __atomic_store_n(&gMeStarted, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&gMeInitializing, 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE))
        return 0;
    __atomic_store_n(&gMeActive, 1, __ATOMIC_RELEASE);
    sceKernelDelayThread(50000);

    if (!selftest_audio())
    {
        __atomic_store_n(&gMeActive, 0, __ATOMIC_RELEASE);
        th06_psp_boot_note("ME AUDIO SELFTEST NG -> ALL SC");
        return 0;
    }
    __atomic_store_n(&gMeVertexArenaOffset, 0u, __ATOMIC_RELEASE);
    if (!selftest_vertices())
    {
        __atomic_store_n(&gMeActive, 0, __ATOMIC_RELEASE);
        th06_psp_boot_note("ME VERTEX SELFTEST NG -> ALL SC");
        return 0;
    }

    __atomic_store_n(&gMeVertexArenaOffset, 0u, __ATOMIC_RELEASE);
    gMeJobs = gMeFallbacks = gMeTimeouts = gMeMaxWaitUs = 0;
    th06_psp_boot_notef("ME AUDIO ON / VTX OFF MAP%d", mapper);
    return 1;
}

static int song_storage_backend_idle(void)
{
    return __atomic_load_n(&gMeArenaReady, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&gMeInitializing, __ATOMIC_SEQ_CST) &&
           !__atomic_load_n(&gMeShuttingDown, __ATOMIC_SEQ_CST) &&
           !__atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&gMeOwner, __ATOMIC_SEQ_CST) == ME_OWNER_NONE &&
           __atomic_load_n(&gMeAudioWanted, __ATOMIC_ACQUIRE) == 0u &&
           __atomic_load_n(&gMeVertexArenaOffset, __ATOMIC_ACQUIRE) == 0u &&
           gMeMailboxUncached && gMeMailboxUncached->command == ME_CMD_NONE;
}

int th06_psp_me_audio_init(void)
{
    int expected = ME_ARENA_FREE;
    // Acquire the arena before the old body touches any mailbox or buffer.
    // INIT covers its complete selftests even after gMeInitializing clears.
    if (__atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&gMeShuttingDown, __ATOMIC_SEQ_CST) ||
        __atomic_load_n(&gMeStarted, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&gMeOwner, __ATOMIC_ACQUIRE) != ME_OWNER_NONE ||
        !__atomic_compare_exchange_n(&gMeArenaOwner, &expected, ME_ARENA_INIT, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    __atomic_store_n(&gMeArenaReady, 0, __ATOMIC_RELEASE);
    // Complete the same seq_cst init/shutdown handshake as the mapper. If
    // shutdown observed FREE before this CAS, it must prevent all writers.
    if (__atomic_load_n(&gMeShuttingDown, __ATOMIC_SEQ_CST) ||
        __atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&gMeArenaOwner, ME_ARENA_FREE, __ATOMIC_SEQ_CST);
        return 0;
    }
    const int result = actual_me_audio_init();
    // OFF on PPSSPP / marker is a completed initialization too. Timeouts,
    // shutdown or an unfinished vertex result never make the arena lendable.
    if (!__atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&gMeShuttingDown, __ATOMIC_SEQ_CST) &&
        __atomic_load_n(&gMeOwner, __ATOMIC_ACQUIRE) == ME_OWNER_NONE &&
        __atomic_load_n(&gMeVertexArenaOffset, __ATOMIC_ACQUIRE) == 0u &&
        gMeMailboxUncached && gMeMailboxUncached->command == ME_CMD_NONE)
        __atomic_store_n(&gMeArenaReady, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&gMeArenaOwner, ME_ARENA_FREE, __ATOMIC_SEQ_CST);
    return result;
}

void *th06_psp_me_borrow_song_storage(unsigned int bytes)
{
#if TH06_PSP_ME_VERTEX_PACKING_ENABLED == 0
    int expected = ME_ARENA_FREE;
    if (bytes == 0u || bytes > ME_VERTEX_ARENA_BYTES || sizeof(void *) != 4u ||
        ((uintptr_t)gMeVertexArena & 63u) != 0u || !song_storage_backend_idle() ||
        !__atomic_compare_exchange_n(&gMeArenaOwner, &expected, ME_ARENA_MIDI, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    // A writer can have claimed the ME after the first idle check. A failed
    // transaction publishes no pointer; its vertex claim rechecks MIDI too.
    if (!song_storage_backend_idle())
    {
        __atomic_store_n(&gMeArenaOwner, ME_ARENA_FREE, __ATOMIC_SEQ_CST);
        return 0;
    }
    return gMeVertexArena;
#else
    (void)bytes;
    return 0;
#endif
}

int th06_psp_me_return_song_storage(void *storage)
{
#if TH06_PSP_ME_VERTEX_PACKING_ENABLED == 0
    int expected = ME_ARENA_MIDI;
    if (storage != gMeVertexArena || !song_storage_backend_idle())
        return 0;
    return __atomic_compare_exchange_n(&gMeArenaOwner, &expected, ME_ARENA_FREE, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#else
    (void)storage;
    return 0;
#endif
}

void th06_psp_me_audio_shutdown(void)
{
    __atomic_store_n(&gMeShuttingDown, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&gMeActive, 0, __ATOMIC_RELEASE);
    // Ordered loads: do not evaluate Started before observing the completion
    // of initialization (function argument evaluation order is unspecified).
    const int initializing = __atomic_load_n(&gMeInitializing, __ATOMIC_SEQ_CST) ||
                             __atomic_load_n(&gMeArenaOwner, __ATOMIC_SEQ_CST) == ME_ARENA_INIT;
    const int started = __atomic_load_n(&gMeStarted, __ATOMIC_ACQUIRE);
    const int poisoned = __atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE);
    const enum Th06MeExitAction action = th06_me_exit_action(
        initializing, started, poisoned);
    if (action == TH06_ME_EXIT_HOLD)
    {
        poison_me();
        return;
    }
    if (action == TH06_ME_EXIT_NONE || !gMeMailboxUncached)
        return;

    // Acquire exclusive ownership before STOP. A caller may already have
    // claimed an idle mailbox and not yet published its job command.
    const uint32_t startUs = sceKernelGetSystemTimeLow();
    while (1)
    {
        int expected = ME_OWNER_NONE;
        if (__atomic_compare_exchange_n(&gMeOwner, &expected, ME_OWNER_AUDIO, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
        if (sceKernelGetSystemTimeLow() - startUs >= 2000000u)
        {
            poison_me();
            return;
        }
        sceKernelDelayThread(50);
    }
    const int ownerInitializing = __atomic_load_n(&gMeInitializing, __ATOMIC_SEQ_CST) ||
                                  __atomic_load_n(&gMeArenaOwner, __ATOMIC_SEQ_CST) == ME_ARENA_INIT;
    const int ownerStarted = __atomic_load_n(&gMeStarted, __ATOMIC_ACQUIRE);
    const int ownerPoisoned = __atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE);
    if (th06_me_exit_action(ownerInitializing, ownerStarted, ownerPoisoned) != TH06_ME_EXIT_STOP)
    {
        release_me();
        return;
    }
    if (gMeMailboxUncached->command == ME_CMD_NONE)
    {
        gMeMailboxUncached->status = ME_STAT_IDLE;
        __asm__ volatile("sync");
        gMeMailboxUncached->command = ME_CMD_STOP;
        while (gMeMailboxUncached->status != ME_STAT_DONE &&
               sceKernelGetSystemTimeLow() - startUs < 2000000u)
            sceKernelDelayThread(50);
        if (gMeMailboxUncached->status != ME_STAT_DONE)
            poison_me();
    }
    else
        poison_me();
    if (!__atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE))
        __atomic_store_n(&gMeStarted, 0, __ATOMIC_RELEASE);
    release_me();
}

int th06_psp_me_poisoned(void)
{
    return __atomic_load_n(&gMePoisoned, __ATOMIC_ACQUIRE);
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
