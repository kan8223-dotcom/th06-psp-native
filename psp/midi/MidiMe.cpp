// TH06 PSP: the Media Engine side of MIDI synthesis.  Everything here runs on
// the ME, called from the mailbox loop in psp/audio_me.c (meLibOnProcess).
//
// Rules for this file (checked on the linked ELF by tools/midi/me_code_check.py):
//  - no static or global data: the state lives in the ME eDRAM state area and
//    every input arrives through the uncached mailbox (Th06MidiMeBox);
//  - no FPU/VFPU, no syscalls, no SC library calls other than the integer
//    helpers the shared synthesizer already needs (memset/memcpy/__udivdi3);
//  - the synthesizer, sequencer and player are the same object code the SC
//    uses (psp/midi/tdls_*.o), so SC and ME output can be compared bit for bit.
#include "midi_me_protocol.h"
#include "MidiSettings.hpp"

#include <new>
#include <stdint.h>

#include "tdls/blob.hpp"
#include "tdls/synth.hpp"
#include "tdls/zun_seq.hpp"

extern "C" {
void meLibDcacheInvalidateRange(const unsigned int addr, const unsigned int size);
void meLibDcacheWritebackRange(const unsigned int addr, const unsigned int size);
}

namespace {

using tdls::u32;
using tdls::u8;

// The state area holds these objects followed by the synthesizer's arena.
struct MeMidiState
{
    u32 magic;
    u32 ready;
    tdls::TdsbView view;
    tdls::ZunMidiPlayer player;
    tdls::ZunSequencer seq;
    tdls::Synth synth;
    // 22,050 Hz synthesis: the same 2x sample repetition as the SC (MidiSettings.hpp)
    u32 upsample;
    th06midi::Upsample2x up;
    tdls::s16 upTmp[(TH06_MIDI_MAX_RENDER_FRAMES / 2 + 1) * 2];
};

constexpr u32 kStateMagic = 0x4d494453u; // "SDIM"
constexpr u32 kArenaOffset = (static_cast<u32>(sizeof(MeMidiState)) + 63u) & ~63u;
static_assert(kArenaOffset + 32u * 1024u <= TH06_MIDI_EDRAM_STATE_BYTES,
              "player/sequencer/synth objects leave less than 32 KiB of arena in the 64 KiB state area");

inline MeMidiState *State()
{
    return reinterpret_cast<MeMidiState *>(0x80000000u | TH06_MIDI_EDRAM_STATE_BASE);
}

inline volatile u32 *EdramUncached(u32 offset)
{
    return reinterpret_cast<volatile u32 *>(0xA0000000u | offset);
}

inline u32 CpuCount()
{
    u32 value;
    asm volatile("mfc0 %0, $9" : "=r"(value));
    return value;
}

inline u32 Fnv(u32 hash, u32 word)
{
    return (hash ^ word) * TH06_MIDI_FNV_PRIME;
}

u32 EdramBlockSum(u32 offset)
{
    const volatile u32 *p = EdramUncached(offset);
    u32 hash = TH06_MIDI_FNV_INIT;
    for (u32 w = 0; w < TH06_MIDI_BLOCK_WORDS; ++w)
        hash = Fnv(hash, p[w]);
    return hash;
}

u32 ExpectedSum(volatile Th06MidiMeBox *box, u32 block)
{
    const volatile u32 *sums = reinterpret_cast<const volatile u32 *>(0x40000000u | box->sumsPhys);
    return sums[block];
}

// Copy one 2 KiB block from the SC's blob copy (main RAM, immutable after the
// SC wrote it back) into eDRAM through the uncached alias, then drop any
// cached eDRAM lines of that block so later cached reads see the new words.
void CopyBlock(const u32 *source, u32 block)
{
    volatile u32 *dst = EdramUncached(TH06_MIDI_EDRAM_BLOB_BASE + block * TH06_MIDI_BLOCK_BYTES);
    const u32 *src = source + block * TH06_MIDI_BLOCK_WORDS;
    for (u32 w = 0; w < TH06_MIDI_BLOCK_WORDS; ++w)
        dst[w] = src[w];
    asm volatile("sync" ::: "memory");
    meLibDcacheInvalidateRange(0x80000000u | (TH06_MIDI_EDRAM_BLOB_BASE + block * TH06_MIDI_BLOCK_BYTES),
                               TH06_MIDI_BLOCK_BYTES);
}

// INFO is safe before the retention gate: it writes only the mailbox, not
// eDRAM.  LOAD and INIT repeat the mode check immediately before their first
// eDRAM write, so a stale SC-side INFO cannot authorize an unsafe write.
void ReadEnvironment(volatile Th06MidiMeBox *box)
{
    u32 sp;
    asm volatile("move %0, $sp" : "=r"(sp));
    box->meSp = sp;
    box->meWitness = *reinterpret_cast<const volatile u32 *>(0x88300018u);
    box->vmeMode = *reinterpret_cast<const volatile u32 *>(0xBCC00040u);
}

void Info(volatile Th06MidiMeBox *box)
{
    ReadEnvironment(box);
    box->result = TH06_MIDI_RESULT_OK;
}

void Load(volatile Th06MidiMeBox *box)
{
    ReadEnvironment(box);
    box->loadBadBlocks = 0;
    box->loadFirstBad = 0xffffffffu;
    if (box->vmeMode != 2u)
    {
        box->result = TH06_MIDI_RESULT_VME_MODE;
        return;
    }
    const u32 phys = box->blobPhys;
    const u32 bytes = box->blobBytes;
    const u32 blocks = box->blockCount;
    if (phys == 0 || (phys & 63u) != 0 || bytes == 0 || blocks == 0 || blocks > TH06_MIDI_MAX_BLOCKS ||
        bytes > blocks * TH06_MIDI_BLOCK_BYTES || box->sumsPhys == 0)
    {
        box->result = TH06_MIDI_RESULT_BAD_ARGS;
        return;
    }
    // A new blob invalidates any synthesizer that pointed into the old one.
    State()->ready = 0;
    State()->magic = 0;

    const u32 copyBytes = blocks * TH06_MIDI_BLOCK_BYTES;
    meLibDcacheInvalidateRange(0x80000000u | phys, copyBytes);
    const u32 *source = reinterpret_cast<const u32 *>(0x80000000u | phys);
    u32 bad = 0;
    u32 firstBad = 0xffffffffu;
    for (u32 block = 0; block < blocks; ++block)
    {
        CopyBlock(source, block);
        if (EdramBlockSum(TH06_MIDI_EDRAM_BLOB_BASE + block * TH06_MIDI_BLOCK_BYTES) != ExpectedSum(box, block))
        {
            if (bad == 0)
                firstBad = block;
            ++bad;
        }
    }
    box->loadBadBlocks = bad;
    box->loadFirstBad = firstBad;
    box->scrubCursor = 0;
    box->scrubPasses = 0;
    box->scrubBad = 0;
    box->scrubRepaired = 0;
    box->result = bad == 0 ? TH06_MIDI_RESULT_OK : TH06_MIDI_RESULT_MISMATCH;
}

void Init(volatile Th06MidiMeBox *box)
{
    ReadEnvironment(box);
    if (box->vmeMode != 2u)
    {
        box->result = TH06_MIDI_RESULT_VME_MODE;
        return;
    }
    const u32 bytes = box->blobBytes;
    if (box->loadBadBlocks != 0 || bytes == 0 || box->queueAddr == 0)
    {
        box->result = TH06_MIDI_RESULT_NOT_READY;
        return;
    }
    MeMidiState *state = new (State()) MeMidiState();
    state->magic = 0;
    state->ready = 0;
    const void *blob = reinterpret_cast<const void *>(0x80000000u | TH06_MIDI_EDRAM_BLOB_BASE);
    const tdls::TdsbStatus status = tdls::tdsbOpen(blob, bytes, state->view);
    box->tdsbStatus = static_cast<u32>(status);
    if (status != tdls::TDSB_OK)
    {
        box->result = TH06_MIDI_RESULT_BLOB;
        return;
    }
    tdls::SynthConfig config;
    tdls::synthDefaultConfig(config);
    config.sampleRate = box->sampleRate;
    config.voices = box->voices;
    config.reserveVoices = box->reserveVoices;
    config.mapRule = box->mapRule;
    config.gainAdjust = box->gainAdjust;
    config.masterGainQ16 = box->masterGainQ16;
    config.drumHold = box->drumHold;

    u8 *arenaBase = reinterpret_cast<u8 *>(0x80000000u | (TH06_MIDI_EDRAM_STATE_BASE + kArenaOffset));
    const u32 arenaBytes = TH06_MIDI_EDRAM_STATE_BYTES - kArenaOffset;
    const u32 need = tdls::Synth::memoryNeeded(state->view, config);
    if (need > arenaBytes)
    {
        box->stateBytes = kArenaOffset + need;
        box->result = TH06_MIDI_RESULT_MEMORY;
        return;
    }
    tdls::Arena arena;
    arena.init(arenaBase, arenaBytes);
    tdls::ZunCommandQueue *queue = reinterpret_cast<tdls::ZunCommandQueue *>(box->queueAddr);
    if (!state->synth.init(state->view, config, arena) || !state->player.init(&state->synth, &state->seq, queue) ||
        !state->player.setEventQuantization(box->quantMs, 0u))
    {
        box->result = TH06_MIDI_RESULT_MEMORY;
        return;
    }
    box->stateBytes = kArenaOffset + arena.used;
    state->upsample = config.sampleRate == 22050u ? 1u : 0u;
    state->magic = kStateMagic;
    state->ready = 1;
    box->result = TH06_MIDI_RESULT_OK;
}

void Scrub(volatile Th06MidiMeBox *box)
{
    const u32 blocks = box->blockCount;
    if (blocks == 0 || box->loadBadBlocks != 0)
        return;
    // blobPhys is 0 once the SC released its RAM copy (32 MB): detect only.
    const u32 sourcePhys = box->blobPhys;
    const u32 *source = reinterpret_cast<const u32 *>(0x80000000u | sourcePhys);
    u32 cursor = box->scrubCursor;
    for (u32 i = 0; i < TH06_MIDI_SCRUB_BLOCKS_PER_JOB; ++i)
    {
        if (cursor >= blocks)
        {
            cursor = 0;
            box->scrubPasses = box->scrubPasses + 1u;
        }
        const u32 offset = TH06_MIDI_EDRAM_BLOB_BASE + cursor * TH06_MIDI_BLOCK_BYTES;
        const u32 expected = ExpectedSum(box, cursor);
        if (EdramBlockSum(offset) != expected)
        {
            box->scrubBad = box->scrubBad + 1u;
            if (sourcePhys != 0)
            {
                CopyBlock(source, cursor);
                if (EdramBlockSum(offset) == expected)
                    box->scrubRepaired = box->scrubRepaired + 1u;
            }
        }
        ++cursor;
    }
    box->scrubCursor = cursor;
}

void Render(volatile Th06MidiMeBox *box)
{
    MeMidiState *state = State();
    const u32 frames = box->frames;
    const u32 outPhys = box->outPhys;
    if (state->magic != kStateMagic || !state->ready)
    {
        box->result = TH06_MIDI_RESULT_NOT_READY;
        return;
    }
    if (frames == 0 || frames > TH06_MIDI_MAX_RENDER_FRAMES || outPhys == 0 || (outPhys & 63u) != 0)
    {
        box->result = TH06_MIDI_RESULT_BAD_ARGS;
        return;
    }
    const u32 t0 = CpuCount();
    tdls::s16 *out = reinterpret_cast<tdls::s16 *>(0x80000000u | outPhys);
    if (state->upsample)
        th06midi::RenderUpsampled2x(state->player, state->up, out, frames, state->upTmp);
    else
        state->player.render(out, frames);
    if (state->player.outputFailed())
    {
        // The core has already cleared pending output/stopped/powered off.
        // Report a failed job, never successful silence; explicit INIT only.
        state->ready = 0;
        box->result = TH06_MIDI_RESULT_NOT_READY;
        return;
    }
    meLibDcacheWritebackRange(reinterpret_cast<u32>(out), (frames * 4u + 63u) & ~63u);
    const u32 t1 = CpuCount();
    Scrub(box);
    const u32 t2 = CpuCount();

    const tdls::SynthStats &stats = state->synth.stats();
    box->elapsedMs = static_cast<u32>(state->player.elapsedMs());
    box->activeVoices = state->synth.activeVoices();
    box->seqRunning = state->seq.timerRunning() ? 1u : 0u;
    box->renderCycles = t1 - t0;
    box->scrubCycles = t2 - t1;
    box->noteOns = stats.noteOns;
    box->notesLost = stats.notesLost;
    box->voicesStolen = stats.voicesStolen;
    box->maxActive = stats.maxActive;
    box->clippedSamples = stats.clippedSamples;
    box->mixPeak = stats.mixPeak;
    box->result = TH06_MIDI_RESULT_OK;
}

} // namespace

extern "C" void th06_midi_me_execute(volatile Th06MidiMeBox *box)
{
    const u32 op = box->op;
    box->result = TH06_MIDI_RESULT_PENDING;
    if (op == TH06_MIDI_OP_LOAD)
        Load(box);
    else if (op == TH06_MIDI_OP_INIT)
        Init(box);
    else if (op == TH06_MIDI_OP_RENDER)
        Render(box);
    else if (op == TH06_MIDI_OP_INFO)
        Info(box);
    else
        box->result = TH06_MIDI_RESULT_BAD_ARGS;
}
