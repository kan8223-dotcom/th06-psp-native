// TH06 PSP: MIDI BGM synthesized at run time from the user's own gm.dls
// (Microsoft GS Wavetable Synth instruments), like the PC version does with
// MSGS.  Nothing derived from gm.dls is shipped: on the first boot the game
// reads <th6>/gm.dls and the songs in 紅魔郷MD.DAT, keeps only the instruments
// the songs use (touhou_dls_common, TDSB v1 blob) and caches that blob in
// TH06PSP/MIDICACHE.BIN.  Without gm.dls the game behaves exactly as before
// (WAV BGM; selecting MIDI falls back to WAV).
//
// Threads:
//  - game thread: the MidiOutput hooks (psp/platform_hooks.cpp) queue commands;
//  - BGM producer thread: th06_midi_render_for_mix() forwards them to the
//    player and renders PCM for the next mix job (ME job or SC fallback);
//  - ME: psp/midi/MidiMe.cpp.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// psp/main.cpp, before the engine starts (logs to the boot overlay).
void th06_midi_boot_prepare(void);
// The cached blob is ready (MIDI can be selected).
int th06_midi_blob_ready(void);
// psp/audio.cpp, right after the ME audio worker initialised: start the SC
// player and, when possible, the ME player (upload, verify, self-test).
void th06_midi_audio_attach(void);
// A MIDI player is running (SC or ME).
int th06_midi_playable(void);
// Game thread: prepare the 208 KiB aligned song arena before scene allocations,
// borrowing the disabled ME vertex arena when available (else heap storage).
// NULL requests fresh storage; 1 means ready, 0 means unavailable.
// Non-NULL transfers a retired, 64-byte-aligned WAV 384 KiB malloc block:
// caller must first stop audio producer/file-reader access under their locks.
// 1 takes ownership (caller must not free it), 0 leaves ownership unchanged.
// A static loan can make song storage ready while returning 0 for non-NULL:
// the supplied WAV block was not taken and still belongs to the caller.
// No commands, waits, synthesis, or song/ACK lifetime changes occur here.
int th06_midi_prepare_song_storage(void *retiredWavPcm);

// BGM producer thread: PCM for `frames` (<= 1024) stereo 44.1 kHz frames, or
// NULL when MIDI is idle.  The buffer stays valid until the next call.
const short *th06_midi_render_for_mix(unsigned int frames);

// Game thread: replacements for the engine's MidiOutput device layer.
int th06_midi_hooks_active(void);
void th06_midi_hook_read_file(unsigned int slot, const char *path);
void th06_midi_hook_load_file(const char *path);
void th06_midi_hook_parse(int slot);
void th06_midi_hook_play(void);
void th06_midi_hook_stop(void);
void th06_midi_hook_fade(unsigned int ms);

typedef struct Th06MidiDiag
{
    unsigned int mode;          // 0 off, 1 SC, 2 ME, 3 silent MEMSIM (test only)
    unsigned int meJobs;        // RENDER jobs on the ME in the window
    unsigned int scJobs;        // renders on the SC in the window
    unsigned int frames;        // frames rendered in the window
    unsigned int waitTotalUs;   // ME: wall time of the jobs; SC: render time
    unsigned int waitMaxUs;
    unsigned int loadPermille;  // waitTotal / audio time of `frames`
    unsigned int renderCyclesMax; // ME CP0 count delta of the largest render part
    unsigned int scrubCyclesMax;
    unsigned int activeMax;
    unsigned int notesLost;     // cumulative (player since its last init)
    unsigned int voicesStolen;  // cumulative
    unsigned int scrubPasses, scrubBad, scrubRepaired; // cumulative (ME)
    unsigned int queueDrops;    // cumulative
    unsigned int fallbacks;     // cumulative ME -> SC/off switches
    unsigned int fallbackReason; // 0 none, 1 claim, 2 timeout/pinned, 3 job error, 4 rot, 5 SC output failure
    unsigned int songsLoaded;   // cumulative SMF loads (game thread)
    unsigned int songsFreed;    // cumulative SMF buffers released after the player let go
    unsigned int commands;      // cumulative commands queued (PLAY/STOP/FADE)
    unsigned int pinnedSmfBytes, pinnedBlobBytes; // measured pin set, not a PSP-1000 RAM safety proof
} Th06MidiDiag;
void th06_midi_diag_window(Th06MidiDiag *out);

#if defined(TH06_PSP_MIDI_PCMTEST)
// Test EBOOT only: render the fixed PCM scripts and log their hashes.
void th06_midi_pcm_test(void);
#endif

#ifdef __cplusplus
}
#endif
