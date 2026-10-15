// TH06 PSP: MIDI synthesis settings.  The defaults follow the MSGS recording
// comparison (2026-10-14): bank rule C (Touhou songs carry no GS reset, so
// MSGS ignores CC0/CC32: melodic parts are (0,0,PC), drums pick the kit by PC
// and 32 playable voices plus 8 quick-stop reserve slots.  MSGS's Open path
// sets gain adjustment to 0; every value can be changed
// from TH06PSP/MIDI.CFG without rebuilding (key=value lines):
//
//   rule=C          A | B | C   (also selects which subset blob is cached)
//   voices=40       total slots (32 playable + 8 reserve), 1..256
//   reserve=8       voices always kept free for quick stops ([MS] extra voices)
//   gain_adjust=0   per-voice boost in 1/100 dB ([MS] SetGainAdjust)
//   master_q16=65536  final gain (Q16)
//   rate=22050      44100 | 22050 (22050 is upsampled 2x for the 44.1 kHz mixer)
//   me=1            0 keeps synthesis on the SC
//
// Header-only and integer-only: the ME side includes it too.
#pragma once

#include "tdls/subset.hpp"
#include "tdls/synth.hpp"

namespace th06midi
{

struct Settings
{
    tdls::u32 rule;
    tdls::u32 voices;
    tdls::u32 reserveVoices;
    tdls::s32 gainAdjust;
    tdls::u32 masterGainQ16;
    tdls::u32 synthRate;
    tdls::u32 useMe;
    tdls::u32 drumHold;   // 1: リズム ch の Note Off を無視（GS 音源の動き）。既定 0: gm.dls のドラムは
                          // Note Off でも自然減衰で同じ（単発試験で確認）なので、測った MSGS どおりにしておく
    tdls::u32 quantMs;    // 事象の時刻の丸め（0=1ms 駆動＝原作 PC の timeSetEvent、10=Win7 以降の MSGS の実測。
                          // 10 だとテンポ次第でドラムが最大 9ms ずれてリズムがよれる。既定 0）
};

inline void DefaultSettings(Settings &s)
{
    tdls::SynthConfig c;
    tdls::synthDefaultConfig(c);
    s.rule = c.mapRule;
    s.voices = c.voices;
    s.reserveVoices = c.reserveVoices;
    s.gainAdjust = c.gainAdjust;
    s.masterGainQ16 = c.masterGainQ16;
    s.synthRate = c.sampleRate;
    s.useMe = 1;
    s.drumHold = 0;
    s.quantMs = 0;
}

inline void ToSynthConfig(const Settings &s, tdls::SynthConfig &c)
{
    tdls::synthDefaultConfig(c);
    c.sampleRate = s.synthRate;
    c.voices = s.voices;
    c.reserveVoices = s.reserveVoices;
    c.mapRule = s.rule;
    c.gainAdjust = s.gainAdjust;
    c.masterGainQ16 = s.masterGainQ16;
    c.drumHold = s.drumHold;
}

inline char RuleLetter(tdls::u32 rule)
{
    return rule == tdls::MAP_B_SUBCAPITAL ? 'B' : (rule == tdls::MAP_C_STRICT_LSB ? 'C' : 'A');
}

inline tdls::u32 UpsampleFactor(const Settings &s)
{
    return s.synthRate == 22050 ? 2u : 1u;
}

// 2x sample repetition for 22,050 Hz synthesis (identical on SC and ME):
// input x[k] becomes (x[k], x[k]). The output stream does not
// depend on how the caller splits it into calls.
struct Upsample2x
{
    tdls::u32 pending;
    tdls::s16 pendL, pendR;
};

template <class Player>
inline void RenderUpsampled2x(Player &player, Upsample2x &u, tdls::s16 *out, tdls::u32 frames, tdls::s16 *tmp)
{
    tdls::u32 o = 0;
    if (u.pending && frames)
    {
        out[0] = u.pendL;
        out[1] = u.pendR;
        u.pending = 0;
        o = 1;
    }
    const tdls::u32 need = (frames - o + 1u) / 2u;
    if (need == 0)
        return;
    player.render(tmp, need);
    for (tdls::u32 k = 0; k < need; ++k)
    {
        const tdls::s32 l = tmp[2u * k];
        const tdls::s32 r = tmp[2u * k + 1u];
        out[2u * o] = static_cast<tdls::s16>(l);
        out[2u * o + 1u] = static_cast<tdls::s16>(r);
        ++o;
        if (o < frames)
        {
            out[2u * o] = static_cast<tdls::s16>(l);
            out[2u * o + 1u] = static_cast<tdls::s16>(r);
            ++o;
        }
        else
        {
            u.pendL = static_cast<tdls::s16>(l);
            u.pendR = static_cast<tdls::s16>(r);
            u.pending = 1;
        }
    }
}

} // namespace th06midi
