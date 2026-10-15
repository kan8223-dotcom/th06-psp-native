// TH06 PSP: build the gm.dls subset blob (TDSB v1) from the user's gm.dls and
// the game's MIDI files, exactly as the PC tool dls_subset_tool does it, so the
// usage list, the subset list (manifest) and the blob hash the same on PC and
// PSP.  Platform independent: compiled into the EBOOT and into the PC check
// harness (tools/midi/midi_pc_check.cpp).  It allocates only through the
// caller's allocator and never touches files.
#pragma once

#include "tdls/subset.hpp"
#include "tdls/types.hpp"

namespace th06midi
{

using tdls::u32;
using tdls::u8;

// Increment when anything below changes what ends up in the cache.
constexpr u32 kBuilderVersion = 2;

// The blob variants the PC tool lists in every subset list, in its order.
constexpr u32 kVariantCount = 5;
constexpr u32 kVariantFlags[kVariantCount] = {0u, tdls::BLOB_PCM8, tdls::BLOB_TRIM_LOOP,
                                               tdls::BLOB_PCM8 | tdls::BLOB_TRIM_LOOP, tdls::BLOB_BIG_ENDIAN};

struct BuildFile
{
    const char *name; // base name as listed (e.g. "th06_01.mid")
    const u8 *data;
    u32 size;
};

struct BuildAllocator
{
    void *(*alloc)(u32 bytes, u32 align, void *user); // zero-filled not required
    void (*release)(void *memory, void *user);
    void *user;
};

struct BuildResult
{
    // The kept blob: variant option flags 0 (16-bit little endian) with the
    // writer's mandatory BLOB_MSGS_GUARD bit, 64-byte aligned,
    // its storage rounded up to whole 2 KiB blocks with zero bytes.  Owned by
    // the caller after a successful build (free with the allocator).
    u8 *blob;
    u32 blobBytes;
    u32 blobStorageBytes;
    u8 blobSha256[32];
    char usageSha[65];
    char manifestSha[65];
    u32 tones;
    u32 instruments, regions, waves;
    u32 uncoveredNotes, unmappedTones;
    u32 notes;
    u32 variantBytes[kVariantCount];
    u8 variantSha256[kVariantCount][32];
    u32 dlsArenaPeak;
    u32 planArenaPeak;
    const char *error; // static string on failure
};

typedef void (*BuildProgress)(const char *stage, void *user);

// files must be sorted by name (the PC tool sorts by base name) and are scanned
// in SMF_MODE_GAME with no init.mid (TH06).  work is the usage-list name.
bool BuildBlob(const u8 *dls, u32 dlsBytes, const u8 dlsSha256[32], const BuildFile *files, u32 fileCount,
               const char *work, tdls::MapRule rule, const BuildAllocator &allocator, BuildResult &out,
               BuildProgress progress, void *progressUser);

// FNV-1a over little-endian 32-bit words of each 2 KiB block (zero padded),
// the per-block checksum the ME uses to verify its eDRAM copy.
u32 BlockSum(const u8 *block);

} // namespace th06midi
