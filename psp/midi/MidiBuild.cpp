#include "MidiBuild.hpp"

#include <string.h>

#include "tdls/byte_source.hpp"
#include "tdls/dls.hpp"
#include "tdls/manifest.hpp"
#include "tdls/sha256.hpp"
#include "tdls/smf_scan.hpp"

namespace th06midi
{
namespace
{

using tdls::Arena;
using tdls::DlsFile;
using tdls::ManifestWriter;
using tdls::UsageSet;

// Generous fixed sizes; the peaks are reported (TH06 needs far less).
constexpr u32 kDlsArenaBytes = 1024u * 1024u;
constexpr u32 kPlanArenaBytes = 256u * 1024u;
constexpr u32 kToneCapacity = 512u;
constexpr u32 kBlockBytes = 2048u;

struct Owned
{
    const BuildAllocator *allocator;
    void *pointers[8];
    u32 count;

    void *take(u32 bytes, u32 align)
    {
        if (count >= sizeof(pointers) / sizeof(pointers[0]))
            return 0;
        void *memory = allocator->alloc(bytes, align, allocator->user);
        if (memory)
            pointers[count++] = memory;
        return memory;
    }
    void keep(void *memory)
    {
        for (u32 i = 0; i < count; ++i)
        {
            if (pointers[i] == memory)
            {
                pointers[i] = pointers[count - 1];
                --count;
                return;
            }
        }
    }
    ~Owned()
    {
        for (u32 i = 0; i < count; ++i)
            allocator->release(pointers[i], allocator->user);
    }
};

void Progress(BuildProgress progress, void *user, const char *stage)
{
    if (progress)
        progress(stage, user);
}

bool Fail(BuildResult &out, const char *why)
{
    out.error = why;
    return false;
}

} // namespace

u32 BlockSum(const u8 *block)
{
    u32 hash = 2166136261u;
    for (u32 offset = 0; offset < kBlockBytes; offset += 4)
    {
        const u32 word = tdls::rd32le(block + offset);
        hash = (hash ^ word) * 16777619u;
    }
    return hash;
}

bool BuildBlob(const u8 *dls, u32 dlsBytes, const u8 dlsSha256[32], const BuildFile *files, u32 fileCount,
               const char *work, tdls::MapRule rule, const BuildAllocator &allocator, BuildResult &out,
               BuildProgress progress, void *progressUser)
{
    memset(&out, 0, sizeof(out));
    Owned owned;
    owned.allocator = &allocator;
    owned.count = 0;
    if (!dls || dlsBytes == 0 || !files || fileCount == 0 || fileCount > 32 || !work)
        return Fail(out, "bad arguments");

    // ---- gm.dls
    Progress(progress, progressUser, "DLS PARSE");
    u8 *dlsArenaMemory = static_cast<u8 *>(owned.take(kDlsArenaBytes, 8));
    if (!dlsArenaMemory)
        return Fail(out, "no memory (dls arena)");
    Arena dlsArena;
    dlsArena.init(dlsArenaMemory, kDlsArenaBytes);
    tdls::MemorySource source(dls, dlsBytes);
    DlsFile dlsFile;
    if (tdls::dlsParse(source, dlsArena, dlsFile) != tdls::DLS_OK)
        return Fail(out, "gm.dls is not a readable DLS level 1 file");
    out.dlsArenaPeak = dlsArena.peak;

    // ---- usage list (same steps as dls_subset_tool scanWork, GAME mode)
    Progress(progress, progressUser, "MIDI SCAN");
    tdls::ToneUse *toneStorage =
        static_cast<tdls::ToneUse *>(owned.take(kToneCapacity * (u32)sizeof(tdls::ToneUse), 8));
    tdls::ToneUse *oneStorage =
        static_cast<tdls::ToneUse *>(owned.take(kToneCapacity * (u32)sizeof(tdls::ToneUse), 8));
    if (!toneStorage || !oneStorage)
        return Fail(out, "no memory (usage)");
    UsageSet usage;
    usage.init(toneStorage, kToneCapacity);
    tdls::InputFile inputs[32];
    u32 maxPolyphony = 0;
    for (u32 i = 0; i < fileCount; ++i)
    {
        if (!files[i].data || files[i].size == 0)
            return Fail(out, "empty MIDI file");
        tdls::SmfChannelState state[16];
        tdls::smfResetChannels(state);
        const tdls::SmfScanOptions options = {tdls::SMF_MODE_GAME, i < 32u ? i : 32u};
        UsageSet one;
        one.init(oneStorage, kToneCapacity);
        tdls::SmfChannelState stateOne[16];
        memcpy(stateOne, state, sizeof(state));
        tdls::SmfInfo info;
        const tdls::SmfStatus s1 =
            tdls::smfScan(files[i].data, files[i].size, stateOne, one, options, 0, &info);
        const tdls::SmfStatus s0 = tdls::smfScan(files[i].data, files[i].size, state, usage, options, 0, 0);
        if (s0 != tdls::SMF_OK || s1 != tdls::SMF_OK)
            return Fail(out, "MIDI file scan failed");
        if (one.stats.maxPolyphony > maxPolyphony)
            maxPolyphony = one.stats.maxPolyphony;
        inputs[i].name = files[i].name;
        inputs[i].size = files[i].size;
        tdls::sha256Buffer(files[i].data, files[i].size, inputs[i].sha256);
    }
    // dls_subset_tool: the work's max polyphony is the per-file maximum.
    usage.stats.maxPolyphony = maxPolyphony;
    if (usage.overflow)
        return Fail(out, "too many tones");
    {
        ManifestWriter writer;
        writer.begin(0);
        tdls::writeUsageList(writer, work, tdls::SMF_MODE_GAME, 0, inputs, fileCount, usage);
        writer.finish(out.usageSha);
        if (writer.overflowed())
            return Fail(out, "usage list line overflow");
    }
    out.tones = usage.count;
    out.notes = usage.stats.notes;

    // ---- subset plan
    Progress(progress, progressUser, "SUBSET PLAN");
    u8 *planArenaMemory = static_cast<u8 *>(owned.take(kPlanArenaBytes, 8));
    if (!planArenaMemory)
        return Fail(out, "no memory (plan arena)");
    Arena planArena;
    planArena.init(planArenaMemory, kPlanArenaBytes);
    tdls::SubsetPlan plan;
    if (!tdls::subsetPlan(dlsFile, usage, rule, planArena, plan))
        return Fail(out, "subset plan failed");
    out.planArenaPeak = planArena.peak;
    out.instruments = plan.instruments;
    out.regions = plan.regions;
    out.waves = plan.waves;
    out.uncoveredNotes = plan.uncoveredNotes;
    out.unmappedTones = plan.unmappedTones;

    // ---- blobs: every variant is written and hashed (the subset list names
    // all five), only option flags 0 is kept. The writer adds BLOB_MSGS_GUARD
    // to every actual blob header; variant flags are format options, as in
    // dls_subset_tool's manifest.
    u32 scratchBytes = 0;
    for (u32 v = 0; v < kVariantCount; ++v)
    {
        out.variantBytes[v] = tdls::subsetBlobSize(dlsFile, usage, plan, kVariantFlags[v]);
        if (out.variantBytes[v] == 0)
            return Fail(out, "empty blob");
        if (v != 0 && out.variantBytes[v] > scratchBytes)
            scratchBytes = out.variantBytes[v];
    }
    const u32 keptBytes = out.variantBytes[0];
    const u32 keptStorage = (keptBytes + kBlockBytes - 1u) / kBlockBytes * kBlockBytes;
    u8 *kept = static_cast<u8 *>(owned.take(keptStorage, 64));
    u8 *scratch = static_cast<u8 *>(owned.take(scratchBytes, 64));
    if (!kept || !scratch)
        return Fail(out, "no memory (blob)");
    tdls::BlobInfo infos[kVariantCount];
    for (u32 v = 0; v < kVariantCount; ++v)
    {
        Progress(progress, progressUser, v == 0 ? "BLOB WRITE" : "BLOB VARIANT");
        u8 *target = v == 0 ? kept : scratch;
        u32 written = 0;
        if (!tdls::subsetWriteBlob(source, dlsFile, usage, plan, kVariantFlags[v], dlsSha256, target,
                                   out.variantBytes[v], &written) ||
            written != out.variantBytes[v])
            return Fail(out, "blob write failed");
        tdls::sha256Buffer(target, written, out.variantSha256[v]);
        infos[v].flags = kVariantFlags[v];
        infos[v].size = written;
        memcpy(infos[v].sha256, out.variantSha256[v], 32);
    }
    memset(kept + keptBytes, 0, keptStorage - keptBytes);
    {
        ManifestWriter writer;
        writer.begin(0);
        tdls::writeSubsetManifest(writer, dlsFile, dlsSha256, out.usageSha, usage, plan, infos, kVariantCount);
        writer.finish(out.manifestSha);
        if (writer.overflowed())
            return Fail(out, "subset list line overflow");
    }
    owned.keep(kept);
    out.blob = kept;
    out.blobBytes = keptBytes;
    out.blobStorageBytes = keptStorage;
    memcpy(out.blobSha256, out.variantSha256[0], 32);
    Progress(progress, progressUser, "BUILD DONE");
    return true;
}

} // namespace th06midi
