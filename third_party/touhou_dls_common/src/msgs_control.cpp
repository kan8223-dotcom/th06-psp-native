#include "tdls/msgs_control.hpp"

#include "tdls/synth_tables.hpp"

namespace tdls {
namespace msgs {

namespace {

const s64 kNever = (s64)0x7fffffffffffffffull;

// GetLevel selects quarter/half/end, skipping a breakpoint within 100 samples.
s64 decaySpan(s64 duration, s64 elapsed)
{
    const s64 quarter = duration >> 2;
    if (elapsed < quarter - 100)
    {
        return quarter - elapsed;
    }
    const s64 half = duration >> 1;
    if (elapsed < half - 100)
    {
        return half - elapsed;
    }
    return duration - elapsed;
}

} // namespace

s32 pitchRatio(s32 cents)
{
    // da_voice.s RVA 2f77c..2f827: division truncates toward zero.
    if (cents >= -100 && cents <= 100)
    {
        return (s32)tables::kCents[cents + 100];
    }
    if (cents > 4800)
    {
        cents = 4800;
    }
    if (cents < -4800)
    {
        cents = -4800;
    }
    if (cents > 100)
    {
        const s32 semi = cents / 100;
        const s32 residual = cents % 100;
        const u32 fractional = (u32)tables::kCents[100 + residual] << (semi / 12);
        return (s32)((fractional * tables::kSemiTones[48 + semi % 12]) >> 12);
    }
    const s32 magnitude = -cents;
    const s32 semi = magnitude / 100;
    const s32 residual = magnitude % 100;
    const u32 fractional = (u32)tables::kCents[100 - residual] >> (magnitude / 1200);
    return (s32)((fractional * tables::kSemiTones[48 - semi % 12]) >> 12);
}

s64 egStart(Eg &e, const EgDef &d, s64 start, u32 key, u32 velocity, u32 minimumSamples)
{
    // da_voice.s RVA 2ff9c..300ea. Minimum attack is applied BEFORE modulation.
    const s64 minimum = (s64)minimumSamples;
    const s64 attack = d.attack < minimum ? minimum : d.attack;
    e.attack = attack * pitchRatio((s32)((s64)d.velToAttack * velocity / 127)) / 4096;
    const s32 keyRatio = pitchRatio((s32)((s64)d.keyToDecay * key / 127));
    e.hold = d.hold * keyRatio / 4096;
    e.decay = (d.decay * keyRatio / 4096) * (1000 - d.sustain) / 1000;
    e.release = d.release < minimum ? minimum : d.release;
    e.delay = d.delay;
    e.sustain = d.sustain;
    e.pitchRange = d.pitchRange;
    e.start = start;
    e.stop = kNever;
    e.active = true;
    return e.delay != 0 ? e.delay : e.attack;
}

s32 egLevel(const Eg &e, s64 now, s64 &nextSpan, bool volume)
{
    // da_voice.s RVA 2e137..2e432. Exactly at stop, evaluate the held envelope.
    if (now > e.stop)
    {
        const s64 elapsed = now - e.stop;
        if (elapsed >= e.release)
        {
            nextSpan = kNever;
            return 0;
        }
        s64 ignored;
        const s32 initial = egLevel(e, e.stop, ignored, volume);
        nextSpan = decaySpan(e.release, elapsed);
        return (s32)((s64)initial * (e.release - elapsed) / e.release);
    }
    s64 elapsed = now - e.start;
    if (elapsed < e.delay)
    {
        nextSpan = e.delay - elapsed;
        return 0;
    }
    elapsed -= e.delay;
    if (elapsed < e.attack)
    {
        s64 level = e.attack != 0 ? elapsed * 1000 / e.attack : 0;
        if (level < 0)
        {
            level = 0;
        }
        if (level > 1000)
        {
            level = 1000;
        }
        nextSpan = e.attack - elapsed;
        return volume ? (s32)tables::kAttack[level / 5] : (s32)level;
    }
    elapsed -= e.attack;
    if (elapsed < e.hold)
    {
        nextSpan = e.hold - elapsed;
        return volume ? (s32)tables::kAttack[200] : 1000;
    }
    elapsed -= e.hold;
    if (elapsed < e.decay)
    {
        nextSpan = decaySpan(e.decay, elapsed);
        return 1000 - (s32)((s64)(1000 - e.sustain) * elapsed / e.decay);
    }
    nextSpan = 44100; // Reference uses this constant even at other sample rates.
    return e.sustain;
}

s32 egPitch(const Eg &e, s64 now, s64 &nextSpan)
{
    // da_voice.s RVA 2e7cb..2e81f. Inactive states do not update the caller span.
    if (!e.active)
    {
        return 0;
    }
    if (e.pitchRange == 0)
    {
        nextSpan = 44100;
        return 0;
    }
    return (s32)((s64)egLevel(e, now, nextSpan, false) * e.pitchRange / 1000);
}

void egStop(Eg &e, s64 now)
{
    // da_voice.s RVA 30707..30760: even pitch EGs use volume=true here.
    if (e.active)
    {
        s64 ignored;
        e.release = e.release * egLevel(e, now, ignored, true) / 1000;
    }
    e.stop = now;
}

void egQuickStop(Eg &e, s64 now, u32 sampleRate)
{
    // da_voice.s RVA 2f8f1..2f978: scale ordinary release, then cap at rate/70.
    egStop(e, now);
    if (e.active && e.release > (s64)(sampleRate / 70u))
    {
        e.release = (s64)(sampleRate / 70u);
    }
}

bool egInRelease(const Eg &e, s64 now)
{
    // da_voice.s RVA 2e90d..2e93b: strict greater-than, not greater-or-equal.
    return e.active && now > e.stop;
}

s64 lfoStart(Lfo &l, s32 frequency, s64 delay, s64 start, bool active)
{
    // da_voice.s RVA 300ea..3015a: one eighth of a cycle, truncated.
    l.frequency = frequency;
    l.delay = delay;
    l.start = start;
    l.active = true;
    l.span = active && frequency > 0 ? 0x200000 / frequency : 44100;
    return l.span;
}

s32 lfoLevel(const Lfo &l, s64 now, s64 &nextSpan)
{
    // da_voice.s RVA 2e432..2e4af: Sine[(frequency*elapsed >> 16) & 255].
    if (!l.active)
    {
        return 0;
    }
    const s64 elapsed = now - l.start - l.delay;
    if (elapsed < 0)
    {
        nextSpan = -elapsed;
        return 0;
    }
    nextSpan = l.span;
    // Unsigned multiplication preserves the low 64-bit phase without overflow UB.
    const u64 phase = (u64)(s64)l.frequency * (u64)elapsed;
    return tables::kSine100[(phase >> 16) & 255u];
}

} // namespace msgs
} // namespace tdls
