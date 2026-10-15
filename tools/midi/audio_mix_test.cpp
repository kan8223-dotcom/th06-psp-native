// Host arithmetic oracle for ACTUAL psp/audio_me.c SC and ME functions.
// The runner only substitutes the ME physical-address/cache boundary.
// No MIDI, copyrighted PCM, PSP scheduling, or hardware is simulated here.
#include "audio_me.h"
#include "midi/midi_me_protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

static std::vector<void *> gPhysical;
static void *HostPhysical(uint32_t id) { return gPhysical.at(id - 1); }
static void meLibDcacheInvalidateRange(uintptr_t, uintptr_t) {}
static void meLibDcacheWritebackRange(uintptr_t, uintptr_t) {}
#include "actual_audio_mix.hpp"

namespace
{
constexpr unsigned kSamples = TH06_PSP_ME_MAX_MIX_FRAMES * 2;
constexpr unsigned kGuard = 32;
constexpr short kCanary = 0x5aa5;
unsigned gChecks, gFailures, gCases;

bool Check(bool ok, const std::string &label)
{
    ++gChecks;
    if (!ok)
    {
        ++gFailures;
        std::fprintf(stderr, "FAIL %s\n", label.c_str());
    }
    return ok;
}

struct Guarded
{
    std::array<short, kSamples + kGuard * 2> storage;
    Guarded() { storage.fill(kCanary); }
    short *data() { return storage.data() + kGuard; }
    bool guards(unsigned samples) const
    {
        return std::all_of(storage.begin(), storage.begin() + kGuard,
                           [](short x) { return x == kCanary; }) &&
               std::all_of(storage.begin() + kGuard + samples, storage.end(),
                           [](short x) { return x == kCanary; });
    }
};

void RunMe(const Th06PspMixJob &job, short *output)
{
    MeSharedMailbox box{};
    gPhysical.clear();
    gPhysical.push_back(output);
    box.audioOutputPhys = 1;
    box.audioFrames = job.frames;
    box.audioInputCount = job.inputCount;
    box.audioMixDivisor = job.mixDivisor;
    const unsigned count = std::min<unsigned>(job.inputCount, TH06_PSP_ME_MAX_MIX_INPUTS);
    for (unsigned i = 0; i < count; ++i)
    {
        const auto &in = job.inputs[i];
        auto &out = box.audioInputs[i];
        if (in.samples)
        {
            gPhysical.push_back(const_cast<short *>(in.samples));
            out.sourcePhys = static_cast<uint32_t>(gPhysical.size());
        }
        out.frames = in.frames;
        out.destinationFrame = in.destinationFrame;
        out.channels = in.channels;
        out.gainQ16 = gain_to_q16(in.gain);
    }
    std::array<int, ME_AUDIO_EDRAM_ACCUM_FRAMES * 2 + 2> wide;
    wide.fill(0x1badcafe);
    process_audio_on_me(&box, wide.data() + 1);
    Check(wide.front() == 0x1badcafe && wide.back() == 0x1badcafe, "ME accumulator canaries");
}

void CompareUnity(Th06PspMixJob &job, const std::string &label)
{
    ++gCases;
    Guarded sc, me;
    mix_on_sc(&job, sc.data());
    RunMe(job, me.data());
    const unsigned samples = job.frames * 2;
    Check(std::memcmp(sc.data(), me.data(), samples * sizeof(short)) == 0, label + " legacy SC==ME");
    Check(sc.guards(samples) && me.guards(samples), label + " legacy guards");
#if AUDIO_MIX_CANDIDATE
    Guarded candidate;
    Check(th06_psp_me_audio_mix_unity_nowait(&job, candidate.data()) == 1, label + " candidate accepted");
    Check(std::memcmp(candidate.data(), me.data(), samples * sizeof(short)) == 0,
          label + " candidate==legacy ME all bytes");
    Check(candidate.guards(samples), label + " candidate guards");
#endif
}

void Invalid(const Th06PspMixJob *job, const std::string &label)
{
#if AUDIO_MIX_CANDIDATE
    Guarded out;
    Check(th06_psp_me_audio_mix_unity_nowait(job, out.data()) == 0, label + " refused");
    Check(out.guards(0), label + " no writes on refusal");
#else
    (void)job;
    (void)label;
#endif
}

uint32_t Random(uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
} // namespace

int main()
{
    std::array<short, kSamples> pcm{};
    Th06PspMixJob job{};
    job.frames = 1024;
    job.inputCount = 1;
    job.mixDivisor = 1;
    job.inputs[0] = {pcm.data(), 1024, 0, 2, 1.0f};
    for (int base = -32768; base < 32768; base += kSamples)
    {
        for (unsigned i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<short>(base + i);
        CompareUnity(job, "full s16 identity " + std::to_string(base));
        Guarded legacy;
        RunMe(job, legacy.data());
        Check(std::memcmp(legacy.data(), pcm.data(), sizeof(pcm)) == 0, "single unity memcpy oracle");
    }
    std::array<std::array<short, kSamples>, TH06_PSP_ME_MAX_MIX_INPUTS> sources{};
    uint32_t random = 0x06a0d10u;
    for (auto &source : sources)
        for (auto &sample : source) sample = static_cast<short>(Random(random));
    for (unsigned frames : {1u, 2u, 37u, 255u, 256u, 257u, 511u, 512u, 513u, 1023u, 1024u})
    {
        job.frames = frames;
        for (unsigned count : {0u, 1u, 2u, 8u, 32u, 64u})
        {
            job.inputCount = count;
            for (unsigned i = 0; i < count; ++i)
                job.inputs[i] = {sources[i].data(), frames, 0, (i & 1u) + 1u, 1.0f};
            CompareUnity(job, "size/count " + std::to_string(frames) + "/" + std::to_string(count));
        }
    }
    job.frames = 1024;
    job.inputCount = 64;
    for (short value : {std::numeric_limits<short>::max(), std::numeric_limits<short>::min()})
    {
        for (auto &source : sources) source.fill(value);
        for (unsigned i = 0; i < job.inputCount; ++i)
            job.inputs[i] = {sources[i].data(), 1024, 0, 2, 1.0f};
        CompareUnity(job, "64-way saturation " + std::to_string(value));
        Guarded out;
        RunMe(job, out.data());
        Check(std::all_of(out.data(), out.data() + kSamples, [value](short x) { return x == value; }),
              "64-way clipping agrees with closed-form bound");
    }
    for (auto &source : sources)
        for (auto &sample : source) sample = static_cast<short>(Random(random));
    for (unsigned trial = 0; trial < 400; ++trial)
    {
        job.frames = 1 + Random(random) % 1024;
        job.inputCount = Random(random) % 65;
        for (unsigned i = 0; i < job.inputCount; ++i)
        {
            const unsigned start = Random(random) % job.frames;
            job.inputs[i] = {sources[i].data(), 1 + Random(random) % 1024,
                             start, 1 + Random(random) % 2, 1.0f};
        }
        CompareUnity(job, "random overlapping/clipped descriptors " + std::to_string(trial));
    }
    job = {};
    job.frames = 1024;
    job.inputCount = 7;
    job.mixDivisor = 1;
    job.inputs[0] = {nullptr, 1024, 0, 2, 1.0f};
    job.inputs[1] = {pcm.data(), 1024, 0, 3, 1.0f};
    job.inputs[2] = {pcm.data(), 1024, 1024, 2, 1.0f};
    job.inputs[3] = {pcm.data(), 0, 0, 2, 1.0f};
    job.inputs[4] = {pcm.data(), std::numeric_limits<unsigned>::max(), 1023, 2, 1.0f};
    job.inputs[5] = {pcm.data(), 1024, std::numeric_limits<unsigned>::max(), 2, 1.0f};
    job.inputs[6] = {sources[0].data(), 1024, 257, 1, 1.0f};
    CompareUnity(job, "legacy skipped/clipped descriptor boundaries");
#if AUDIO_MIX_CANDIDATE
    // The production shortcut uses memmove, so even source==destination or
    // overlap keeps its promised byte order. Other mixed aliasing is not a
    // production contract (output is gFinalMix, sources are separate buffers).
    std::array<short, kSamples + 16> alias{};
    for (unsigned i = 0; i < alias.size(); ++i) alias[i] = static_cast<short>(i * 37u);
    const auto saved = alias;
    job = {};
    job.frames = 1024;
    job.inputCount = 1;
    job.mixDivisor = 1;
    job.inputs[0] = {alias.data(), 1024, 0, 2, 1.0f};
    Check(th06_psp_me_audio_mix_unity_nowait(&job, alias.data() + 8) == 1,
          "direct-copy overlapping output accepted");
    Check(std::equal(saved.begin(), saved.begin() + kSamples, alias.begin() + 8),
          "direct-copy overlap preserves all sample bytes");
    Check(std::equal(saved.begin(), saved.begin() + 8, alias.begin()) &&
          std::equal(saved.begin() + kSamples + 8, saved.end(), alias.begin() + kSamples + 8),
          "direct-copy overlap leaves outside samples untouched");
#endif
    // Negative oracle: fractional WAV/fade mixing is NOT generally identical
    // between float SC multiplication and Q16 ME gain. Do not broaden the new
    // no-wait route from unity MIDI+SE into this old non-unity behavior.
    pcm.fill(-32768);
    job = {};
    job.frames = 1024;
    job.inputCount = 1;
    job.mixDivisor = 1;
    job.inputs[0] = {pcm.data(), 1024, 0, 2, 0.1f};
    Guarded sc, me;
    mix_on_sc(&job, sc.data());
    RunMe(job, me.data());
    Check(sc.data()[0] == -3276 && me.data()[0] == -3277, "fractional gain negative oracle is detected");
    Invalid(&job, "fractional gain");
    for (float gain : {0.0f, -1.0f, 2.0f, std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::quiet_NaN()})
    {
        job.inputs[0].gain = gain;
        Invalid(&job, "non-unity or non-finite gain");
    }
    job.inputs[0].gain = 1.0f;
    job.mixDivisor = 0;
    Invalid(&job, "zero divisor not explicit unity");
    job.mixDivisor = 8;
    Invalid(&job, "non-unity divisor");
    job.mixDivisor = 1;
    job.frames = 0;
    Invalid(&job, "zero frames");
    job.frames = 1025;
    Invalid(&job, "oversize frames");
    job.frames = 1024;
    job.inputCount = 65;
    Invalid(&job, "oversize descriptors");
    Invalid(nullptr, "null job");
#if AUDIO_MIX_CANDIDATE
    job.inputCount = 1;
    Check(th06_psp_me_audio_mix_unity_nowait(&job, nullptr) == 0, "null output refused");
#endif
    std::printf("AUDIO_MIX\t%s\tCASES%u\tCHECKS%u\tFAILURES%u\tCANDIDATE%d\tNOT_HARDWARE\n",
                gFailures ? "FAIL" : "PASS", gCases, gChecks, gFailures, AUDIO_MIX_CANDIDATE);
    return gFailures ? EXIT_FAILURE : EXIT_SUCCESS;
}
