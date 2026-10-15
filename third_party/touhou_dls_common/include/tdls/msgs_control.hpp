// Integer control-rate behavior recovered from wdmaud.drv 10.0.26100.8737 x86.
// This contains formulas and state only; no downloaded instrument parameters.
#ifndef TDLS_MSGS_CONTROL_HPP
#define TDLS_MSGS_CONTROL_HPP

#include "types.hpp"

namespace tdls {
namespace msgs {

// PRELToPFRACT: cents -> Q12, including the reference's staged truncation.
s32 pitchRatio(s32 cents);

struct EgDef
{
    s64 attack, decay, release, delay, hold;
    s32 velToAttack, keyToDecay, sustain, pitchRange;
};

struct Eg
{
    s64 attack, decay, release, delay, hold;
    s64 start, stop;
    s32 sustain, pitchRange;
    bool active;
};

// Returns the first control span (delay if nonzero, otherwise attack).
s64 egStart(Eg &e, const EgDef &d, s64 start, u32 key, u32 velocity, u32 minimumSamples);
// nextSpan is replaced, not minimized: the voice caller takes the minimum.
s32 egLevel(const Eg &e, s64 now, s64 &nextSpan, bool volume);
s32 egPitch(const Eg &e, s64 now, s64 &nextSpan);
void egStop(Eg &e, s64 now);
void egQuickStop(Eg &e, s64 now, u32 sampleRate);
bool egInRelease(const Eg &e, s64 now);

struct Lfo
{
    s32 frequency; // 256-entry phase increment, with 16 fractional bits.
    s64 delay, start, span;
    bool active;
};

// active means any modulation-wheel/base pitch or volume depth is nonzero.
s64 lfoStart(Lfo &l, s32 frequency, s64 delay, s64 start, bool active);
// Returns -100..100. Inactive LFOs leave nextSpan untouched.
s32 lfoLevel(const Lfo &l, s64 now, s64 &nextSpan);

} // namespace msgs
} // namespace tdls

#endif
