// TH06 PSP: MIDI (gm.dls) synthesis on the Media Engine -- the ME eDRAM layout
// and the job protocol shared by the SC side (psp/midi/MidiPsp.cpp), the ME side
// (psp/midi/MidiMe.cpp) and the mailbox owner (psp/audio_me.c).
//
// C and C++ both include this header.  Every field of Th06MidiMeBox lives in
// the uncached mailbox (audio_me.c), so neither side needs cache maintenance
// for it.  The ME never touches SC objects: it reads the blob copy, the block
// checksums and the SMF bytes through addresses passed here, and writes only
// its own eDRAM, the output PCM range named by the job, and this box.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
#define TH06_MIDI_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define TH06_MIDI_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

// ---------------------------------------------------------------------------
// ME eDRAM layout (ME-local physical offsets; the ME uses 0x80000000|offset
// for cached and 0xA0000000|offset for uncached access).
//
//   0x000000 .. 0x000400   untouched (PSPPMD baseline never used it)
//   0x000400 .. 0x000C00   PCM mix accumulator, 2 KiB (audio_me.c, unchanged)
//   0x000C00 .. 0x001000   guard
//   0x001000 .. 0x011000   MIDI state, 64 KiB (player, sequencer, synth, arena)
//   0x011000 .. 0x1C0000   MIDI blob (TDSB v1), up to 1,765,376 bytes
//   0x1C0000 .. 0x200000   kept free: MECC starts the ME stack at the top of
//                          the PSP-1000's 2 MiB (0x80200000) and it grows down
//
// PSP-2000 and later have 4 MiB and MECC puts the stack at 0x80400000; the
// same lower-2-MiB layout is used there, so one layout serves every model.
// The ME reports its $sp at init and the SC refuses the ME path if the stack
// is not above TH06_MIDI_EDRAM_STACK_FLOOR (psp/midi/MidiPsp.cpp).
// ---------------------------------------------------------------------------
#define TH06_MIDI_EDRAM_ACCUM_BASE 0x00000400u
#define TH06_MIDI_EDRAM_ACCUM_END 0x00000C00u
#define TH06_MIDI_EDRAM_STATE_BASE 0x00001000u
#define TH06_MIDI_EDRAM_STATE_BYTES 0x00010000u
#define TH06_MIDI_EDRAM_STATE_END (TH06_MIDI_EDRAM_STATE_BASE + TH06_MIDI_EDRAM_STATE_BYTES)
#define TH06_MIDI_EDRAM_BLOB_BASE 0x00011000u
#define TH06_MIDI_EDRAM_STACK_FLOOR 0x001C0000u
#define TH06_MIDI_EDRAM_BLOB_CAPACITY (TH06_MIDI_EDRAM_STACK_FLOOR - TH06_MIDI_EDRAM_BLOB_BASE)
#define TH06_MIDI_EDRAM_FAT_TOP 0x00200000u
#define TH06_MIDI_EDRAM_STACK_RESERVE (TH06_MIDI_EDRAM_FAT_TOP - TH06_MIDI_EDRAM_STACK_FLOOR)

TH06_MIDI_STATIC_ASSERT(TH06_MIDI_EDRAM_ACCUM_END <= TH06_MIDI_EDRAM_STATE_BASE,
                        "MIDI state overlaps the 2 KiB mix accumulator");
TH06_MIDI_STATIC_ASSERT(TH06_MIDI_EDRAM_STATE_END <= TH06_MIDI_EDRAM_BLOB_BASE, "MIDI state overlaps the blob");
TH06_MIDI_STATIC_ASSERT((TH06_MIDI_EDRAM_BLOB_BASE & 63u) == 0, "blob base must be 64-byte aligned");
TH06_MIDI_STATIC_ASSERT(TH06_MIDI_EDRAM_STACK_RESERVE == 0x40000u,
                        "keep 256 KiB below the PSP-1000 ME stack top (docs/11 5.4)");
TH06_MIDI_STATIC_ASSERT(TH06_MIDI_EDRAM_BLOB_BASE + TH06_MIDI_EDRAM_BLOB_CAPACITY == TH06_MIDI_EDRAM_STACK_FLOOR,
                        "blob area ends at the stack reserve");
TH06_MIDI_STATIC_ASSERT(TH06_MIDI_EDRAM_BLOB_CAPACITY >= 1196404u,
                        "the TH06 rule-A 16-bit blob (1,196,404 B) must fit on the PSP-1000");

// The blob is checked in 2 KiB blocks (FNV-1a over little-endian 32-bit
// words, the same function as TH10 SeEdram.hpp / TH07 HashBgmBlock).  The
// last block is padded with zero bytes before hashing.
#define TH06_MIDI_BLOCK_BYTES 2048u
#define TH06_MIDI_BLOCK_WORDS (TH06_MIDI_BLOCK_BYTES / 4u)
#define TH06_MIDI_MAX_BLOCKS (TH06_MIDI_EDRAM_BLOB_CAPACITY / TH06_MIDI_BLOCK_BYTES)
#define TH06_MIDI_FNV_INIT 2166136261u
#define TH06_MIDI_FNV_PRIME 16777619u

// Blocks re-read against their checksum per RENDER job (the background scrub).
// 4 blocks = 8 KiB per job; a 1.2 MB blob is covered in ~150 jobs (~1.7 s of
// 512-frame jobs).  A bad block is rewritten from the SC's immutable copy.
#define TH06_MIDI_SCRUB_BLOCKS_PER_JOB 4u

// Largest RENDER job (stereo frames).  MixAudio never asks for more than 1024.
#define TH06_MIDI_MAX_RENDER_FRAMES 1024u

enum Th06MidiMeOp
{
    TH06_MIDI_OP_NONE = 0,
    TH06_MIDI_OP_LOAD = 1,   // copy the blob into eDRAM and read every block back
    TH06_MIDI_OP_INIT = 2,   // (re)build player/sequencer/synth in the eDRAM state area
    TH06_MIDI_OP_RENDER = 3, // render `frames` into outPhys, then scrub a slice
    TH06_MIDI_OP_INFO = 4,   // read VME/stack environment; no eDRAM writes
};

enum Th06MidiMeResult
{
    TH06_MIDI_RESULT_PENDING = 0,
    TH06_MIDI_RESULT_OK = 1,
    TH06_MIDI_RESULT_BAD_ARGS = 2,
    TH06_MIDI_RESULT_MISMATCH = 3, // LOAD read-back differs from the SC checksums
    TH06_MIDI_RESULT_BLOB = 4,     // tdsbOpen rejected the eDRAM copy
    TH06_MIDI_RESULT_MEMORY = 5,   // synth arena does not fit the state area
    TH06_MIDI_RESULT_NOT_READY = 6,
    TH06_MIDI_RESULT_VME_MODE = 7, // mode must be exactly 2 before any eDRAM writes
};

// One per mailbox.  The SC writes the inputs, then the mailbox command; the ME
// writes the outputs and completes the mailbox command (audio_me.c).
typedef struct Th06MidiMeBox
{
    uint32_t op;
    uint32_t result;

    // LOAD inputs: the SC's immutable blob copy and its block checksum table
    // (both physical; the table is read through the uncached alias).
    uint32_t blobPhys;
    uint32_t blobBytes;
    uint32_t sumsPhys;
    uint32_t blockCount;
    // LOAD outputs
    uint32_t loadBadBlocks;
    uint32_t loadFirstBad;

    // INIT inputs: synth configuration and the command queue address (the
    // uncached alias of a ZunCommandQueue in main RAM).
    uint32_t queueAddr;
    uint32_t sampleRate;
    uint32_t voices;
    uint32_t reserveVoices;
    uint32_t mapRule;
    int32_t gainAdjust;
    uint32_t masterGainQ16;
    uint32_t drumHold;
    uint32_t quantMs;     // ZunMidiPlayer::setEventQuantization period (0, 1 or 10)
    // INFO / INIT outputs (LOAD also refreshes the environment fields)
    uint32_t stateBytes;  // bytes of the 64 KiB state area in use
    uint32_t vmeMode;     // hw(0xBCC00040) read on the ME (must be exactly 2)
    uint32_t meSp;        // $sp on the ME at init (stack top check)
    uint32_t meWitness;   // ME image word at 0x88300018 (selects the stack top)
    uint32_t tdsbStatus;

    // RENDER inputs
    uint32_t outPhys;
    uint32_t frames;
    // RENDER outputs
    uint32_t elapsedMs;    // ZunMidiPlayer::elapsedMs() low 32 bits after the job
    uint32_t activeVoices; // after the job
    uint32_t seqRunning;   // ZunSequencer::timerRunning()
    uint32_t renderCycles; // CP0 Count delta of the render part (diagnostic)
    uint32_t scrubCycles;  // CP0 Count delta of the scrub part
    uint32_t noteOns, notesLost, voicesStolen, maxActive, clippedSamples, mixPeak;

    // Scrub state (owned by the ME after LOAD; the SC only reads it)
    uint32_t scrubCursor;
    uint32_t scrubPasses;
    uint32_t scrubBad;     // blocks found different from their checksum
    uint32_t scrubRepaired;
    uint32_t reserved[4];
} Th06MidiMeBox;

TH06_MIDI_STATIC_ASSERT(sizeof(Th06MidiMeBox) % 4u == 0, "box is word-sized");

#ifdef __cplusplus
extern "C" {
#endif
// ME side (psp/midi/MidiMe.cpp), called from the ME loop in audio_me.c.
void th06_midi_me_execute(volatile Th06MidiMeBox *box);
#ifdef __cplusplus
}
#endif
