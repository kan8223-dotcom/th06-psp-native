#pragma once

// The real-hardware vertex path is disabled. Keep the renderer and the loan
// provider on one compile-time contract if that path is ever re-enabled.
#ifndef TH06_PSP_ME_VERTEX_PACKING_ENABLED
#define TH06_PSP_ME_VERTEX_PACKING_ENABLED 0
#endif
#if TH06_PSP_ME_VERTEX_PACKING_ENABLED != 0 && TH06_PSP_ME_VERTEX_PACKING_ENABLED != 1
#error TH06_PSP_ME_VERTEX_PACKING_ENABLED must be 0 or 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    TH06_PSP_ME_MAX_MIX_INPUTS = 64,
    TH06_PSP_ME_MAX_MIX_FRAMES = 1024
};

typedef struct Th06PspMixInput
{
    const short *samples;
    unsigned int frames;
    unsigned int destinationFrame;
    unsigned int channels;
    float gain;
} Th06PspMixInput;

typedef struct Th06PspMixJob
{
    unsigned int frames;
    unsigned int inputCount;
    unsigned int mixDivisor;
    Th06PspMixInput inputs[TH06_PSP_ME_MAX_MIX_INPUTS];
} Th06PspMixJob;

// Returns 1 when ME produced this block, 0 when the identical SC fallback did.
// Either return value leaves `output` ready for the existing software ring.
int th06_psp_me_audio_mix(const Th06PspMixJob *job, short *output);
// Immediate final mix for already-synthesized MIDI plus unity-gain SE.
// Returns 1 with the same saturated PCM as ME, without claiming/sleeping on
// the worker. Returns 0 without touching output unless divisor/input gains
// are all exactly unity and the normal job bounds are valid. Reuses the
// existing SC work area; the audio producer remains its sole runtime owner.
int th06_psp_me_audio_mix_unity_nowait(const Th06PspMixJob *job, short *output);
int th06_psp_me_audio_init(void);
void th06_psp_me_audio_shutdown(void);
void th06_psp_me_audio_diag_window(unsigned int *jobs, unsigned int *fallbacks,
                                    unsigned int *timeouts, unsigned int *maxWaitUs);

// Pack one engine vertex stream into the native interleaved GE layout on ME.
// A successful output remains valid until th06_psp_me_vertex_frame_begin(),
// which the renderer calls only after the previous GE list has completed.
typedef struct Th06PspMeVertexPack
{
    const void *position;
    const void *texcoord;
    const void *diffuse;
    unsigned int positionStride;
    unsigned int texcoordStride;
    unsigned int diffuseStride;
    unsigned int count;
    unsigned int textured;
    unsigned int colored;
} Th06PspMeVertexPack;

// MIDI synthesis on the ME (psp/midi).  begin() claims the worker and returns
// the MIDI section of the uncached mailbox (NULL: use the SC); the caller
// fills the job and finish() runs it.  A timeout poisons the ME for the boot
// (audio mixing and MIDI both continue on the SC).
struct Th06MidiMeBox;
int th06_psp_me_available(void);
// Unacknowledged worker: do not release payloads or the application's code.
int th06_psp_me_poisoned(void);
volatile struct Th06MidiMeBox *th06_psp_me_midi_begin(void);
// Completion polls yield between checks so the game can run while the ME
// synthesizes. Already-DONE skips all sleeps; firstSleepUs is optional.
// The existing timeout/poison/ACK and payload-lifetime contract still applies.
int th06_psp_me_midi_finish(unsigned int timeoutUs, unsigned int firstSleepUs, unsigned int *waitUs);

void th06_psp_me_vertex_frame_begin(void);
int th06_psp_me_vertex_pack(const Th06PspMeVertexPack *job, const void **output);

// Borrow the unused 64-byte-aligned vertex arena for bounded song storage.
// Only after AudioInit completes, with vertex packing disabled and the ME
// idle/nonpoisoned. The whole arena is reserved even for a smaller request.
// The caller must exclude MIDI pin/failure and return only after every song
// span and ME read-set has been retired. A failed return retains the loan;
// its pointer must never be passed to libc free or reused by initialization.
void *th06_psp_me_borrow_song_storage(unsigned int bytes);
int th06_psp_me_return_song_storage(void *storage);

#ifdef __cplusplus
}
#endif
