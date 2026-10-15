// PC check harness for psp/midi (never shipped).  It compiles the same
// psp/midi/MidiBuild.cpp and psp/midi/MidiScript.cpp that go into the EBOOT, so
// the PC can prove the hashes the PSP build must reproduce:
//
//   midi_pc_check build    --dls <gm.dls> --midi <dir> [--rule A|B|C]
//   midi_pc_check script   --blob <x.tdsb> --midi <dir> [settings]
//   midi_pc_check selftest --blob <x.tdsb> [settings]
//   midi_pc_check budget   --blob <x.tdsb> --psp-layout O:A:ART:W:R:I:M:V [settings]
//   settings: --rule A|B|C --voices N --reserve R --gain-adjust G --master-q16 Q --rate HZ
//             (defaults = th06midi::DefaultSettings, the game's defaults)
//
// The caller supplies private input/output paths (gm.dls derivatives are not distributed).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "MidiBuild.hpp"
#include "MidiScript.hpp"
#include "MidiSettings.hpp"
#include "midi_me_protocol.h"
#include "tdls/blob.hpp"
#include "tdls/sha256.hpp"

namespace
{

bool LoadFile(const std::string &path, std::vector<tdls::u8> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    const size_t got = n > 0 ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    return got == out.size();
}

std::string Hex(const tdls::u8 *digest)
{
    char hex[65];
    tdls::sha256Hex(digest, hex);
    return hex;
}

void *Alloc(tdls::u32 bytes, tdls::u32 align, void *)
{
    void *p = nullptr;
    if (posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, bytes ? bytes : 1) != 0)
        return nullptr;
    return p;
}

void Release(void *p, void *)
{
    std::free(p);
}

struct MidiSet
{
    std::vector<std::string> names;
    std::vector<std::vector<tdls::u8>> data;
};

bool LoadMidiDir(const std::string &dir, const char *prefix, MidiSet &set)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return false;
    while (dirent *e = readdir(d))
    {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(0, std::strlen(prefix), prefix) == 0 && n.substr(n.size() - 4) == ".mid")
            set.names.push_back(n);
    }
    closedir(d);
    std::sort(set.names.begin(), set.names.end());
    for (auto &n : set.names)
    {
        set.data.emplace_back();
        if (!LoadFile(dir + "/" + n, set.data.back()))
            return false;
    }
    return !set.names.empty();
}

std::string RenderToolArgs(const th06midi::Settings &s)
{
    char b[160];
    std::snprintf(b, sizeof(b), "--rule %c --voices %u --reserve %u --gain-adjust %d --master-q16 %u --rate %u "
                               "--quantize-ms %u",
                  th06midi::RuleLetter(s.rule), s.voices, s.reserveVoices, s.gainAdjust, s.masterGainQ16,
                  s.synthRate, s.quantMs);
    return b;
}

int CmdBuild(const std::string &dlsPath, const std::string &midiDir, const th06midi::Settings &settings)
{
    std::vector<tdls::u8> dls;
    MidiSet set;
    if (!LoadFile(dlsPath, dls) || !LoadMidiDir(midiDir, "th06_", set))
    {
        std::fprintf(stderr, "cannot read inputs\n");
        return 1;
    }
    tdls::u8 dlsSha[32];
    tdls::sha256Buffer(dls.data(), (tdls::u32)dls.size(), dlsSha);
    std::vector<th06midi::BuildFile> files;
    for (size_t i = 0; i < set.names.size(); ++i)
        files.push_back({set.names[i].c_str(), set.data[i].data(), (tdls::u32)set.data[i].size()});
    th06midi::BuildAllocator allocator = {Alloc, Release, nullptr};
    th06midi::BuildResult r;
    if (!th06midi::BuildBlob(dls.data(), (tdls::u32)dls.size(), dlsSha, files.data(), (tdls::u32)files.size(),
                             "th06", (tdls::MapRule)settings.rule, allocator, r, nullptr, nullptr))
    {
        std::fprintf(stderr, "build failed: %s\n", r.error);
        return 1;
    }
    std::printf("rule %c\n", th06midi::RuleLetter(settings.rule));
    std::printf("dls_sha256 %s bytes %zu\n", Hex(dlsSha).c_str(), dls.size());
    std::printf("files %zu\n", files.size());
    std::printf("usage_sha256 %s tones %u notes %u\n", r.usageSha, r.tones, r.notes);
    std::printf("manifest_sha256 %s\n", r.manifestSha);
    std::printf("ins %u rgn %u wave %u uncovered %u unmapped %u\n", r.instruments, r.regions, r.waves,
                r.uncoveredNotes, r.unmappedTones);
    for (tdls::u32 v = 0; v < th06midi::kVariantCount; ++v)
        std::printf("blob flags=%u size=%u sha256=%s\n", th06midi::kVariantFlags[v], r.variantBytes[v],
                    Hex(r.variantSha256[v]).c_str());
    std::printf("arena dls_peak %u plan_peak %u storage %u\n", r.dlsArenaPeak, r.planArenaPeak, r.blobStorageBytes);
    tdls::u32 sum = 0;
    for (tdls::u32 off = 0; off < r.blobStorageBytes; off += 2048)
        sum ^= th06midi::BlockSum(r.blob + off);
    std::printf("block_sums_xor %08x blocks %u\n", sum, r.blobStorageBytes / 2048);
    Release(r.blob, nullptr);
    return 0;
}

int CmdScript(const std::string &blobPath, const std::string &midiDir, bool selftestOnly,
              const th06midi::Settings &settings)
{
    std::vector<tdls::u8> blob;
    if (!LoadFile(blobPath, blob))
    {
        std::fprintf(stderr, "cannot read blob\n");
        return 1;
    }
    MidiSet set;
    if (!selftestOnly && !LoadMidiDir(midiDir, "th06_", set))
    {
        std::fprintf(stderr, "cannot read MIDI files\n");
        return 1;
    }
    th06midi::ScriptHost host;
    std::memset(&host, 0, sizeof(host));
    host.blob = blob.data();
    host.blobBytes = (tdls::u32)blob.size();
    host.findSong = [](void *user, const char *name, const tdls::u8 **data, tdls::u32 *size) -> bool {
        MidiSet *s = static_cast<MidiSet *>(user);
        for (size_t i = 0; i < s->names.size(); ++i)
            if (s->names[i] == name)
            {
                *data = s->data[i].data();
                *size = (tdls::u32)s->data[i].size();
                return true;
            }
        return false;
    };
    host.user = &set;
    std::printf("settings %s\n", RenderToolArgs(settings).c_str());
    const tdls::u32 count = selftestOnly ? 0 : th06midi::kScriptCount;
    for (tdls::u32 i = 0; i < count; ++i)
    {
        th06midi::ScriptResult result;
        if (!th06midi::RunScript(th06midi::kScripts[i], host, settings, result))
        {
            std::fprintf(stderr, "script %s failed: %s\n", th06midi::kScripts[i].name, result.error);
            return 1;
        }
        std::printf("pccmd %s %s %s\n", th06midi::kScripts[i].name, th06midi::kScripts[i].pcCommand,
                    RenderToolArgs(settings).c_str());
        std::printf("script %s frames %u pcm_sha256 %s noteons %u lost %u maxactive %u\n",
                    th06midi::kScripts[i].name, result.frames, Hex(result.pcmSha256).c_str(), result.noteOns,
                    result.notesLost, result.maxActive);
    }
    th06midi::ScriptResult self;
    if (!th06midi::RunSelfTestScript(host, settings, self))
    {
        std::fprintf(stderr, "selftest script failed: %s\n", self.error);
        return 1;
    }
    std::printf("selftest frames %u pcm_sha256 %s noteons %u\n", self.frames, Hex(self.pcmSha256).c_str(),
                self.noteOns);
    return 0;
}

int CmdBudget(const std::string &blobPath, const std::string &layout, const th06midi::Settings &settings)
{
    tdls::u32 offset = 0, available = 0, sizes[6] = {};
    if (std::sscanf(layout.c_str(), "%u:%u:%u:%u:%u:%u:%u:%u", &offset, &available, &sizes[0], &sizes[1],
                    &sizes[2], &sizes[3], &sizes[4], &sizes[5]) != 8 ||
        offset + available != TH06_MIDI_EDRAM_STATE_BYTES ||
        settings.voices == 0 || settings.voices > 256)
    {
        std::fprintf(stderr, "invalid PSP state layout\n");
        return 1;
    }
    std::vector<tdls::u8> blob;
    tdls::TdsbView view;
    if (!LoadFile(blobPath, blob) || tdls::tdsbOpen(blob.data(), (tdls::u32)blob.size(), view) != tdls::TDSB_OK)
    {
        std::fprintf(stderr, "cannot read blob\n");
        return 1;
    }
    tdls::u32 arts = 1;
    for (tdls::u32 i = 0; i < view.nIns; ++i)
        arts += view.r16(view.insAt(i) + 10) != 0;
    for (tdls::u32 i = 0; i < view.nRgn; ++i)
        arts += view.r16(view.rgnAt(i) + 32) != 0;
    const tdls::u32 count[6] = {arts, view.nWave, view.nRgn, view.nIns, view.nMap, settings.voices};
    const tdls::u32 hostSizes[6] = {sizeof(tdls::SynthArt), sizeof(tdls::SynthWave), sizeof(tdls::SynthRegion),
                                  sizeof(tdls::SynthInstrument), sizeof(tdls::SynthMapEntry),
                                  sizeof(tdls::SynthVoice)};
    const char *names[6] = {"art", "wave", "region", "instrument", "map", "voice"};
    auto aligned = [](tdls::u32 bytes) { return (bytes + 7u) & ~7u; };
    tdls::SynthConfig config;
    th06midi::ToSynthConfig(settings, config);
    tdls::u32 need = tdls::Synth::memoryNeeded(view, config);
    for (tdls::u32 i = 0; i < 6; ++i)
    {
        need = need - aligned(count[i] * hostSizes[i]) + aligned(count[i] * sizes[i]);
        std::printf("arena_%s count %u psp_size %u bytes %u\n", names[i], count[i], sizes[i],
                    aligned(count[i] * sizes[i]));
    }
    tdls::u32 maxSlots = 0;
    const tdls::u32 fixed = need - aligned(settings.voices * sizes[5]);
    for (tdls::u32 voices = 1; voices <= 256; ++voices)
        if (fixed + aligned(voices * sizes[5]) <= available)
            maxSlots = voices;
    const tdls::u32 blobRounded = ((tdls::u32)blob.size() + 2047u) & ~2047u;
    const tdls::u32 blobCapacity = TH06_MIDI_EDRAM_BLOB_CAPACITY;
    std::printf("ME state_offset %u arena_need %u arena_available %u remaining %lld max_slots %u\n", offset,
                need, available, (long long)available - need, maxSlots);
    std::printf("ME blob_bytes %zu rounded %u capacity %u remaining %lld\n", blob.size(), blobRounded,
                blobCapacity, (long long)blobCapacity - blobRounded);
    std::printf("ME stack_reserve %u\n", TH06_MIDI_EDRAM_STACK_RESERVE);
    const tdls::u32 reserved = TH06_MIDI_EDRAM_STATE_BASE + TH06_MIDI_EDRAM_STATE_BYTES + blobRounded +
                              TH06_MIDI_EDRAM_STACK_RESERVE;
    const tdls::u32 physical[2] = {TH06_MIDI_EDRAM_FAT_TOP, 4u * 1024u * 1024u};
    const char *model[2] = {"PSP-1000", "Slim+"};
    for (tdls::u32 i = 0; i < 2; ++i)
        std::printf("ME model %s physical %u prefix %u state_reserved %u blob_rounded %u stack_reserved %u "
                    "free %lld fixed_blob_slack %lld extra_unused %u\n", model[i], physical[i],
                    TH06_MIDI_EDRAM_STATE_BASE, TH06_MIDI_EDRAM_STATE_BYTES, blobRounded,
                    TH06_MIDI_EDRAM_STACK_RESERVE, (long long)physical[i] - reserved,
                    (long long)blobCapacity - blobRounded, physical[i] - TH06_MIDI_EDRAM_FAT_TOP);
    const bool fits = need <= available && blobRounded <= blobCapacity;
    std::printf("ME BUDGET %s (compile-time PSP ABI, not hardware execution)\n", fits ? "OK" : "NG");
    return fits ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    std::string cmd = argc > 1 ? argv[1] : "";
    std::string dls, midi, blob, pspLayout;
    th06midi::Settings settings;
    th06midi::DefaultSettings(settings);
    for (int i = 2; i + 1 < argc; i += 2)
    {
        std::string a = argv[i];
        const char *v = argv[i + 1];
        if (a == "--dls")
            dls = v;
        else if (a == "--midi")
            midi = v;
        else if (a == "--blob")
            blob = v;
        else if (a == "--psp-layout")
            pspLayout = v;
        else if (a == "--rule")
            settings.rule = v[0] == 'A' ? tdls::MAP_A_CAPITAL : (v[0] == 'B' ? tdls::MAP_B_SUBCAPITAL : tdls::MAP_C_STRICT_LSB);
        else if (a == "--voices")
            settings.voices = (tdls::u32)std::atoi(v);
        else if (a == "--reserve")
            settings.reserveVoices = (tdls::u32)std::atoi(v);
        else if (a == "--gain-adjust")
            settings.gainAdjust = std::atoi(v);
        else if (a == "--master-q16")
            settings.masterGainQ16 = (tdls::u32)std::strtoul(v, nullptr, 0);
        else if (a == "--rate")
            settings.synthRate = (tdls::u32)std::atoi(v);
        else if (a == "--quantize-ms")
            settings.quantMs = (tdls::u32)std::atoi(v);
    }
    if (cmd == "build" && !dls.empty() && !midi.empty())
        return CmdBuild(dls, midi, settings);
    if (cmd == "script" && !blob.empty() && !midi.empty())
        return CmdScript(blob, midi, false, settings);
    if (cmd == "selftest" && !blob.empty())
        return CmdScript(blob, midi, true, settings);
    if (cmd == "budget" && !blob.empty() && !pspLayout.empty())
        return CmdBudget(blob, pspLayout, settings);
    std::fprintf(stderr, "usage: midi_pc_check build --dls F --midi DIR [--rule X] | script --blob F --midi DIR "
                         "[settings] | selftest --blob F [settings] | budget --blob F --psp-layout D [settings]\n");
    return 64;
}
