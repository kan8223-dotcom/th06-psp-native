// TH06 PSP: run-time MIDI synthesis from the user's gm.dls.  See MidiPsp.hpp.
#include "MidiPsp.hpp"

#include <_newlib_version.h>
#include <malloc.h>
#include <new>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "FileSystem.hpp"
#include "Supervisor.hpp"
#include "audio_me.h"
#include "diag.hpp"
#include "fileio.hpp"
#include "i18n.hpp"
#include "midi/MidiBuild.hpp"
#include "midi/MidiScript.hpp"
#include "midi/MidiSettings.hpp"
#include "midi/MidiFailurePolicy.hpp"
#include "midi/Pbg3SongDecode.hpp"
#include "midi/midi_me_protocol.h"
#include "pbg3/Pbg3Archive.hpp"
#include "tdls/blob.hpp"
#include "tdls/sha256.hpp"
#include "tdls/synth.hpp"
#include "tdls/zun_seq.hpp"

#ifndef TH06_TDLS_SOURCE_ID
#define TH06_TDLS_SOURCE_ID 0u
#endif

namespace
{

using tdls::s16;
using tdls::u32;
using tdls::u8;
inline unsigned int U(u32 v);

// ------------------------------------------------------------------ globals
th06midi::Settings gSettings;
bool gSettingsLoaded;

u8 *gBlob;               // the subset blob (TDSB v1), 64-byte aligned
u32 gBlobBytes;
u32 gBlobStorage;        // rounded up to whole 2 KiB blocks, zero tail
u8 gBlobSha[32];
volatile int gBlobReady;

enum Mode : int
{
    MODE_OFF = 0,
    MODE_SC = 1,
    MODE_ME = 2,
    MODE_MEMSIM = 3, // test EBOOT only: ME-mode RAM footprint, no synthesis
};
volatile int gMode;
volatile u32 gMidiFailed;
volatile u32 gMePinRequested;
th06midi::MeBorrowedPayloads gMeBorrowed;
bool gMeBlobExposed;
bool gMeBlobPinned;
u32 gMeLastElapsed;
static_assert(th06midi::MeBorrowedPayloads::kCapacity >= tdls::ZunCommandQueue::kCapacity + 1,
              "read set must retain current SMF plus all queued PLAY payloads");

void PinUnacknowledgedMePayloads()
{
    gMeBorrowed.freeze();
    gMeBlobPinned = gMeBlobExposed && gBlob != nullptr;
    __atomic_store_n(&gMePinRequested, 1u, __ATOMIC_RELEASE);
}

// The 32 MB PSP-1000 cannot keep the 1.2 MB blob in main RAM next to the
// game: PPSSPP model 0 failed the stage 1 -> 2 GUI texture allocation with it
// resident.  There the blob lives only in ME eDRAM after the self-test, and a
// later ME failure turns MIDI off (the next song plays as WAV) instead of an
// SC fallback.  64 MB models keep the RAM copy for the SC fallback.
bool gSmallRam;

// SC player (also the reference for the boot self-test and the fallback)
tdls::TdsbView gScView;
tdls::Synth gScSynth;
tdls::ZunSequencer gScSeq;
tdls::ZunCommandQueue gScQueue;
tdls::ZunMidiPlayer gScPlayer;
u8 *gScArena;
u32 gScArenaBytes;
th06midi::Upsample2x gScUp;
s16 gScUpTmp[(TH06_MIDI_MAX_RENDER_FRAMES / 2 + 1) * 2];

// ME player: the queue lives in main RAM and is used only through its
// uncached alias (the SC producer pushes, the ME pops).
alignas(64) unsigned char gMeQueueRaw[(sizeof(tdls::ZunCommandQueue) + 63u) & ~63u];
tdls::ZunCommandQueue *gMeQueue;
alignas(64) u32 gBlockSums[TH06_MIDI_MAX_BLOCKS];

alignas(64) s16 gMidiMeOut[TH06_MIDI_MAX_RENDER_FRAMES * 2];
alignas(64) s16 gMidiScOut[TH06_MIDI_MAX_RENDER_FRAMES * 2];
static_assert(sizeof(gMidiMeOut) % 64u == 0, "ME output range must be whole cache lines");

// ---- game thread -> producer thread command ring (single writer/reader)
struct GameCommand
{
    u32 type;
    u32 arg;
    const u8 *data;
    u32 size;
};
constexpr u32 kRingSize = 64;
GameCommand gRing[kRingSize];
volatile u32 gRingHead; // written by the game thread (commands pushed)
volatile u32 gRingTail; // written by the producer (commands forwarded)
volatile u32 gApplied;  // producer: commands the player has applied
volatile u32 gQueueDrops;

// Intended song (game thread writes under a sequence counter; the producer
// reads it only to restart the song after an ME -> SC fallback).
volatile u32 gIntentSeq;
const u8 *volatile gIntentData;
volatile u32 gIntentSize;
volatile u32 gIntentPlaying;

// Song buffers (game thread only)
struct SongSlot
{
    u8 *data;
    u32 size;
    bool mePinned;
};
SongSlot gSlots[32];
int gSelectedSlot = -1;
struct Retired
{
    u8 *data;
    u32 retireAt;
};
Retired gRetired[16];

// Reuse part of the WAV mode's 384 KiB budget only when MIDI is selected.
// The old permanent 3 x 120 KiB pool remained resident even in WAV mode.
// Retail TH06's two largest songs are 113,871 and 94,384 bytes; rounding each
// to 64 bytes needs 208,320 bytes, below this 212,992-byte arena.  The engine
// normally keeps boss in slot 1 and stage/title/ending in slot 31.  Place
// those at opposite ends so replacing either size leaves room for the other.
// Retired/pinned spans keep their addresses until the existing ACK policy
// releases them.  Never compact or overwrite a live span to make space.
constexpr u32 kSongPoolBytes = 208u * 1024u;
constexpr u32 kSongPoolSpanCount = 32u + 16u;
struct SongSpan
{
    u32 offset;
    u32 bytes;
};
u8 *gSongPool;
bool gSongPoolStaticLoan;
SongSpan gSongSpans[kSongPoolSpanCount];
void LogSongPoolState(u32 slot, u32 size);
bool InSongPool(const void *p)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(gSongPool);
    return gSongPool != nullptr && address >= begin && address - begin < kSongPoolBytes;
}
void ReleaseSong(u8 *p)
{
    if (!p) return;
    if (InSongPool(p))
    {
        const u32 offset = static_cast<u32>(p - gSongPool);
        for (SongSpan &span : gSongSpans)
        {
            if (span.bytes != 0 && span.offset == offset)
            {
                span = SongSpan{};
                return;
            }
        }
        // A pool address must never go to libc free, even if metadata is bad.
        th06_psp_boot_note("MIDI SONG POOL FREE UNKNOWN");
        return;
    }
    free(p);
}

bool FindSongSpan(u32 bytes, bool fromHigh, u32 &offset)
{
    offset = fromHigh ? kSongPoolBytes - bytes : 0;
    // Each overlap moves monotonically past a live span.  The bound also
    // makes corrupt metadata fail closed instead of spinning during a load.
    for (u32 pass = 0; pass <= kSongPoolSpanCount; ++pass)
    {
        bool moved = false;
        for (const SongSpan &span : gSongSpans)
        {
            if (span.bytes == 0 || offset + bytes <= span.offset ||
                offset >= span.offset + span.bytes)
                continue;
            if (fromHigh)
            {
                if (span.offset < bytes) return false;
                offset = span.offset - bytes;
            }
            else
            {
                offset = span.offset + span.bytes;
                if (offset > kSongPoolBytes - bytes) return false;
            }
            moved = true;
            break;
        }
        if (!moved) return true;
    }
    return false;
}

// Reserve without damaging a player's borrowed payload. Failures are
// diagnosed and cannot pass the memory gate: a direct archive load stops;
// the external PoolSong caller retains its old safe heap-buffer fallback.
u8 *ReserveSongSpan(u32 size, u32 slot)
{
    if (size == 0 || size > kSongPoolBytes)
    {
        th06_psp_boot_notef("MIDI SONG POOL FULL SIZE %uB", U(size));
        return nullptr;
    }
    const u32 bytes = (size + 63u) & ~63u;
    SongSpan *available = nullptr;
    for (SongSpan &span : gSongSpans)
        if (span.bytes == 0)
        {
            available = &span;
            break;
        }
    u32 offset = 0;
    if (!available || !FindSongSpan(bytes, slot == 1u, offset))
    {
        th06_psp_boot_notef("MIDI SONG POOL FULL PENDING S%u %uB", U(slot), U(size));
        LogSongPoolState(slot, size);
        return nullptr;
    }
    if (!gSongPool)
    {
        if (!th06_midi_prepare_song_storage(nullptr))
        {
            th06_psp_boot_note("MIDI SONG POOL FULL ALLOC FAILED");
            return nullptr;
        }
    }
    *available = SongSpan{offset, bytes};
    return gSongPool + offset;
}

u8 *PoolSong(u8 *heapData, u32 size, u32 slot)
{
    u8 *pooled = ReserveSongSpan(size, slot);
    if (!pooled) return heapData;
    memcpy(pooled, heapData, size);
    free(heapData);
    return pooled;
}
u32 gRetiredCount;

// eDRAM blocks found corrupted by the scrub before MIDI leaves the ME.
constexpr u32 kScrubBadLimit = 8;

// ---- producer-side player status
volatile u32 gBusy;
// ---- diagnostics (producer writes, diag thread exchanges)
volatile u32 gDiagMeJobs, gDiagScJobs, gDiagFrames, gDiagWaitTotal, gDiagWaitMax;
volatile u32 gDiagRenderCyclesMax, gDiagScrubCyclesMax, gDiagActiveMax;
volatile u32 gStatNotesLost, gStatStolen, gStatScrubPasses, gStatScrubBad, gStatScrubRepaired;
volatile u32 gFallbacks, gFallbackReason;
volatile u32 gSongsLoaded, gSongsFreed, gSongCommands; // game thread

void RecordMax(volatile u32 *slot, u32 value)
{
    u32 old = __atomic_load_n(slot, __ATOMIC_RELAXED);
    while (value > old && !__atomic_compare_exchange_n(slot, &old, value, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    {
    }
}

inline unsigned int U(u32 v)
{
    return static_cast<unsigned int>(v);
}

inline u32 Phys(const void *p)
{
    return static_cast<u32>(reinterpret_cast<uintptr_t>(p)) & 0x1fffffffu;
}

inline const u8 *Uncached(const u8 *p)
{
    return p ? reinterpret_cast<const u8 *>(0x40000000u | Phys(p)) : p;
}

u32 NowUs()
{
    return sceKernelGetSystemTimeLow();
}

// ------------------------------------------------------------- file helpers
// Large sequential reads only (PSP Go ef0 stalls on small read/seek runs,
// memory psp-go-ef0-tiny-read-stall).
constexpr u32 kIoChunk = 256u * 1024u;

bool ReadExact(SceUID fd, void *dst, u32 bytes)
{
    u8 *p = static_cast<u8 *>(dst);
    while (bytes > 0)
    {
        const u32 n = bytes > kIoChunk ? kIoChunk : bytes;
        if (sceIoRead(fd, p, n) != static_cast<int>(n))
            return false;
        p += n;
        bytes -= n;
    }
    return true;
}

bool WriteExact(SceUID fd, const void *src, u32 bytes)
{
    const u8 *p = static_cast<const u8 *>(src);
    while (bytes > 0)
    {
        const u32 n = bytes > kIoChunk ? kIoChunk : bytes;
        if (sceIoWrite(fd, p, n) != static_cast<int>(n))
            return false;
        p += n;
        bytes -= n;
    }
    return true;
}

u8 *ReadWholeFile(const char *path, u32 *size)
{
    *size = 0;
    SceIoStat st{};
    if (sceIoGetstat(path, &st) < 0 || st.st_size <= 0 || st.st_size > 64ll * 1024 * 1024)
        return nullptr;
    const u32 bytes = static_cast<u32>(st.st_size);
    u8 *data = static_cast<u8 *>(memalign(64, bytes));
    if (!data)
        return nullptr;
    const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    const bool ok = fd >= 0 && ReadExact(fd, data, bytes);
    if (fd >= 0)
        sceIoClose(fd);
    if (!ok)
    {
        free(data);
        return nullptr;
    }
    *size = bytes;
    return data;
}

bool JoinPath(char *out, u32 cap, const char *dir, const char *name)
{
    const int n = std::snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && static_cast<u32>(n) < cap;
}

// ------------------------------------------------------------ settings
void LoadSettings()
{
    if (gSettingsLoaded)
        return;
    gSettingsLoaded = true;
    th06midi::DefaultSettings(gSettings);
    char path[320];
    if (!JoinPath(path, sizeof(path), th06_psp_game_dir(), "MIDI.CFG"))
        return;
    u32 size = 0;
    u8 *text = ReadWholeFile(path, &size);
    if (!text)
        return;
    u32 line = 0;
    while (line < size)
    {
        u32 end = line;
        while (end < size && text[end] != '\n' && text[end] != '\r')
            ++end;
        char key[24] = {};
        char value[24] = {};
        u32 k = 0, v = 0;
        u32 i = line;
        while (i < end && text[i] != '=' && k + 1 < sizeof(key))
            key[k++] = static_cast<char>(text[i++]);
        if (i < end && text[i] == '=')
        {
            ++i;
            while (i < end && v + 1 < sizeof(value))
                value[v++] = static_cast<char>(text[i++]);
            const long number = std::strtol(value, nullptr, 0);
            if (std::strcmp(key, "rule") == 0)
                gSettings.rule = value[0] == 'A' || value[0] == 'a'
                                     ? tdls::MAP_A_CAPITAL
                                     : (value[0] == 'B' || value[0] == 'b' ? tdls::MAP_B_SUBCAPITAL
                                                                           : tdls::MAP_C_STRICT_LSB);
            else if (std::strcmp(key, "voices") == 0 && number >= 1 && number <= 256)
                gSettings.voices = static_cast<u32>(number);
            else if (std::strcmp(key, "reserve") == 0 && number >= 0 && number < 256)
                gSettings.reserveVoices = static_cast<u32>(number);
            else if (std::strcmp(key, "gain_adjust") == 0 && number >= -9600 && number <= 9600)
                gSettings.gainAdjust = static_cast<tdls::s32>(number);
            else if (std::strcmp(key, "master_q16") == 0 && number >= 0 && number <= 0x100000)
                gSettings.masterGainQ16 = static_cast<u32>(number);
            else if (std::strcmp(key, "rate") == 0 && (number == 44100 || number == 22050))
                gSettings.synthRate = static_cast<u32>(number);
            else if (std::strcmp(key, "me") == 0)
                gSettings.useMe = number != 0 ? 1u : 0u;
            else if (std::strcmp(key, "drum_hold") == 0)
                gSettings.drumHold = number != 0 ? 1u : 0u;
            else if (std::strcmp(key, "quant_ms") == 0 && (number == 0 || number == 1 || number == 10))
                gSettings.quantMs = static_cast<u32>(number);
        }
        line = end + 1;
    }
    free(text);
}

// --------------------------------------------------------------- cache file
constexpr char kCacheName[] = "MIDICACHE.BIN";
constexpr char kCacheTemp[] = "MIDICACHE.TMP";
constexpr u32 kCacheVersion = 2;
constexpr u32 kCacheBlobFlags = tdls::BLOB_MSGS_GUARD;

struct CacheHeader
{
    char magic[8];
    u32 headerBytes;
    u32 version;
    u32 builderVersion;
    u32 tdlsSourceId;
    u32 rule;
    u32 blobFlags;
    u32 dlsBytes;
    u8 dlsMtime[16];
    u32 mdBytes;
    u8 mdMtime[16];
    u8 dlsSha[32];
    u8 mdSha[32];
    u32 blobBytes;
    u8 blobSha[32];
    char usageSha[64];
    char listSha[64];
    u32 tones, instruments, regions, waves;
    u8 reserved[512 - 8 - 4 * 7 - 16 - 4 - 16 - 32 - 32 - 4 - 32 - 64 - 64 - 16 - 4];
    u32 headerSum;
};
static_assert(sizeof(CacheHeader) == 512, "cache header is 512 bytes");
static_assert(sizeof(ScePspDateTime) == 16, "ScePspDateTime is 16 bytes");

u32 HeaderSum(const CacheHeader &h)
{
    const u8 *p = reinterpret_cast<const u8 *>(&h);
    u32 hash = TH06_MIDI_FNV_INIT;
    for (u32 i = 0; i < offsetof(CacheHeader, headerSum); ++i)
        hash = (hash ^ p[i]) * TH06_MIDI_FNV_PRIME;
    return hash;
}

struct SourceStamp
{
    char path[320];
    u32 bytes;
    u8 mtime[16];
};

bool StampFile(const char *path, SourceStamp &s)
{
    SceIoStat st{};
    if (!path || sceIoGetstat(path, &st) < 0 || st.st_size <= 0)
        return false;
    std::snprintf(s.path, sizeof(s.path), "%s", path);
    s.bytes = static_cast<u32>(st.st_size);
    std::memcpy(s.mtime, &st.sce_st_mtime, sizeof(s.mtime));
    return true;
}

void Hex8(const u8 *digest, char *out)
{
    char full[65];
    tdls::sha256Hex(digest, full);
    std::memcpy(out, full, 16);
    out[16] = '\0';
}

bool AllocateBlob(u32 bytes)
{
    const u32 storage = (bytes + TH06_MIDI_BLOCK_BYTES - 1u) / TH06_MIDI_BLOCK_BYTES * TH06_MIDI_BLOCK_BYTES;
    gBlob = static_cast<u8 *>(memalign(64, storage));
    if (!gBlob)
        return false;
    std::memset(gBlob + bytes, 0, storage - bytes);
    gBlobBytes = bytes;
    gBlobStorage = storage;
    return true;
}

void ReleaseBlob()
{
    if (gMeBlobPinned) return;
    free(gBlob);
    gBlob = nullptr;
    gBlobBytes = gBlobStorage = 0;
}

void ReleaseUnusedMidiMemory()
{
    // Called before a player is published, or after MIDI is disabled.  A
    // failed boot self-test must not leave the unused 1.2 MB blob resident
    // beside the PSP-1000's game assets.
    free(gScArena);
    gScArena = nullptr;
    gScArenaBytes = 0;
    ReleaseBlob();
    __atomic_store_n(&gBlobReady, 0, __ATOMIC_RELEASE);
}

bool TryLoadCache(const char *cachePath, const SourceStamp &dls, const SourceStamp &md)
{
    const SceUID fd = sceIoOpen(cachePath, PSP_O_RDONLY, 0);
    if (fd < 0)
        return false;
    CacheHeader h;
    bool ok = ReadExact(fd, &h, sizeof(h)) && std::memcmp(h.magic, "TH6MIDC1", 8) == 0 &&
              h.headerBytes == sizeof(h) && h.version == kCacheVersion &&
              h.builderVersion == th06midi::kBuilderVersion && h.tdlsSourceId == TH06_TDLS_SOURCE_ID &&
              h.rule == gSettings.rule && h.blobFlags == kCacheBlobFlags && h.dlsBytes == dls.bytes &&
              std::memcmp(h.dlsMtime, dls.mtime, 16) == 0 && h.mdBytes == md.bytes &&
              std::memcmp(h.mdMtime, md.mtime, 16) == 0 && h.headerSum == HeaderSum(h) && h.blobBytes > 64 &&
              h.blobBytes <= 8u * 1024u * 1024u;
    if (ok)
        ok = AllocateBlob(h.blobBytes) && ReadExact(fd, gBlob, h.blobBytes);
    sceIoClose(fd);
    if (ok)
    {
        tdls::sha256Buffer(gBlob, gBlobBytes, gBlobSha);
        ok = std::memcmp(gBlobSha, h.blobSha, 32) == 0;
    }
    if (!ok)
    {
        ReleaseBlob();
        return false;
    }
    char blob8[17], list8[17];
    Hex8(gBlobSha, blob8);
    std::memcpy(list8, h.listSha, 16);
    list8[16] = '\0';
    th06_psp_boot_notef("MIDI CACHE HIT RULE%c LIST %s BLOB %s %uB", th06midi::RuleLetter(gSettings.rule), list8,
                        blob8, U(gBlobBytes));
    return true;
}

void *BuildAlloc(u32 bytes, u32 align, void *)
{
    return memalign(align < 8 ? 8 : align, bytes ? bytes : 1);
}

void BuildRelease(void *p, void *)
{
    free(p);
}

void BuildProgress(const char *stage, void *)
{
    th06_psp_boot_notef("MIDI BUILD %s", stage);
}

bool BuildCache(const char *cachePath, const SourceStamp &dls, const SourceStamp &md)
{
    const u32 t0 = NowUs();
    th06_psp_boot_note("MIDI CACHE BUILD (FIRST BOOT OR CHANGED FILES)");

    // MD.DAT identity (306 KB) and the songs inside it.
    u32 mdSize = 0;
    u8 *mdRaw = ReadWholeFile(md.path, &mdSize);
    if (!mdRaw)
    {
        th06_psp_boot_note("MIDI OFF (MD.DAT UNREADABLE)");
        return false;
    }
    u8 mdSha[32];
    tdls::sha256Buffer(mdRaw, mdSize, mdSha);
    free(mdRaw);

    Pbg3Archive *archive = new (std::nothrow) Pbg3Archive();
    if (!archive || !archive->Load(TH_MD_DAT_FILE))
    {
        delete archive;
        th06_psp_boot_note("MIDI OFF (MD.DAT NOT PBG3)");
        return false;
    }
    th06midi::BuildFile files[32];
    char names[32][16];
    u8 *songData[32] = {};
    u32 count = 0;
    for (u32 n = 1; n <= 32 && count < 32; ++n)
    {
        std::snprintf(names[count], sizeof(names[count]), "th06_%02u.mid", U(n));
        const i32 entry = archive->FindEntry(names[count]);
        if (entry < 0)
            continue;
        songData[count] = archive->ReadDecompressEntry(static_cast<u32>(entry), names[count]);
        if (!songData[count])
            continue;
        files[count].name = names[count];
        files[count].data = songData[count];
        files[count].size = archive->GetEntrySize(static_cast<u32>(entry));
        ++count;
    }
    delete archive;
    const u32 t1 = NowUs();

    u32 dlsSize = 0;
    u8 *dlsData = count ? ReadWholeFile(dls.path, &dlsSize) : nullptr;
    u8 dlsSha[32] = {};
    if (dlsData)
        tdls::sha256Buffer(dlsData, dlsSize, dlsSha);
    const u32 t2 = NowUs();

    th06midi::BuildResult result;
    const th06midi::BuildAllocator allocator = {BuildAlloc, BuildRelease, nullptr};
    bool ok = dlsData && dlsSize == dls.bytes &&
              th06midi::BuildBlob(dlsData, dlsSize, dlsSha, files, count, "th06",
                                  static_cast<tdls::MapRule>(gSettings.rule), allocator, result, BuildProgress,
                                  nullptr);
    free(dlsData);
    for (u32 i = 0; i < count; ++i)
        free(songData[i]);
    const u32 t3 = NowUs();
    if (!ok)
    {
        th06_psp_boot_notef("MIDI OFF (BUILD: %s)", dlsData ? (result.error ? result.error : "?") : "GM.DLS READ");
        return false;
    }
    if (result.blobBytes > TH06_MIDI_EDRAM_BLOB_CAPACITY)
    {
        th06_psp_boot_notef("MIDI BLOB %uB > EDRAM %uB (SC ONLY)", U(result.blobBytes),
                            U(TH06_MIDI_EDRAM_BLOB_CAPACITY));
    }
    gBlob = result.blob;
    gBlobBytes = result.blobBytes;
    gBlobStorage = result.blobStorageBytes;
    std::memcpy(gBlobSha, result.blobSha256, 32);

    char dls8[17], md8[17], usage8[17], list8[17], blob8[17];
    Hex8(dlsSha, dls8);
    Hex8(mdSha, md8);
    Hex8(gBlobSha, blob8);
    std::memcpy(usage8, result.usageSha, 16);
    usage8[16] = '\0';
    std::memcpy(list8, result.manifestSha, 16);
    list8[16] = '\0';
    th06_psp_boot_notef("MIDI GM.DLS %uB SHA %s MD %s", U(dlsSize), dls8, md8);
    th06_psp_boot_notef("MIDI SONGS %u TONES %u INS %u RGN %u WAVE %u", U(count), U(result.tones),
                        U(result.instruments), U(result.regions), U(result.waves));
    // Full hashes, for comparison with dls_subset_tool's summary.txt
    th06_psp_boot_notef("MIDI USAGE %s", result.usageSha);
    th06_psp_boot_notef("MIDI LIST RULE%c %s", th06midi::RuleLetter(gSettings.rule), result.manifestSha);
    {
        char full[65];
        tdls::sha256Hex(gBlobSha, full);
        th06_psp_boot_notef("MIDI BLOB %uB %s", U(gBlobBytes), full);
    }

    // Write the cache: header + blob, to a temporary name first.
    CacheHeader h;
    std::memset(&h, 0, sizeof(h));
    std::memcpy(h.magic, "TH6MIDC1", 8);
    h.headerBytes = sizeof(h);
    h.version = kCacheVersion;
    h.builderVersion = th06midi::kBuilderVersion;
    h.tdlsSourceId = TH06_TDLS_SOURCE_ID;
    h.rule = gSettings.rule;
    h.blobFlags = kCacheBlobFlags;
    h.dlsBytes = dls.bytes;
    std::memcpy(h.dlsMtime, dls.mtime, 16);
    h.mdBytes = md.bytes;
    std::memcpy(h.mdMtime, md.mtime, 16);
    std::memcpy(h.dlsSha, dlsSha, 32);
    std::memcpy(h.mdSha, mdSha, 32);
    h.blobBytes = gBlobBytes;
    std::memcpy(h.blobSha, gBlobSha, 32);
    std::memcpy(h.usageSha, result.usageSha, 64);
    std::memcpy(h.listSha, result.manifestSha, 64);
    h.tones = result.tones;
    h.instruments = result.instruments;
    h.regions = result.regions;
    h.waves = result.waves;
    h.headerSum = HeaderSum(h);
    char tempPath[320];
    bool written = false;
    if (JoinPath(tempPath, sizeof(tempPath), th06_psp_game_dir(), kCacheTemp))
    {
        const SceUID fd = sceIoOpen(tempPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0)
        {
            written = WriteExact(fd, &h, sizeof(h)) && WriteExact(fd, gBlob, gBlobBytes);
            sceIoClose(fd);
            if (written)
            {
                sceIoRemove(cachePath);
                written = sceIoRename(tempPath, cachePath) >= 0;
            }
            if (!written)
                sceIoRemove(tempPath);
        }
    }
    const u32 t4 = NowUs();
    th06_psp_boot_notef("MIDI BUILD %ums (SONGS %u READ %u BUILD %u WRITE %u) ARENA %u/%u%s",
                        U((t4 - t0) / 1000u), U((t1 - t0) / 1000u), U((t2 - t1) / 1000u), U((t3 - t2) / 1000u),
                        U((t4 - t3) / 1000u), U(result.dlsArenaPeak), U(result.planArenaPeak),
                        written ? "" : " CACHE NOT SAVED");
    return true;
}

bool FindGmDls(SourceStamp &dls)
{
    const char *dirs[2] = {th06_psp_data_dir(), th06_psp_game_dir()};
    const char *names[3] = {"gm.dls", "GM.DLS", "bgm/gm.dls"};
    for (const char *dir : dirs)
    {
        for (const char *name : names)
        {
            char path[320];
            if (dir && JoinPath(path, sizeof(path), dir, name) && StampFile(path, dls))
                return true;
        }
    }
    return false;
}

// --------------------------------------------------------- SC player
bool InitScPlayer()
{
    if (tdls::tdsbOpen(gBlob, gBlobBytes, gScView) != tdls::TDSB_OK)
        return false;
    tdls::SynthConfig config;
    th06midi::ToSynthConfig(gSettings, config);
    const u32 need = tdls::Synth::memoryNeeded(gScView, config);
    if (!gScArena || gScArenaBytes < need)
    {
        free(gScArena);
        gScArena = static_cast<u8 *>(memalign(64, need));
        gScArenaBytes = gScArena ? need : 0;
        if (!gScArena)
            return false;
    }
    new (&gScSynth) tdls::Synth();
    new (&gScSeq) tdls::ZunSequencer();
    new (&gScQueue) tdls::ZunCommandQueue();
    new (&gScPlayer) tdls::ZunMidiPlayer();
    std::memset(&gScUp, 0, sizeof(gScUp));
    tdls::Arena arena;
    arena.init(gScArena, gScArenaBytes);
    return gScSynth.init(gScView, config, arena) && gScPlayer.init(&gScSynth, &gScSeq, &gScQueue) &&
           gScPlayer.setEventQuantization(gSettings.quantMs, 0u);
}

void RenderScInto(s16 *out, u32 frames)
{
    if (th06midi::UpsampleFactor(gSettings) == 2u)
        th06midi::RenderUpsampled2x(gScPlayer, gScUp, out, frames, gScUpTmp);
    else
        gScPlayer.render(out, frames);
}

// --------------------------------------------------------- ME player
void ConstructMeQueue()
{
    sceKernelDcacheWritebackInvalidateRange(gMeQueueRaw, sizeof(gMeQueueRaw));
    void *uncached = reinterpret_cast<void *>(0x40000000u | Phys(gMeQueueRaw));
    gMeQueue = new (uncached) tdls::ZunCommandQueue();
}

struct MeJobOutput
{
    u32 result;
    u32 waitUs;
};

// Runs one MIDI job.  fill() writes the job inputs into the claimed box;
// read() copies outputs before the worker can be claimed again.
template <class Fill, class Read>
bool RunMeJob(u32 timeoutUs, u32 firstSleepUs, Fill fill, Read read, MeJobOutput &out)
{
    out.result = 0;
    out.waitUs = 0;
    volatile Th06MidiMeBox *box = th06_psp_me_midi_begin();
    if (!box)
        return false;
    box->result = TH06_MIDI_RESULT_PENDING;
    fill(box);
    unsigned int wait = 0;
    const int ok = th06_psp_me_midi_finish(timeoutUs, firstSleepUs, &wait);
    out.waitUs = wait;
    if (!ok)
    {
        if (th06_psp_me_poisoned()) PinUnacknowledgedMePayloads();
        return false;
    }
    out.result = box->result;
    read(box);
    return true;
}

struct MeInitInfo
{
    u32 vmeMode, meSp, meWitness, stateBytes, tdsbStatus, result;
};

bool MeReadEnvironment(MeInitInfo &info)
{
    MeJobOutput job;
    const bool ok = RunMeJob(
        200000u, 0u,
        [&](volatile Th06MidiMeBox *box) { box->op = TH06_MIDI_OP_INFO; },
        [&](volatile Th06MidiMeBox *box) {
            info.vmeMode = box->vmeMode;
            info.meSp = box->meSp;
            info.meWitness = box->meWitness;
        },
        job);
    info.result = job.result;
    return ok && job.result == TH06_MIDI_RESULT_OK;
}

bool MeInitPlayer(MeInitInfo &info)
{
    if (th06_psp_me_poisoned() || __atomic_load_n(&gMePinRequested, __ATOMIC_ACQUIRE))
        return false;
    MeJobOutput job;
    const bool ok = RunMeJob(
        200000u, 0u,
        [&](volatile Th06MidiMeBox *box) {
            ConstructMeQueue(); // exclusive mailbox claimed; no old job reader
            box->op = TH06_MIDI_OP_INIT;
            box->queueAddr = static_cast<u32>(reinterpret_cast<uintptr_t>(gMeQueue));
            box->sampleRate = gSettings.synthRate;
            box->voices = gSettings.voices;
            box->reserveVoices = gSettings.reserveVoices;
            box->mapRule = gSettings.rule;
            box->gainAdjust = gSettings.gainAdjust;
            box->masterGainQ16 = gSettings.masterGainQ16;
            box->drumHold = gSettings.drumHold;
            box->quantMs = gSettings.quantMs;
        },
        [&](volatile Th06MidiMeBox *box) {
            info.vmeMode = box->vmeMode;
            info.meSp = box->meSp;
            info.meWitness = box->meWitness;
            info.stateBytes = box->stateBytes;
            info.tdsbStatus = box->tdsbStatus;
        },
        job);
    info.result = job.result;
    if (ok && job.result == TH06_MIDI_RESULT_OK)
    {
        gMeBorrowed.reinitialized();
        gMeLastElapsed = 0;
    }
    return ok && job.result == TH06_MIDI_RESULT_OK;
}

struct MeRenderInfo
{
    u32 elapsedMs, active, running, renderCycles, scrubCycles;
    u32 lost, stolen, scrubPasses, scrubBad, scrubRepaired;
};

// 0 ok, else fallback reason (1 claim, 2 timeout, 3 job error)
u32 MeRender(u32 frames, MeRenderInfo &info, u32 *waitUs)
{
    const u32 timeout = 8000u + frames * 34u;
    MeJobOutput job;
    if (!th06_psp_me_available())
        return 2;
    const bool ran = RunMeJob(
        timeout, 0u,
        [&](volatile Th06MidiMeBox *box) {
            box->op = TH06_MIDI_OP_RENDER;
            box->outPhys = Phys(gMidiMeOut);
            box->frames = frames;
            // 0 once the RAM copy is released: the scrub can then only detect.
            box->blobPhys = gBlob ? Phys(gBlob) : 0u;
        },
        [&](volatile Th06MidiMeBox *box) {
            info.elapsedMs = box->elapsedMs;
            info.active = box->activeVoices;
            info.running = box->seqRunning;
            info.renderCycles = box->renderCycles;
            info.scrubCycles = box->scrubCycles;
            info.lost = box->notesLost;
            info.stolen = box->voicesStolen;
            info.scrubPasses = box->scrubPasses;
            info.scrubBad = box->scrubBad;
            info.scrubRepaired = box->scrubRepaired;
        },
        job);
    *waitUs = job.waitUs;
    if (!ran)
        return job.waitUs ? 2u : (th06_psp_me_available() ? 1u : 2u);
    if (job.result != TH06_MIDI_RESULT_OK)
        return 3;
    if (info.elapsedMs != gMeLastElapsed) gMeBorrowed.applied();
    gMeLastElapsed = info.elapsedMs;
    // The ME wrote this range back; drop any SC line before the SC can read it.
    sceKernelDcacheInvalidateRange(gMidiMeOut, sizeof(gMidiMeOut));
    return 0;
}

bool MeLoadBlob(u32 *badBlocks, u32 *firstBad, u32 *waitUs)
{
    const u32 blocks = gBlobStorage / TH06_MIDI_BLOCK_BYTES;
    for (u32 b = 0; b < blocks; ++b)
        gBlockSums[b] = th06midi::BlockSum(gBlob + b * TH06_MIDI_BLOCK_BYTES);
    sceKernelDcacheWritebackRange(gBlockSums, sizeof(gBlockSums));
    sceKernelDcacheWritebackRange(gBlob, gBlobStorage);
    MeJobOutput job;
    const bool ok = RunMeJob(
        2000000u, 2000u,
        [&](volatile Th06MidiMeBox *box) {
            box->op = TH06_MIDI_OP_LOAD;
            gMeBlobExposed = true;
            box->blobPhys = Phys(gBlob);
            box->blobBytes = gBlobBytes;
            box->sumsPhys = Phys(gBlockSums);
            box->blockCount = blocks;
        },
        [&](volatile Th06MidiMeBox *box) {
            *badBlocks = box->loadBadBlocks;
            *firstBad = box->loadFirstBad;
        },
        job);
    *waitUs = job.waitUs;
    return ok && job.result == TH06_MIDI_RESULT_OK;
}

// ------------------------------------------------------------ self-test
// The same schedule, commands and chunking go to the SC player and to the
// ME player; every chunk must match byte for byte.
class PairSink : public th06midi::ScheduleSink
{
  public:
    PairSink() : frame(0), mismatches(0), firstMismatch(0xffffffffu), meFailed(false), meUs(0) {}
    bool Push(const tdls::ZunCommand &command)
    {
        if (command.type == tdls::ZUN_CMD_PLAY && !gMeBorrowed.canPlay(command.data)) return false;
        tdls::ZunCommand me = command;
        me.data = Uncached(command.data);
        const bool pushed = gScQueue.push(command) && gMeQueue->push(me);
        if (pushed && command.type == tdls::ZUN_CMD_PLAY)
            return gMeBorrowed.play(command.data, th06midi::kSelfTestSmfMaxBytes);
        if (pushed && command.type == tdls::ZUN_CMD_STOP) gMeBorrowed.stop();
        return pushed;
    }
    bool Render(s16 *out, u32 frames)
    {
        RenderScInto(out, frames);
        if (gScPlayer.outputFailed()) return false;
        MeRenderInfo info;
        u32 wait = 0;
        if (MeRender(frames, info, &wait) != 0)
        {
            meFailed = true;
            return false;
        }
        meUs += wait;
        if (std::memcmp(out, gMidiMeOut, frames * 4u) != 0)
        {
            for (u32 i = 0; i < frames * 2u && firstMismatch == 0xffffffffu; ++i)
                if (out[i] != gMidiMeOut[i])
                    firstMismatch = frame + i / 2u;
            ++mismatches;
        }
        frame += frames;
        return true;
    }
    u32 frame;
    u32 mismatches;
    u32 firstMismatch;
    bool meFailed;
    u32 meUs;
};

class ScSink : public th06midi::ScheduleSink
{
  public:
    bool Push(const tdls::ZunCommand &command) { return gScQueue.push(command); }
    bool Render(s16 *out, u32 frames)
    {
        RenderScInto(out, frames);
        return !gScPlayer.outputFailed();
    }
};

// Returns true when the schedule ran; *match is the SC==ME verdict.
bool RunSelfTest(bool withMe, bool *match, char *shaHex8, u32 *frames, u32 *meUs, u32 *firstMismatch)
{
    *match = false;
    *frames = 0;
    *meUs = 0;
    *firstMismatch = 0xffffffffu;
    u8 *smf = static_cast<u8 *>(memalign(64, th06midi::kSelfTestSmfMaxBytes));
    s16 *pcm = static_cast<s16 *>(memalign(64, th06midi::kSelfTestSchedule.chunkFrames * 4u));
    const u32 smfBytes = smf ? th06midi::BuildSelfTestSmf(smf, th06midi::kSelfTestSmfMaxBytes) : 0;
    bool ran = false;
    if (smfBytes && pcm && InitScPlayer())
    {
        sceKernelDcacheWritebackRange(smf, th06midi::kSelfTestSmfMaxBytes);
        tdls::Sha256 hash;
        hash.init();
        const char *error = nullptr;
        if (withMe)
        {
            MeInitInfo info;
            if (MeInitPlayer(info))
            {
                PairSink sink;
                ran = th06midi::RunSchedule(th06midi::kSelfTestSchedule, smf, smfBytes, 44100u, sink, pcm, hash,
                                            frames, &error) &&
                      !sink.meFailed;
                *match = ran && sink.mismatches == 0;
                *meUs = sink.meUs;
                *firstMismatch = sink.firstMismatch;
            }
        }
        else
        {
            ScSink sink;
            ran = th06midi::RunSchedule(th06midi::kSelfTestSchedule, smf, smfBytes, 44100u, sink, pcm, hash, frames,
                                        &error);
            *match = ran;
        }
        u8 digest[32];
        hash.finish(digest);
        Hex8(digest, shaHex8);
    }
    free(pcm);
    if (!(__atomic_load_n(&gMePinRequested, __ATOMIC_ACQUIRE) && gMeBorrowed.contains(smf)))
        free(smf);
    return ran;
}

// --------------------------------------------------------- producer side
void PublishApplied(u32 value)
{
    __atomic_store_n(&gApplied, value, __ATOMIC_RELEASE);
}

void ForwardCommands(int mode)
{
    u32 tail = gRingTail;
    const u32 head = __atomic_load_n(&gRingHead, __ATOMIC_ACQUIRE);
    while (tail != head)
    {
        const GameCommand &g = gRing[tail % kRingSize];
        tdls::ZunCommand c = {g.type, g.arg, g.data, g.size};
        bool pushed;
        if (mode == MODE_ME)
        {
            if (g.type == tdls::ZUN_CMD_PLAY && !gMeBorrowed.canPlay(g.data)) break;
            c.data = Uncached(g.data);
            pushed = gMeQueue->push(c);
            if (pushed && g.type == tdls::ZUN_CMD_PLAY) gMeBorrowed.play(g.data, g.size);
            if (pushed && g.type == tdls::ZUN_CMD_STOP) gMeBorrowed.stop();
        }
        else
        {
            pushed = gScQueue.push(c);
        }
        if (!pushed)
            break;
        ++tail;
    }
    __atomic_store_n(&gRingTail, tail, __ATOMIC_RELEASE);
}

void DisableMidi(u32 reason)
{
    __atomic_store_n(&gMidiFailed, 1u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&gFallbacks, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&gFallbackReason, reason, __ATOMIC_RELAXED);
    // PSP-1000 releases the SC arena after the ME self-test. The static SC
    // objects still contain pointers to its voice/region storage, so even a
    // reset would write into freed game memory. Only reset an owned arena;
    // ME failure/pinning and command discard below remain independent.
    if (gScArena != nullptr)
    {
        gScSeq.stopPlayback();
        gScSynth.powerOn();
    }
    tdls::ZunCommand discarded;
    while (gScQueue.pop(discarded)) {}
    std::memset(&gScUp, 0, sizeof(gScUp));
    gBusy = 0;
    __atomic_store_n(&gRingTail, __atomic_load_n(&gRingHead, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    // A failed/unacknowledged render is never published as applied. The game
    // thread instead collects non-pinned SMFs at the next PlayAudio boundary.
    __atomic_store_n(&gMode, static_cast<int>(MODE_OFF), __ATOMIC_RELEASE);
    ReleaseUnusedMidiMemory(); // immutable ME blob is preserved if pinned
}

void SwitchToSc(u32 reason)
{
    if (th06_psp_me_poisoned())
    {
        DisableMidi(2);
        return;
    }
    // The ME player's state is abandoned.  Restart the intended song on a
    // fresh SC player; commands already given to the ME count as applied.
    __atomic_fetch_add(&gFallbacks, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&gFallbackReason, reason, __ATOMIC_RELAXED);
    if (!gBlob || !InitScPlayer())
    {
        // 32 MB: no RAM copy to synthesize from.  MIDI stops; the next
        // PlayAudio sees no player and uses WAV (platform_hooks.cpp).
        PublishApplied(gRingTail);
        __atomic_store_n(&gMode, static_cast<int>(MODE_OFF), __ATOMIC_RELEASE);
        ReleaseUnusedMidiMemory();
        return;
    }
    u32 seq0, seq1;
    const u8 *data;
    u32 size, playing;
    do
    {
        seq0 = __atomic_load_n(&gIntentSeq, __ATOMIC_ACQUIRE);
        data = gIntentData;
        size = gIntentSize;
        playing = gIntentPlaying;
        seq1 = __atomic_load_n(&gIntentSeq, __ATOMIC_ACQUIRE);
    } while ((seq0 & 1u) != 0 || seq0 != seq1);
    if (playing && data && size)
    {
        tdls::ZunCommand c = {tdls::ZUN_CMD_PLAY, 0u, data, size};
        gScQueue.push(c);
    }
    PublishApplied(gRingTail);
    __atomic_store_n(&gMode, static_cast<int>(MODE_SC), __ATOMIC_RELEASE);
}

bool WantsRender()
{
    if (g_Supervisor.cfg.musicMode == MIDI)
        return true;
    if (__atomic_load_n(&gRingHead, __ATOMIC_ACQUIRE) != gRingTail)
        return true;
    if (__atomic_load_n(&gApplied, __ATOMIC_ACQUIRE) != gRingTail)
        return true;
    return gBusy != 0;
}

// ----------------------------------------------------------- game thread
constexpr u32 kNeverRetire = 0xffffffffu;

void ReleaseRetired()
{
    const u32 applied = __atomic_load_n(&gApplied, __ATOMIC_ACQUIRE);
    const bool disabled = __atomic_load_n(&gMode, __ATOMIC_ACQUIRE) == MODE_OFF &&
                          __atomic_load_n(&gMidiFailed, __ATOMIC_ACQUIRE) != 0;
    // OFF publishes failure/pinning. Observe it before pin so a producer
    // that disables between these captures cannot make us free its read-set.
    const bool pin = __atomic_load_n(&gMePinRequested, __ATOMIC_ACQUIRE) != 0;
    u32 keep = 0;
    for (u32 i = 0; i < gRetiredCount; ++i)
    {
        const bool borrowed = pin && gMeBorrowed.contains(gRetired[i].data);
        if (borrowed) gRetired[i].retireAt = kNeverRetire;
        if (!borrowed && (disabled || (gRetired[i].retireAt != kNeverRetire &&
                                      static_cast<int>(applied - gRetired[i].retireAt) >= 0)))
        {
            ReleaseSong(gRetired[i].data);
            __atomic_fetch_add(&gSongsFreed, 1u, __ATOMIC_RELAXED);
        }
        else
            gRetired[keep++] = gRetired[i];
    }
    gRetiredCount = keep;
    for (u32 i = 0; i < 32; ++i)
    {
        SongSlot &s = gSlots[i];
        if (pin && s.data && gMeBorrowed.contains(s.data)) s.mePinned = true;
        if (disabled && s.data && !s.mePinned)
        {
            ReleaseSong(s.data);
            s.data = nullptr;
            s.size = 0;
            __atomic_fetch_add(&gSongsFreed, 1u, __ATOMIC_RELAXED);
        }
    }
    if (disabled) gSelectedSlot = -1;
    if (disabled && gSongPool)
    {
        bool borrowed = false;
        for (const SongSpan &span : gSongSpans)
            borrowed = borrowed || span.bytes != 0;
        if (!borrowed)
        {
            if (gSongPoolStaticLoan)
            {
                // Even a span-free arena stays owned while a late ME may
                // still hold payloads/the module. A failed return must never
                // turn a static pointer into a libc free or a new loan.
                if (!pin && !th06_psp_me_poisoned() &&
                    th06_psp_me_return_song_storage(gSongPool))
                {
                    gSongPool = nullptr;
                    gSongPoolStaticLoan = false;
                }
            }
            else
            {
                free(gSongPool);
                gSongPool = nullptr;
            }
        }
    }
}

bool PushGame(u32 type, u32 arg, const u8 *data, u32 size)
{
    if (__atomic_load_n(&gMidiFailed, __ATOMIC_ACQUIRE)) return false;
    const u32 head = gRingHead;
    if (head - __atomic_load_n(&gRingTail, __ATOMIC_ACQUIRE) >= kRingSize)
    {
        __atomic_fetch_add(&gQueueDrops, 1u, __ATOMIC_RELAXED);
        th06_psp_boot_note("MIDI COMMAND QUEUE FULL");
        return false;
    }
    gRing[head % kRingSize] = GameCommand{type, arg, data, size};
    __atomic_store_n(&gRingHead, head + 1u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&gSongCommands, 1u, __ATOMIC_RELAXED);
    return true;
}

void SetIntent(const u8 *data, u32 size, u32 playing)
{
    __atomic_add_fetch(&gIntentSeq, 1u, __ATOMIC_ACQ_REL);
    gIntentData = data;
    gIntentSize = size;
    gIntentPlaying = playing;
    __atomic_add_fetch(&gIntentSeq, 1u, __ATOMIC_ACQ_REL);
}

void LogSongPoolState(u32 slot, u32 size)
{
    // Failure boundary only.  Capture ACK counters before diagnostic I/O;
    // the producer may advance while these immutable game-owned spans print.
    const u32 head = __atomic_load_n(&gRingHead, __ATOMIC_ACQUIRE);
    const u32 tail = __atomic_load_n(&gRingTail, __ATOMIC_ACQUIRE);
    const u32 applied = __atomic_load_n(&gApplied, __ATOMIC_ACQUIRE);
    th06_psp_boot_notef("MIDI SONG POOL STATE S%u REQ%uB HEAD%u TAIL%u APPLIED%u RETIRED%u PIN%u MODE%d",
                        U(slot), U(size), U(head), U(tail), U(applied), U(gRetiredCount),
                        U(__atomic_load_n(&gMePinRequested, __ATOMIC_ACQUIRE)),
                        __atomic_load_n(&gMode, __ATOMIC_ACQUIRE));
    for (const SongSpan &span : gSongSpans)
    {
        if (span.bytes == 0) continue;
        const u8 *data = gSongPool + span.offset;
        int owner = -1;
        u32 retireAt = 0;
        bool pinned = false;
        for (u32 i = 0; i < 32; ++i)
            if (gSlots[i].data == data)
            {
                owner = static_cast<int>(i);
                pinned = gSlots[i].mePinned;
            }
        for (u32 i = 0; i < gRetiredCount; ++i)
            if (gRetired[i].data == data)
                retireAt = gRetired[i].retireAt;
        th06_psp_boot_notef("MIDI SONG POOL SPAN OFFSET%u BYTES%u SLOT%d RETIRE%u PIN%u",
                            U(span.offset), U(span.bytes), owner, U(retireAt), U(pinned));
    }
}

bool WaitForSongStopApplied(u32 sequence)
{
    const u32 start = NowUs();
    for (;;)
    {
        if (__atomic_load_n(&gMePinRequested, __ATOMIC_ACQUIRE) || th06_psp_me_poisoned() ||
            __atomic_load_n(&gMode, __ATOMIC_ACQUIRE) == MODE_OFF)
            return false;
        if (static_cast<int>(__atomic_load_n(&gApplied, __ATOMIC_ACQUIRE) - sequence) >= 0)
            return true;
        if (NowUs() - start >= 100000u)
            return false;
        // Original ReadFileData stops synchronously before releasing its old
        // buffer.  Here the producer owns the player, so wait for that STOP's
        // actual ACK at the song-load boundary, without holding either audio
        // mutex or changing the player's sample clock/command contents.
        sceKernelDelayThread(1000);
    }
}

bool SongStorageWritable()
{
    return __atomic_load_n(&gMode, __ATOMIC_ACQUIRE) != MODE_OFF &&
           !__atomic_load_n(&gMidiFailed, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&gMePinRequested, __ATOMIC_ACQUIRE) && !th06_psp_me_poisoned();
}

Pbg3Archive *FindSongArchive(const char *path, i32 &entry, const char *&name)
{
    entry = -1;
    name = path;
    if (!path || !g_Pbg3Archives) return nullptr;
    // Preserve FileSystem::OpenPath's two separator passes and first-match
    // order exactly, rather than changing its backslash-only edge case.
    name = std::strrchr(path, '\\');
    name = name ? name + 1 : path;
    const char *slash = std::strrchr(name, '/');
    name = slash ? slash + 1 : path;
    for (u32 i = 0; i < 0x10u; ++i)
    {
        Pbg3Archive *archive = g_Pbg3Archives[i];
        if (!archive) continue;
        entry = archive->FindEntry(name);
        if (entry >= 0) return archive;
    }
    return nullptr;
}

void LoadSlot(u32 slot, const char *path)
{
    if (slot >= 32 || __atomic_load_n(&gMidiFailed, __ATOMIC_ACQUIRE))
        return;
    // The engine's ReadFileData stops playback before replacing a file.  The
    // old buffer may be read by the player until that STOP is applied; if the
    // STOP could not even be queued the buffer is never freed (a leak is
    // preferable to the player reading freed memory).
    const bool replacingSong = gSlots[slot].data != nullptr;
    const bool stopQueued = PushGame(tdls::ZUN_CMD_STOP, 0, nullptr, 0);
    const u32 stopSequence = gRingHead;
    SetIntent(gIntentData, gIntentSize, 0);
    if (gSlots[slot].data)
    {
        if (gRetiredCount < sizeof(gRetired) / sizeof(gRetired[0]))
            gRetired[gRetiredCount++] = Retired{gSlots[slot].data,
                                               stopQueued && !gSlots[slot].mePinned ? gRingHead : kNeverRetire};
        else
            th06_psp_boot_note("MIDI RETIRE LIST FULL");
        gSlots[slot].data = nullptr;
        gSlots[slot].size = 0;
    }
    i32 entry = -1;
    const char *name = nullptr;
    Pbg3Archive *archive = FindSongArchive(path, entry, name);
    u8 *data = nullptr;
    u32 size = 0;
    if (archive)
    {
        size = archive->GetEntrySize(static_cast<u32>(entry));
        g_LastFileSize = size; // Same entry-size side effect as OpenPath.
        // A direct destination is never written before the actual STOP ACK,
        // including a first load into an empty slot while another song runs.
        if (!stopQueued || !WaitForSongStopApplied(stopSequence))
        {
            th06_psp_boot_notef("MIDI SONG STOP ACK FAILED S%u SEQ%u", U(slot), U(stopSequence));
            LogSongPoolState(slot, size);
            ReleaseRetired();
            return;
        }
        if (!SongStorageWritable()) { ReleaseRetired(); return; }
        ReleaseRetired();
        data = ReserveSongSpan(size, slot);
        if (!data)
        {
            th06_psp_boot_notef("MIDI SONG NG STORAGE %s", name);
            return;
        }
        u32 encodedSize = 0;
        u32 expectedChecksum = 0;
        u8 *encoded = archive->ReadEntryRaw(&encodedSize, &expectedChecksum, entry);
        if (!encoded || !SongStorageWritable())
        {
            free(encoded);
            ReleaseSong(data); // Unpublished span, never in the ME read-set.
            th06_psp_boot_notef("MIDI SONG NG RAW %s", name);
            ReleaseRetired();
            return;
        }
        th06midi::Pbg3SongDecodeResult result{};
        const bool decoded = th06midi::DecodePbg3Song(encoded, encodedSize, data, size, size,
                                                     expectedChecksum, &result);
        free(encoded);
        if (!decoded)
        {
            ReleaseSong(data);
            th06_psp_boot_notef("MIDI SONG NG DECODE %s R%u IN%u OUT%u", name,
                                U(static_cast<u32>(result.status)),
                                U(static_cast<u32>(result.consumedBytes)),
                                U(static_cast<u32>(result.writtenBytes)));
            return;
        }
        if (!SongStorageWritable())
        {
            ReleaseSong(data);
            ReleaseRetired();
            return;
        }
        th06_psp_boot_notef("MIDI SONG DIRECT S%u %s D%u C%u", U(slot), name, U(size), U(encodedSize));
    }
    else
    {
        // Keep the existing external/non-archive lookup path and its safe
        // heap-buffer fallback. Do not invoke the old unbounded decompressor
        // after an archive entry has failed a bounded decode.
        data = path ? FileSystem::OpenPath(path, 0) : nullptr;
        if (!data)
        {
            th06_psp_boot_notef("MIDI SONG NG %s", path ? path : "?");
            return;
        }
        size = g_LastFileSize;
        if (__atomic_load_n(&gMidiFailed, __ATOMIC_ACQUIRE)) { free(data); return; }
        if (replacingSong && stopQueued && !WaitForSongStopApplied(stopSequence))
        {
            th06_psp_boot_notef("MIDI SONG STOP ACK FAILED S%u SEQ%u", U(slot), U(stopSequence));
            LogSongPoolState(slot, size);
        }
        if (!SongStorageWritable())
        {
            free(data);
            ReleaseRetired();
            return;
        }
        ReleaseRetired();
        data = PoolSong(data, size, slot);
    }
    sceKernelDcacheWritebackRange(data, size);
    gSlots[slot].data = data;
    gSlots[slot].size = size;
    gSlots[slot].mePinned = false;
    __atomic_fetch_add(&gSongsLoaded, 1u, __ATOMIC_RELAXED);
}

} // namespace

// ================================================================== C API
extern "C" void th06_midi_boot_prepare(void)
{
    // Before anything else is allocated: the 32 MB PSP-1000 has a ~22 MB heap,
    // the 64 MB models (MEMSIZE=1) about 50 MB.
    {
        void *probe = malloc(24u * 1024u * 1024u);
        gSmallRam = probe == nullptr;
        free(probe);
    }
    LoadSettings();
    th06_psp_boot_notef("MIDI RAM %s", gSmallRam ? "32MB (BLOB IN EDRAM ONLY)" : "64MB");
    th06_psp_boot_notef("MIDI CFG RULE%c V%u R%u G%d M%u HZ%u ME%u DH%u Q%u", th06midi::RuleLetter(gSettings.rule),
                        U(gSettings.voices), U(gSettings.reserveVoices), static_cast<int>(gSettings.gainAdjust),
                        U(gSettings.masterGainQ16), U(gSettings.synthRate), U(gSettings.useMe), U(gSettings.drumHold), U(gSettings.quantMs));
    SourceStamp dls{};
    if (!FindGmDls(dls))
    {
        th06_psp_boot_note("MIDI OFF (NO gm.dls IN th6) -> WAV ONLY");
        return;
    }
    SourceStamp md{};
    if (!StampFile(th06_psp_retail_data_path(TH_MD_DAT_FILE), md))
    {
        th06_psp_boot_note("MIDI OFF (MD.DAT NOT FOUND)");
        return;
    }
    th06_psp_boot_notef("MIDI GM.DLS FOUND %uB", U(dls.bytes));
    char cachePath[320];
    if (!JoinPath(cachePath, sizeof(cachePath), th06_psp_game_dir(), kCacheName))
        return;
    const u32 t0 = NowUs();
    bool ready = TryLoadCache(cachePath, dls, md);
    if (ready)
        th06_psp_boot_notef("MIDI CACHE LOAD %ums", U((NowUs() - t0) / 1000u));
    else
        ready = BuildCache(cachePath, dls, md);
    if (!ready)
        return;
    if (tdls::tdsbOpen(gBlob, gBlobBytes, gScView) != tdls::TDSB_OK)
    {
        th06_psp_boot_note("MIDI OFF (BLOB REJECTED)");
        ReleaseBlob();
        return;
    }
    __atomic_store_n(&gBlobReady, 1, __ATOMIC_RELEASE);
}

extern "C" int th06_midi_blob_ready(void)
{
    return __atomic_load_n(&gBlobReady, __ATOMIC_ACQUIRE);
}

extern "C" void th06_midi_audio_attach(void)
{
    if (__atomic_load_n(&gMidiFailed, __ATOMIC_ACQUIRE) || !th06_midi_blob_ready() ||
        __atomic_load_n(&gMode, __ATOMIC_ACQUIRE) != MODE_OFF)
        return;
    const u32 t0 = NowUs();
    // SC reference first: it is also the PPSSPP path and the fallback.
    bool scMatch = false;
    char scSha[17] = "-";
    u32 frames = 0, meUs = 0, firstMismatch = 0;
    if (!RunSelfTest(false, &scMatch, scSha, &frames, &meUs, &firstMismatch))
    {
        th06_psp_boot_note("MIDI OFF (SC PLAYER INIT FAILED)");
        ReleaseUnusedMidiMemory();
        return;
    }
    th06_psp_boot_notef("MIDI SC SELFTEST F%u SHA %s %ums", U(frames), scSha, U((NowUs() - t0) / 1000u));

    int mode = MODE_SC;
    const bool meWanted = gSettings.useMe && th06_psp_me_available() && gBlobStorage <= TH06_MIDI_EDRAM_BLOB_CAPACITY;
    if (meWanted)
    {
        u32 bad = 0, firstBad = 0, loadUs = 0;
        MeInitInfo info{};
        // INFO performs no eDRAM writes.  Refuse every mode except 2 before
        // loading any tone data; LOAD and INIT independently repeat the guard.
        if (!MeReadEnvironment(info))
        {
            th06_psp_boot_notef("MIDI ME INFO NG R%u -> SC", U(info.result));
        }
        else if (info.vmeMode != 2u)
        {
            th06_psp_boot_notef("MIDI ME OFF (VME MODE %u, NEED 2, BEFORE LOAD) -> SC", U(info.vmeMode));
        }
        else if ((info.meSp & 0x003fffffu) < TH06_MIDI_EDRAM_STACK_FLOOR)
        {
            th06_psp_boot_notef("MIDI ME OFF (STACK %08X BELOW %08X, BEFORE LOAD) -> SC", U(info.meSp),
                                U(TH06_MIDI_EDRAM_STACK_FLOOR));
        }
        else if (!MeLoadBlob(&bad, &firstBad, &loadUs))
        {
            th06_psp_boot_notef("MIDI ME LOAD NG BAD%u FIRST%u %uus -> SC", U(bad), U(firstBad), U(loadUs));
        }
        else if (!MeInitPlayer(info))
        {
            th06_psp_boot_notef("MIDI ME INIT NG R%u T%u ST%u -> SC", U(info.result), U(info.tdsbStatus),
                                U(info.stateBytes));
        }
        else
        {
            const u32 spOffset = info.meSp & 0x003fffffu;
            th06_psp_boot_notef("MIDI ME LOAD %uus BLOCKS %u VME%u SP%08X W%08X ST%uB", U(loadUs),
                                U(gBlobStorage / TH06_MIDI_BLOCK_BYTES), U(info.vmeMode), U(info.meSp),
                                U(info.meWitness), U(info.stateBytes));
            if (info.vmeMode != 2u)
            {
                th06_psp_boot_notef("MIDI ME OFF (VME MODE %u, NEED 2) -> SC", U(info.vmeMode));
            }
            else if (spOffset < TH06_MIDI_EDRAM_STACK_FLOOR)
            {
                th06_psp_boot_notef("MIDI ME OFF (STACK %08X BELOW %08X) -> SC", U(info.meSp),
                                    U(TH06_MIDI_EDRAM_STACK_FLOOR));
            }
            else
            {
                bool match = false;
                char sha[17] = "-";
                const bool ran = RunSelfTest(true, &match, sha, &frames, &meUs, &firstMismatch);
                if (ran && match)
                {
                    th06_psp_boot_notef("MIDI SELFTEST SC=ME OK F%u SHA %s ME %uus", U(frames), sha, U(meUs));
                    mode = MODE_ME;
                }
                else
                {
                    th06_psp_boot_notef("MIDI SELFTEST SC!=ME %s AT F%u -> SC", ran ? "MISMATCH" : "ME FAIL",
                                        U(firstMismatch));
                }
            }
        }
    }
    else if (gSettings.useMe)
    {
        th06_psp_boot_note("MIDI ME UNAVAILABLE -> SC");
    }

    if (th06_psp_me_poisoned())
    {
        DisableMidi(2);
        th06_psp_boot_notef("MIDI BOOT TIMEOUT PIN SMF%u BLOB%u", U(gMeBorrowed.bytes()), U(gMeBlobPinned ? gBlobStorage : 0u));
        th06_psp_require_power_off("MIDI BOOT ME NOT ACKNOWLEDGED");
    }
    // Fresh players for the game.
    if (mode == MODE_ME)
    {
        MeInitInfo info{};
        if (!MeInitPlayer(info))
        {
            th06_psp_boot_note("MIDI ME REINIT NG -> SC");
            mode = MODE_SC;
        }
    }
    if (th06_psp_me_poisoned())
    {
        DisableMidi(2);
        th06_psp_boot_notef("MIDI BOOT REINIT TIMEOUT PIN SMF%u BLOB%u", U(gMeBorrowed.bytes()),
                            U(gMeBlobPinned ? gBlobStorage : 0u));
        th06_psp_require_power_off("MIDI BOOT REINIT NOT ACKNOWLEDGED");
    }
#if defined(TH06_PSP_MIDI_MEMSIM)
    // Test EBOOT only: reproduce the PSP-1000 ME-mode main-RAM footprint in
    // PPSSPP (no ME there): blob and SC arena released, songs still loaded.
    gSmallRam = true;
    mode = MODE_MEMSIM;
    th06_psp_boot_note("MIDI MEMSIM (TEST): ME-MODE RAM FOOTPRINT, NO SOUND");
#endif
    if (gSmallRam && mode != MODE_SC)
    {
        free(gScArena);
        gScArena = nullptr;
        gScArenaBytes = 0;
        ReleaseBlob();
        th06_psp_boot_note("MIDI 32MB: RAM BLOB RELEASED (EDRAM ONLY, NO SC FALLBACK)");
    }
    else if (gSmallRam)
    {
        ReleaseUnusedMidiMemory();
        th06_psp_boot_note("MIDI OFF (32MB WITHOUT ME) -> WAV");
        return;
    }
    else if (!InitScPlayer())
    {
        th06_psp_boot_note("MIDI OFF (SC PLAYER INIT FAILED)");
        ReleaseUnusedMidiMemory();
        return;
    }
    gRingHead = gRingTail = 0;
    PublishApplied(0);
    __atomic_store_n(&gMode, mode, __ATOMIC_RELEASE);
    th06_psp_boot_notef("MIDI READY %s V%u %ums", mode == MODE_ME ? "ME" : (mode == MODE_SC ? "SC" : "MEMSIM"),
                        U(gSettings.voices), U((NowUs() - t0) / 1000u));
}

extern "C" int th06_midi_playable(void)
{
    // Caller is the game thread (Supervisor::PlayAudio / MidiOutput wraps).
    if (__atomic_load_n(&gMode, __ATOMIC_ACQUIRE) == MODE_OFF &&
        __atomic_load_n(&gMidiFailed, __ATOMIC_ACQUIRE)) ReleaseRetired();
    return __atomic_load_n(&gMode, __ATOMIC_ACQUIRE) != MODE_OFF;
}

extern "C" int th06_midi_prepare_song_storage(void *retiredWavPcm)
{
    // Game thread only; a supplied WAV block must already be retired with
    // audio producer/file-reader access stopped under the caller's locks.
    // Do not drain commands, retire songs, or alter player/ACK state here.
    if (__atomic_load_n(&gMode, __ATOMIC_ACQUIRE) == MODE_OFF ||
        __atomic_load_n(&gMidiFailed, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&gMePinRequested, __ATOMIC_ACQUIRE) || th06_psp_me_poisoned())
        return 0;
    if (gSongPool != nullptr)
        return retiredWavPcm == nullptr ? 1 : 0;

    // Runtime ME vertex packing is disabled. Its existing static budget is
    // available only after the backend has finished its boot self-test and
    // atomically excluded every init/vertex writer. No heap allocation is
    // needed in the normal configuration; span/ACK/pin rules stay unchanged.
    if (void *loan = th06_psp_me_borrow_song_storage(kSongPoolBytes))
    {
        gSongPool = static_cast<u8 *>(loan);
        gSongPoolStaticLoan = true;
        th06_psp_boot_notef("MIDI SONG ARENA %uB (VTX LOAN)", U(kSongPoolBytes));
        // A supplied WAV buffer was NOT adopted: the caller still owns and
        // must release it, even though song storage is now ready.
        return retiredWavPcm == nullptr ? 1 : 0;
    }

    if (retiredWavPcm == nullptr)
    {
        u8 *storage = static_cast<u8 *>(memalign(64, kSongPoolBytes));
        if (storage == nullptr) return 0;
        gSongPool = storage;
        gSongPoolStaticLoan = false;
        th06_psp_boot_notef("MIDI SONG ARENA %uB (EARLY)", U(kSongPoolBytes));
        return 1;
    }

    // Only this SDK's audited _realloc_r shrink path may adopt a WAV cache:
    // old chunk >= new chunk branches directly to split/free of the tail,
    // returning the original payload address (and hence its 64-byte ME
    // alignment). This is not a portable realloc alignment guarantee.
#if __NEWLIB__ == 4 && __NEWLIB_MINOR__ == 5 && __NEWLIB_PATCHLEVEL__ == 0
    static_assert(sizeof(void *) == 4 && sizeof(size_t) == 4,
                  "WAV cache adoption requires the audited 32-bit PSP newlib ABI");
    constexpr size_t kRetiredWavCacheBytes = 384u * 1024u;
    if ((reinterpret_cast<uintptr_t>(retiredWavPcm) & 63u) != 0 ||
        malloc_usable_size(retiredWavPcm) < kRetiredWavCacheBytes)
        return 0;
    u8 *storage = static_cast<u8 *>(realloc(retiredWavPcm, kSongPoolBytes));
    if (storage == nullptr) return 0; // Failure leaves the caller's block owned.
    // Success has transferred ownership. Never return 0 after realloc: even
    // an allocator regression that moved it must not make the caller free
    // the old, now-invalid pointer. The audited shrink cannot move it.
    gSongPool = storage;
    gSongPoolStaticLoan = false;
    th06_psp_boot_notef("MIDI SONG ARENA %uB (WAV REUSE)", U(kSongPoolBytes));
    return 1;
#else
    return 0;
#endif
}

extern "C" const short *th06_midi_render_for_mix(unsigned int frames)
{
    const int mode = __atomic_load_n(&gMode, __ATOMIC_ACQUIRE);
    if (mode == MODE_OFF || frames == 0 || frames > TH06_MIDI_MAX_RENDER_FRAMES || !WantsRender())
        return nullptr;
    if (mode == MODE_MEMSIM)
    {
        // Test EBOOT only: commands are consumed as if a player applied them.
        __atomic_store_n(&gRingTail, __atomic_load_n(&gRingHead, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        PublishApplied(gRingTail);
        return nullptr;
    }
    const u32 forwardedBefore = gRingTail;
    ForwardCommands(mode);
    const u32 forwarded = gRingTail;
    if (mode == MODE_ME)
    {
        MeRenderInfo info{};
        u32 waitUs = 0;
        const u32 failure = MeRender(frames, info, &waitUs);
        if (failure == 0)
        {
            static u32 lastMs;
            if (info.elapsedMs != lastMs)
                PublishApplied(forwarded);
            lastMs = info.elapsedMs;
            gBusy = info.running || info.active;
            __atomic_fetch_add(&gDiagMeJobs, 1u, __ATOMIC_RELAXED);
            __atomic_fetch_add(&gDiagFrames, frames, __ATOMIC_RELAXED);
            __atomic_fetch_add(&gDiagWaitTotal, waitUs, __ATOMIC_RELAXED);
            RecordMax(&gDiagWaitMax, waitUs);
            RecordMax(&gDiagRenderCyclesMax, info.renderCycles);
            RecordMax(&gDiagScrubCyclesMax, info.scrubCycles);
            RecordMax(&gDiagActiveMax, info.active);
            gStatNotesLost = info.lost;
            gStatStolen = info.stolen;
            gStatScrubPasses = info.scrubPasses;
            gStatScrubBad = info.scrubBad;
            gStatScrubRepaired = info.scrubRepaired;
            // A few repaired blocks are tolerable (each is rewritten from the
            // SC copy); a growing count means eDRAM is not being refreshed.
            if (info.scrubBad <= (gBlob ? kScrubBadLimit : 0u))
                return gMidiMeOut;
            SwitchToSc(4);
            if (__atomic_load_n(&gMode, __ATOMIC_ACQUIRE) != MODE_SC)
                return nullptr;
            ForwardCommands(MODE_SC);
        }
        else
        {
            (void)forwardedBefore;
            SwitchToSc(failure);
            if (__atomic_load_n(&gMode, __ATOMIC_ACQUIRE) != MODE_SC)
                return nullptr;
            ForwardCommands(MODE_SC);
        }
    }
    const u32 startUs = NowUs();
    const tdls::u64 msBefore = gScPlayer.elapsedMs();
    const u32 forwardedSc = gRingTail;
    RenderScInto(gMidiScOut, frames);
    if (gScPlayer.outputFailed())
    {
        DisableMidi(5);
        return nullptr;
    }
    if (gScPlayer.elapsedMs() != msBefore)
        PublishApplied(forwardedSc);
    gBusy = gScSeq.timerRunning() || gScSynth.activeVoices() != 0;
    const u32 us = NowUs() - startUs;
    __atomic_fetch_add(&gDiagScJobs, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&gDiagFrames, frames, __ATOMIC_RELAXED);
    __atomic_fetch_add(&gDiagWaitTotal, us, __ATOMIC_RELAXED);
    RecordMax(&gDiagWaitMax, us);
    RecordMax(&gDiagActiveMax, gScSynth.activeVoices());
    gStatNotesLost = gScSynth.stats().notesLost;
    gStatStolen = gScSynth.stats().voicesStolen;
    return gMidiScOut;
}

extern "C" int th06_midi_hooks_active(void)
{
    return th06_midi_playable();
}

extern "C" void th06_midi_hook_read_file(unsigned int slot, const char *path)
{
    ReleaseRetired();
    LoadSlot(slot, path);
}

extern "C" void th06_midi_hook_load_file(const char *path)
{
    ReleaseRetired();
    LoadSlot(0x1f, path);
    gSelectedSlot = 0x1f;
}

extern "C" void th06_midi_hook_parse(int slot)
{
    ReleaseRetired();
    gSelectedSlot = slot >= 0 && slot < 32 && gSlots[slot].data ? slot : -1;
}

extern "C" void th06_midi_hook_play(void)
{
    ReleaseRetired();
    if (gSelectedSlot < 0 || !gSlots[gSelectedSlot].data)
        return;
    const SongSlot &s = gSlots[gSelectedSlot];
    PushGame(tdls::ZUN_CMD_PLAY, 0, s.data, s.size);
    SetIntent(s.data, s.size, 1);
}

extern "C" void th06_midi_hook_stop(void)
{
    ReleaseRetired();
    PushGame(tdls::ZUN_CMD_STOP, 0, nullptr, 0);
    SetIntent(gIntentData, gIntentSize, 0);
}

extern "C" void th06_midi_hook_fade(unsigned int ms)
{
    ReleaseRetired();
    PushGame(tdls::ZUN_CMD_FADE, ms, nullptr, 0);
}

extern "C" void th06_midi_diag_window(Th06MidiDiag *out)
{
    if (!out)
        return;
    std::memset(out, 0, sizeof(*out));
    out->mode = static_cast<unsigned int>(__atomic_load_n(&gMode, __ATOMIC_ACQUIRE));
    out->meJobs = __atomic_exchange_n(&gDiagMeJobs, 0u, __ATOMIC_ACQ_REL);
    out->scJobs = __atomic_exchange_n(&gDiagScJobs, 0u, __ATOMIC_ACQ_REL);
    out->frames = __atomic_exchange_n(&gDiagFrames, 0u, __ATOMIC_ACQ_REL);
    out->waitTotalUs = __atomic_exchange_n(&gDiagWaitTotal, 0u, __ATOMIC_ACQ_REL);
    out->waitMaxUs = __atomic_exchange_n(&gDiagWaitMax, 0u, __ATOMIC_ACQ_REL);
    out->renderCyclesMax = __atomic_exchange_n(&gDiagRenderCyclesMax, 0u, __ATOMIC_ACQ_REL);
    out->scrubCyclesMax = __atomic_exchange_n(&gDiagScrubCyclesMax, 0u, __ATOMIC_ACQ_REL);
    out->activeMax = __atomic_exchange_n(&gDiagActiveMax, 0u, __ATOMIC_ACQ_REL);
    // audio time of `frames` at 44.1 kHz = frames * 1e6 / 44100 us
    const unsigned long long audioUs = static_cast<unsigned long long>(out->frames) * 1000000ull / 44100ull;
    out->loadPermille = audioUs ? static_cast<unsigned int>(static_cast<unsigned long long>(out->waitTotalUs) *
                                                            1000ull / audioUs)
                                : 0u;
    out->notesLost = gStatNotesLost;
    out->voicesStolen = gStatStolen;
    out->scrubPasses = gStatScrubPasses;
    out->scrubBad = gStatScrubBad;
    out->scrubRepaired = gStatScrubRepaired;
    out->queueDrops = __atomic_load_n(&gQueueDrops, __ATOMIC_RELAXED);
    out->fallbacks = __atomic_load_n(&gFallbacks, __ATOMIC_RELAXED);
    out->fallbackReason = __atomic_load_n(&gFallbackReason, __ATOMIC_RELAXED);
    out->songsLoaded = __atomic_load_n(&gSongsLoaded, __ATOMIC_RELAXED);
    out->songsFreed = __atomic_load_n(&gSongsFreed, __ATOMIC_RELAXED);
    out->commands = __atomic_load_n(&gSongCommands, __ATOMIC_RELAXED);
    if (__atomic_load_n(&gMePinRequested, __ATOMIC_ACQUIRE))
    {
        out->pinnedSmfBytes = gMeBorrowed.bytes();
        out->pinnedBlobBytes = gMeBlobPinned ? gBlobStorage : 0u;
    }
}

#if defined(TH06_PSP_MIDI_PCMTEST)
// Test EBOOT only (make MIDI_PCMTEST=1, never shipped): render the fixed PCM
// scripts on the SC and log their hashes; tools/midi/run_pc_checks.sh prints
// the dls_render_tool hashes they must equal.
namespace
{
struct PcmTestSongs
{
    Pbg3Archive *archive;
    const char *names[4];
    u8 *data[4];
    u32 size[4];
    u32 count;
};

bool PcmTestFindSong(void *user, const char *name, const u8 **data, u32 *size)
{
    PcmTestSongs *songs = static_cast<PcmTestSongs *>(user);
    for (u32 i = 0; i < songs->count; ++i)
        if (std::strcmp(songs->names[i], name) == 0)
        {
            *data = songs->data[i];
            *size = songs->size[i];
            return true;
        }
    if (songs->count >= 4)
        return false;
    const i32 entry = songs->archive->FindEntry(name);
    if (entry < 0)
        return false;
    u8 *bytes = songs->archive->ReadDecompressEntry(static_cast<u32>(entry), name);
    if (!bytes)
        return false;
    songs->names[songs->count] = name;
    songs->data[songs->count] = bytes;
    songs->size[songs->count] = songs->archive->GetEntrySize(static_cast<u32>(entry));
    *data = bytes;
    *size = songs->size[songs->count];
    ++songs->count;
    return true;
}
} // namespace

extern "C" void th06_midi_pcm_test(void)
{
    th06_psp_boot_note("MIDI PCMTEST BEGIN");
    if (!th06_midi_blob_ready())
    {
        th06_psp_boot_note("MIDI PCMTEST NO BLOB");
        return;
    }
    PcmTestSongs songs{};
    songs.archive = new (std::nothrow) Pbg3Archive();
    if (!songs.archive || !songs.archive->Load(TH_MD_DAT_FILE))
    {
        th06_psp_boot_note("MIDI PCMTEST NO MD.DAT");
        return;
    }
    th06midi::ScriptHost host{};
    host.blob = gBlob;
    host.blobBytes = gBlobBytes;
    host.findSong = PcmTestFindSong;
    host.user = &songs;
    const u32 rates[2] = {44100u, 22050u};
    for (u32 r = 0; r < 2; ++r)
    {
        th06midi::Settings settings = gSettings;
        settings.synthRate = rates[r];
        th06_psp_boot_notef("MIDI PCMTEST SETTINGS RULE%c V%u R%u G%d M%u HZ%u", th06midi::RuleLetter(settings.rule),
                            U(settings.voices), U(settings.reserveVoices), static_cast<int>(settings.gainAdjust),
                            U(settings.masterGainQ16), U(settings.synthRate));
        for (u32 i = 0; i < th06midi::kScriptCount; ++i)
        {
            const u32 t0 = NowUs();
            th06midi::ScriptResult result;
            char hex[65] = "-";
            const bool ok = th06midi::RunScript(th06midi::kScripts[i], host, settings, result);
            if (ok)
                tdls::sha256Hex(result.pcmSha256, hex);
            th06_psp_boot_notef("MIDI PCMTEST %s@%u %s F%u %s %ums", th06midi::kScripts[i].name, U(rates[r]),
                                ok ? "OK" : (result.error ? result.error : "NG"), U(result.frames), hex,
                                U((NowUs() - t0) / 1000u));
        }
        th06midi::ScriptResult self;
        char hex[65] = "-";
        if (th06midi::RunSelfTestScript(host, settings, self))
            tdls::sha256Hex(self.pcmSha256, hex);
        th06_psp_boot_notef("MIDI PCMTEST selftest@%u F%u %s", U(rates[r]), U(self.frames), hex);
    }
    for (u32 i = 0; i < songs.count; ++i)
        free(songs.data[i]);
    delete songs.archive;
    th06_psp_boot_note("MIDI PCMTEST END");
}
#endif
