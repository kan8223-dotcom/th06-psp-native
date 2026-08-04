#pragma once

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
int th06_psp_me_audio_init(void);
void th06_psp_me_audio_shutdown(void);
void th06_psp_me_audio_diag_window(unsigned int *jobs, unsigned int *fallbacks,
                                    unsigned int *timeouts, unsigned int *maxWaitUs);

#ifdef __cplusplus
}
#endif
