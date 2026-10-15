#include "tdls/synth.hpp"

#include <string.h>

#include "tdls/smf_scan.hpp"
#include "tdls/subset.hpp"
#include "tdls/synth_tables.hpp"

namespace tdls {

namespace {

// DLS1 の接続（dls1.h の値。0x0002/0x0208/0x030c は DLS1 では予約で、MS サンプルも読まない）
enum
{
    SRC_NONE = 0x0000,
    SRC_LFO = 0x0001,
    SRC_KEYONVELOCITY = 0x0002,
    SRC_KEYNUMBER = 0x0003,
    SRC_EG2 = 0x0005,
    SRC_CC1 = 0x0081,
    DST_ATTENUATION = 0x0001,
    DST_PITCH = 0x0003,
    DST_PAN = 0x0004,
    DST_LFO_FREQUENCY = 0x0104,
    DST_LFO_STARTDELAY = 0x0105,
    DST_EG1_ATTACKTIME = 0x0206,
    DST_EG1_DECAYTIME = 0x0207,
    DST_EG1_RELEASETIME = 0x0209,
    DST_EG1_SUSTAINLEVEL = 0x020a,
    DST_EG2_ATTACKTIME = 0x030a,
    DST_EG2_DECAYTIME = 0x030b,
    DST_EG2_RELEASETIME = 0x030d,
    DST_EG2_SUSTAINLEVEL = 0x030e,
};

inline s32 clampS32(s32 v, s32 lo, s32 hi) { return v < lo ? lo : (v > hi ? hi : v); }


u64 mulQ48(u64 a, u64 b)
{
    const u64 mask = 0xffffffu;
    u64 ah = a >> 24, al = a & mask, bh = b >> 24, bl = b & mask;
    return ah * bh + ((ah * bl + al * bh + ((al * bl) >> 24)) >> 24);
}

// Keep the fractional 16 bits of DLS time cents; rounding cents loses samples.
u32 scaleExp2(u32 n, s64 scale, s64 denominator)
{
    s64 oct = scale / denominator;
    s64 rem = scale % denominator;
    if (rem < 0) { rem += denominator; --oct; }
    if (oct < -48) return 0;
    if (oct > 32) return 0xffffffffu;
    u32 bits = (u32)(((u64)rem << 32) / (u64)denominator);
    u64 q = 1ull << 48;
    for (u32 i = 0; i < 32; ++i)
        if (bits & (0x80000000u >> i)) q = mulQ48(q, tables::kExp2Frac48[i]);
    // Product Q48*n via 24-bit limbs, avoiding __int128 on MIPS/PPU.
    u64 lo = (q & 0xffffffu) * n;
    u64 hi = (q >> 24) * n + (lo >> 24);
    s64 shift = 24 - oct;
    if (shift >= 64) return 0;
    if (shift < 0) return 0xffffffffu;
    u64 result = hi >> shift;
    return result > 0xffffffffull ? 0xffffffffu : (u32)result;
}

u32 timeCentsToSamples(s32 scale, u32 rate, u32 maxSamples)
{
    if (scale == (s32)0x80000000u)
    {
        return 0;
    }
    u32 n = scaleExp2(rate, scale, 1200ll * 65536);
    return n > maxSamples ? maxSamples : n;
}

// [MS] Verify の LFO 周波数の範囲（22050Hz で m_pfFrequency 64..7600）を位相増分に直した上下限
u32 lfoIncFromPf(u32 pf, u32 rate) { return (u32)(((u64)pf * 22050u) / rate); }

u32 lfoIncFromCents(s32 scale, u32 rate)
{
    u32 k = (u32)((440ull << 24) / rate);
    u32 inc = scaleExp2(k, (s64)scale - 6900ll * 65536, 1200ll * 65536);
    u32 lo = lfoIncFromPf(64, rate), hi = lfoIncFromPf(7600, rate);
    return inc < lo ? lo : (inc > hi ? hi : inc);
}

s32 clampS16(s64 v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : (s32)v); }

// MSGS VRELToVFRACT truncates signed division toward zero, to 0.1dB.
s32 vrelToAmp(s32 v)
{
    s32 db = clampS32(v / 10, -1000, 0);
    return tables::kDbToVolume[1000 + db];
}

s64 mulDivNearest(s64 value, u32 factor, u32 divisor)
{
    s64 product = value * factor;
    return product >= 0 ? (product + divisor / 2u) / divisor :
                         -((-product + divisor / 2u) / divisor);
}

inline u8 priorityOf(u8 ch)
{
    // [MS] / DLS1: ch10 が最優先、次に 1..9、11..16。数字が大きいほど優先。
    u8 part = ch == 9 ? 0 : (ch < 9 ? (u8)(ch + 1) : ch);
    return (u8)(16 - part);
}

// Roland の番地（1 バイト 7bit）に i を足す（smf_scan と同じ）
u32 gsAddrAdd(u32 a, u32 i)
{
    u32 b0 = (a & 0x7fu) + i;
    u32 b1 = ((a >> 8) & 0x7fu) + (b0 >> 7);
    u32 b2 = ((a >> 16) & 0x7fu) + (b1 >> 7);
    return ((b2 & 0x7fu) << 16) | ((b1 & 0x7fu) << 8) | (b0 & 0x7fu);
}

// GS パートのブロック x → チャンネル（smf_scan と同じ SC の慣例）
u8 gsBlockToChannel(u32 block)
{
    if (block == 0)
    {
        return 9;
    }
    if (block <= 9)
    {
        return (u8)(block - 1);
    }
    return (u8)block;
}



typedef s16 __attribute__((may_alias)) s16a;

struct RdNative16
{
    static s32 at(const u8 *p, u32 i) { return ((const s16a *)p)[i]; }
};
struct RdLe16
{
    static s32 at(const u8 *p, u32 i)
    {
        const u8 *q = p + 2u * i;
        return (s32)(s16)(u16)((u32)q[0] | ((u32)q[1] << 8));
    }
};
struct RdBe16
{
    static s32 at(const u8 *p, u32 i)
    {
        const u8 *q = p + 2u * i;
        return (s32)(s16)(u16)(((u32)q[0] << 8) | (u32)q[1]);
    }
};
struct Rd8
{
    static s32 at(const u8 *p, u32 i) { return (s32)(s8)p[i] * 256; }
};

// Portable emulation of MSGS's MMX/scalar arithmetic. Keep the pair/ramp
// state across internal scratch-buffer splits, which are not control boundaries.
bool normalizePhase(SynthVoice &v)
{
    if (v.pos < v.end) return true;
    if (!v.looping) return false;
    v.pos = v.loopStart + (v.pos - v.loopStart) % v.loopLen;
    return true;
}

void advancePhase(SynthVoice &v, u32 step)
{
    u32 frac = v.frac + (step & 4095u);
    v.pos += (step >> 12) + (frac >> 12);
    v.frac = frac & 4095u;
}

void updateRamp(SynthVoice &v)
{
    v.accL += v.deltaL;
    v.accR += v.deltaR;
    v.accStep += v.deltaStep;
    v.step = (u32)(v.accStep > 0 ? v.accStep >> 8 : 0);
    v.rampCounter = v.rampPeriod;
}

template <class R>
u32 mixVoice(SynthVoice &v, s32 *mix, u32 n)
{
    u32 i = 0;
    for (; i < n; ++i)
    {
        if (!normalizePhase(v)) break;
        s32 s0 = v.pos < v.samples ? R::at(v.pcm, v.pos) : v.guard;
        s32 s1 = v.pos + 1u < v.samples ? R::at(v.pcm, v.pos + 1u) :
                 (v.pos + 1u == v.samples && v.hasGuard ? v.guard : (v.looping ? R::at(v.pcm, v.loopStart) : 0));
        s32 x = s0 + (s32)(((s64)(s1 - s0) * v.frac) >> 12);
        u32 advance = v.step;
        if (v.mmx)
        {
            if (!v.pairSecond)
            {
                v.rampCounter -= 2;
                if (!v.rampCounter) updateRamp(v);
                v.pairSecond = 1;
            }
            else
            {
                v.pairSecond = 0;
                advance = v.step;
            }
            s32 l = clampS32((s32)(v.accL > 0 ? v.accL >> 5 : 0), 0, 32767);
            s32 r = clampS32((s32)(v.accR > 0 ? v.accR >> 5 : 0), 0, 32767);
            mix[2u * i] = clampS16(mix[2u * i] + ((x * l) >> 16));
            mix[2u * i + 1u] = clampS16(mix[2u * i + 1u] + ((x * r) >> 16));
        }
        else
        {
            if (--v.rampCounter == 0) updateRamp(v);
            advance = v.step;
            s32 l = (s32)(v.accL >> 8), r = (s32)(v.accR >> 8);
            // Mix8 interpolates in the source's 8-bit domain.
            if (v.fmt == 3)
                x = s0 / 256 + (s32)(((s64)(s1 / 256 - s0 / 256) * v.frac) >> 12);
            u32 shift = v.fmt == 3 ? 5u : 13u;
            mix[2u * i] = clampS16(mix[2u * i] + (s32)(((s64)x * l) >> shift));
            mix[2u * i + 1u] = clampS16(mix[2u * i + 1u] + (s32)(((s64)x * r) >> shift));
        }
        advancePhase(v, advance);
    }
    return i;
}

void defaultArt(SynthArt &a, u32 rate)
{
    memset(&a, 0, sizeof(a));
    a.veg.sustain = 1000; // [MS] CSourceEG::Init: m_pcSustain = 1000
    a.peg.sustain = 1000;
    a.lfoInc = (u32)((5ull << 24) / rate); // MSGS LFO phase is Q24.
}

void buildArt(const TdsbView &b, u32 first, u32 count, SynthArt &a, u32 rate, u32 maxEg)
{
    defaultArt(a, rate);
    for (u32 k = 0; k < count; k++)
    {
        u32 o = b.connAt(first + k);
        u16 src = b.r16(o), ctl = b.r16(o + 2), dst = b.r16(o + 4);
        s32 sc = b.rs32(o + 8);
        switch (src)
        {
        case SRC_NONE:
            switch (dst)
            {
            case DST_LFO_FREQUENCY: a.lfoInc = lfoIncFromCents(sc, rate); break;
            case DST_LFO_STARTDELAY: a.lfoDelay = timeCentsToSamples(sc, rate, rate * 10u); break; // [MS] ≤441000@44.1k
            case DST_EG1_ATTACKTIME: a.veg.attack = timeCentsToSamples(sc, rate, maxEg); break;
            case DST_EG1_DECAYTIME: a.veg.decay = timeCentsToSamples(sc, rate, maxEg); break;
            case DST_EG1_RELEASETIME: a.veg.release = timeCentsToSamples(sc, rate, maxEg); break;
            case DST_EG1_SUSTAINLEVEL: a.veg.sustain = clampS32(sc >> 16, 0, 1000); break;
            case DST_EG2_ATTACKTIME: a.peg.attack = timeCentsToSamples(sc, rate, maxEg); break;
            case DST_EG2_DECAYTIME: a.peg.decay = timeCentsToSamples(sc, rate, maxEg); break;
            case DST_EG2_RELEASETIME: a.peg.release = timeCentsToSamples(sc, rate, maxEg); break;
            case DST_EG2_SUSTAINLEVEL: a.peg.sustain = clampS32(sc >> 16, 0, 1000); break;
            case DST_PAN: a.defaultPan = (sc >> 12) / 125; break; // [MS]
            default: break; // 0x0002/0x0208/0x030c（予約）などは読まない
            }
            break;
        case SRC_LFO:
            if (ctl == SRC_NONE)
            {
                if (dst == DST_ATTENUATION) a.lfoVol = clampS16(((s64)sc * 10) >> 16);
                else if (dst == DST_PITCH) a.lfoPitch = clampS16(sc >> 16);
            }
            else if (ctl == SRC_CC1)
            {
                if (dst == DST_ATTENUATION) a.lfoMwVol = clampS16(((s64)sc * 10) >> 16);
                else if (dst == DST_PITCH) a.lfoMwPitch = clampS16(sc >> 16);
            }
            break;
        case SRC_KEYONVELOCITY:
            if (dst == DST_EG1_ATTACKTIME) a.veg.velToAttack = clampS16(sc >> 16);
            else if (dst == DST_EG2_ATTACKTIME) a.peg.velToAttack = clampS16(sc >> 16);
            break; // ベロシティ→音量の接続は [MS] と同じく読まない（固定の曲線を使う）
        case SRC_KEYNUMBER:
            if (dst == DST_EG1_DECAYTIME) a.veg.keyToDecay = clampS16(sc >> 16);
            else if (dst == DST_EG2_DECAYTIME) a.peg.keyToDecay = clampS16(sc >> 16);
            break;
        case SRC_EG2:
            if (dst == DST_PITCH) a.peg.pitchRange = clampS32(sc >> 16, -1200, 1200);
            break;
        default:
            break;
        }
    }
    a.lfoActive = (a.lfoVol | a.lfoPitch | a.lfoMwVol | a.lfoMwPitch) != 0 ? 1 : 0;
}

u32 align8(u32 v) { return (v + 7u) & ~7u; }

u32 countArts(const TdsbView &b)
{
    u32 n = 1; // 0 番は既定値
    for (u32 i = 0; i < b.nIns; i++)
    {
        if (b.r16(b.insAt(i) + 10) != 0) n++;
    }
    for (u32 i = 0; i < b.nRgn; i++)
    {
        if (b.r16(b.rgnAt(i) + 32) != 0) n++;
    }
    return n;
}

const u32 kDrumNrpnBytes = 2u * 6u * 128u;

} // namespace

void synthDefaultConfig(SynthConfig &c)
{
    c.sampleRate = 22050;
    c.voices = 40;
    c.reserveVoices = 8;
    c.mapRule = MAP_C_STRICT_LSB;
    c.gainAdjust = 0; // wdmaud.drv: CSynth::Open resets the constructor's +600.
    c.masterGainQ16 = 0x10000;
    c.drumHold = 0;
}

Synth::Synth() { zero(); }

void Synth::zero()
{
    memset(&cfg_, 0, sizeof(cfg_));
    memset(&stats_, 0, sizeof(stats_));
    memset(ch_, 0, sizeof(ch_));
    masterVrel_ = 0;
    serial_ = 0;
    ready_ = false;
    hostLittle_ = true;
    gsMode_ = false;
    clock_ = eventBlockStart_ = 0;
    eventOffset_ = eventBlockFrames_ = eventCount_ = 0;
    recordingEvents_ = eventHistory_ = false;
    quickRelease_ = 0;
    maxEgSamples_ = 0;
    arts_ = 0;
    nArts_ = 0;
    waves_ = 0;
    nWaves_ = 0;
    rgns_ = 0;
    nRgns_ = 0;
    ins_ = 0;
    nIns_ = 0;
    map_ = 0;
    nMap_ = 0;
    voices_ = 0;
    nVoices_ = 0;
    drumNrpn_ = 0;
    memset(mix_, 0, sizeof(mix_));
}

u32 Synth::memoryNeeded(const TdsbView &b, const SynthConfig &cfg)
{
    u32 n = 0;
    n += align8(countArts(b) * (u32)sizeof(SynthArt)) + 8;
    n += align8(b.nWave * (u32)sizeof(SynthWave)) + 8;
    n += align8(b.nRgn * (u32)sizeof(SynthRegion)) + 8;
    n += align8(b.nIns * (u32)sizeof(SynthInstrument)) + 8;
    n += align8(b.nMap * (u32)sizeof(SynthMapEntry)) + 8;
    n += align8(cfg.voices * (u32)sizeof(SynthVoice)) + 8;
    n += align8(kDrumNrpnBytes) + 8;
    return n;
}

bool Synth::init(const TdsbView &b, const SynthConfig &cfg, Arena &arena)
{
    zero();
    cfg_ = cfg;
    if (cfg.sampleRate < 8000 || cfg.sampleRate > 96000 || cfg.voices == 0 || cfg.voices > 256 || !b.data)
    {
        return false;
    }
    if (cfg_.reserveVoices >= cfg_.voices)
    {
        cfg_.reserveVoices = cfg_.voices - 1;
    }
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
    hostLittle_ = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#else
    {
        u16 probe = 1;
        hostLittle_ = *(const u8 *)&probe == 1;
    }
#endif
    const u32 rate = cfg_.sampleRate;
    quickRelease_ = rate / 70u;     // [MS] QuickStopVoice: dwSampleRate / 70
    maxEgSamples_ = rate * 40u;     // [MS] FORCEBOUNDS(…, 0, 1764000)＝44.1kHz で 40 秒
    nArts_ = countArts(b);
    nWaves_ = b.nWave;
    nRgns_ = b.nRgn;
    nIns_ = b.nIns;
    nMap_ = b.nMap;
    nVoices_ = cfg_.voices;
    arts_ = (SynthArt *)arena.alloc(nArts_ * (u32)sizeof(SynthArt), 8);
    waves_ = (SynthWave *)arena.alloc(nWaves_ * (u32)sizeof(SynthWave), 8);
    rgns_ = (SynthRegion *)arena.alloc(nRgns_ * (u32)sizeof(SynthRegion), 8);
    ins_ = (SynthInstrument *)arena.alloc(nIns_ * (u32)sizeof(SynthInstrument), 8);
    map_ = (SynthMapEntry *)arena.alloc(nMap_ * (u32)sizeof(SynthMapEntry), 8);
    voices_ = (SynthVoice *)arena.alloc(nVoices_ * (u32)sizeof(SynthVoice), 8);
    drumNrpn_ = (u8 *)arena.alloc(kDrumNrpnBytes, 8);
    if (!arts_ || !waves_ || !rgns_ || !ins_ || !map_ || !voices_ || !drumNrpn_)
    {
        return false;
    }
    memset(voices_, 0, nVoices_ * sizeof(SynthVoice));
    memset(rgns_, 0, nRgns_ * sizeof(SynthRegion));

    // wave
    for (u32 i = 0; i < nWaves_; i++)
    {
        u32 o = b.waveAt(i);
        SynthWave &w = waves_[i];
        w.pcm = b.data + b.pcmOff + b.r32(o);
        w.samples = b.r32(o + 4);
        u32 wr = b.r32(o + 8);
        w.rate = wr;
        u64 ratio = (((u64)wr) << 24) / rate;
        w.rateRatioQ24 = ratio > 0x7fffffffull ? 0x7fffffffu : (u32)ratio;
        if (b.pcm8)
        {
            w.fmt = 3;
        }
        else if (b.bigEndian != hostLittle_ && (((size_t)w.pcm) & 1u) == 0)
        {
            w.fmt = 0;
        }
        else
        {
            w.fmt = b.bigEndian ? 2 : 1;
        }
        w.hasGuard = b.msgsGuard ? 1u : 0u;
        w.guard = 0;
        if (b.msgsGuard)
        {
            switch (w.fmt)
            {
            case 0: w.guard = RdNative16::at(w.pcm, w.samples); break;
            case 1: w.guard = RdLe16::at(w.pcm, w.samples); break;
            case 2: w.guard = RdBe16::at(w.pcm, w.samples); break;
            default: w.guard = Rd8::at(w.pcm, w.samples); break;
            }
        }
    }
    // 調音（0 番は既定値）
    defaultArt(arts_[0], rate);
    u32 nextArt = 1;
    for (u32 i = 0; i < nIns_; i++)
    {
        u32 o = b.insAt(i);
        SynthInstrument &in = ins_[i];
        in.key = ((u32)(b.r8(o) ? 1u : 0u) << 24) | ((u32)(b.r8(o + 1) & 0x7fu) << 16) |
                 ((u32)(b.r8(o + 2) & 0x7fu) << 8) | (u32)(b.r8(o + 3) & 0x7fu);
        in.firstRgn = b.r32(o + 4);
        in.nRgn = b.r16(o + 8);
        u32 nArt = b.r16(o + 10);
        u32 insArt = 0;
        if (nArt)
        {
            insArt = nextArt++;
            buildArt(b, b.r32(o + 12), nArt, arts_[insArt], rate, maxEgSamples_);
        }
        for (u32 k = 0; k < in.nRgn; k++)
        {
            u32 ri = in.firstRgn + k;
            u32 ro = b.rgnAt(ri);
            SynthRegion &r = rgns_[ri];
            r.keyLo = b.r8(ro);
            r.keyHi = b.r8(ro + 1);
            r.group = (u8)(b.r16(ro + 4) & 0xffu);
            r.allowOverlap = (b.r16(ro + 6) & 1u) ? 1 : 0; // F_RGN_OPTION_SELFNONEXCLUSIVE
            r.wave = b.r32(ro + 8);
            r.rootKey = (u8)(b.r16(ro + 12) & 0x7fu);
            r.fineTune = (s16)clampS32((s16)b.r16(ro + 14), -1200, 1200); // [MS] FORCEBOUNDS
            // x86 Download uses a wrapping 32-bit IMUL, then a signed word.
            s32 attenProduct = (s32)((u32)b.rs32(ro + 16) * 10u);
            r.atten = clampS32((s16)(attenProduct >> 16), -9600, 0);
            u32 loopStart = b.r32(ro + 24), loopLen = b.r32(ro + 28);
            u32 rflags = b.r16(ro + 34);
            r.oneShot = 1;
            if ((rflags & 1u) && r.wave != TDSB_NONE && loopLen > 0)
            {
                u64 e = (u64)loopStart + loopLen;
                if (e <= waves_[r.wave].samples + waves_[r.wave].hasGuard && loopLen >= 6u)
                {
                    r.loopStart = loopStart;
                    r.loopEnd = (u32)e;
                    r.oneShot = 0;
                }
            }
            u32 rArt = b.r16(ro + 32);
            if (rArt)
            {
                r.art = nextArt++;
                buildArt(b, b.r32(ro + 36), rArt, arts_[r.art], rate, maxEgSamples_);
            }
            else
            {
                r.art = insArt;
            }
        }
    }
    for (u32 i = 0; i < nMap_; i++)
    {
        u32 o = b.mapAt(i);
        map_[i].key = ((u32)(b.r8(o) ? 1u : 0u) << 24) | ((u32)(b.r8(o + 1) & 0x7fu) << 16) |
                      ((u32)(b.r8(o + 2) & 0x7fu) << 8) | (u32)(b.r8(o + 3) & 0x7fu);
        map_[i].ins = b.r32(o + 4);
    }
    ready_ = true;
    powerOn();
    return true;
}

void Synth::clearStats() { memset(&stats_, 0, sizeof(stats_)); }

u32 Synth::activeVoices() const
{
    u32 n = 0;
    for (u32 i = 0; i < nVoices_; i++) n += voices_[i].inUse;
    return n;
}

u8 Synth::drumNrpn(u32 map, u32 param, u32 note) const
{
    if (!drumNrpn_ || map > 1 || param > 5 || note > 127) return 0xff;
    return drumNrpn_[(map * 6u + param) * 128u + note];
}

void Synth::resetChannels()
{
    for (u32 c = 0; c < kChannels; c++)
    {
        SynthChannel &h = ch_[c];
        memset(&h, 0, sizeof(h));
        h.rhythm = c == 9 ? 1 : 0;
        h.volume = 100;    // [MS] CVolumeIn 既定 100
        h.pan = 64;
        h.expression = 127;
        h.reverb = 40;     // GS の既定（保持だけ）
        h.nrpnMsb = h.nrpnLsb = 127;
        h.bend = 0x2000;
        h.rpn = 0x3fff;
        h.bendRange = 200; // [MS] 既定 200 セント。リセットでも 200 に戻す（MS サンプルは戻さない＝[独自]）
        memset(h.nrpnPart, 0xff, sizeof(h.nrpnPart));
    }
    if (drumNrpn_)
    {
        memset(drumNrpn_, 0xff, kDrumNrpnBytes);
    }
}

void Synth::resetAll(bool clearVoices)
{
    if (clearVoices)
    {
        for (u32 i = 0; i < nVoices_; i++) clearVoice(voices_[i]); // [MS] AllNotesOff は即止め
    }
    resetChannels();
    masterVrel_ = 0;
    gsMode_ = false;
}

void Synth::powerOn()
{
    resetAll(true);
    serial_ = 0;
}

void Synth::clearVoice(SynthVoice &v)
{
    v.inUse = 0;
    v.noteOn = 0;
    v.sustained = 0;
    v.tagged = 0;
    v.dying = 0;
    v.hold = 0;
}

void Synth::deviceReset()
{
    // midiOutReset 相当（未確認）: 全チャンネルの発音中の音を通常の解放へ、サステインを外す。コントローラは触らない。
    for (u32 c = 0; c < kChannels; c++) ch_[c].sustain = 0;
    for (u32 i = 0; i < nVoices_; i++)
    {
        SynthVoice &v = voices_[i];
        if (v.inUse && v.noteOn) stopVoice(v, false);
    }
}

s32 Synth::exactInstrument(u32 key)
{
    for (u32 i = 0; i < nIns_; i++)
    {
        if (ins_[i].key == key) return (s32)i;
    }
    return -1;
}

s32 Synth::findKit(u8 prog)
{
    s32 first = -1;
    for (u32 i = 0; i < nIns_; i++)
    {
        u32 k = ins_[i].key;
        if ((k >> 24) == 0 || (k & 0x7fu) != prog) continue;
        if (((k >> 8) & 0x7f7fu) == 0) return (s32)i;
        if (first < 0) first = (s32)i;
    }
    return first;
}

s32 Synth::fallbackInstrument(u32 key)
{
    // [前任] subset.cpp の mapTone と同じ規則を、ブロブに入っている楽器だけに当てる
    u8 drum = (u8)(key >> 24), msb = (u8)(key >> 16), lsb = (u8)(key >> 8), prog = (u8)key;
    (void)lsb;
    s32 i;
    if (drum)
    {
        i = findKit(prog);
        return i >= 0 ? i : findKit(0);
    }
    i = exactInstrument(key);
    if (i >= 0) return i;
    if (cfg_.mapRule == MAP_A_CAPITAL || cfg_.mapRule == MAP_B_SUBCAPITAL)
    {
        i = exactInstrument(((u32)msb << 16) | prog);
        if (i >= 0) return i;
        u8 sub = (u8)(msb & ~7u);
        if (cfg_.mapRule == MAP_B_SUBCAPITAL && sub != 0 && sub != msb)
        {
            i = exactInstrument(((u32)sub << 16) | prog);
            if (i >= 0) return i;
        }
    }
    return exactInstrument(prog);
}

s32 Synth::findInstrument(u32 key)
{
    u32 lo = 0, hi = nMap_;
    while (lo < hi)
    {
        u32 mid = lo + (hi - lo) / 2;
        if (map_[mid].key == key)
        {
            return map_[mid].ins == TDSB_NONE ? -1 : (s32)map_[mid].ins;
        }
        if (map_[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    s32 i = fallbackInstrument(key);
    if (i >= 0) stats_.notesFallback++;
    return i;
}

SynthVoice *Synth::stealVoice(u8 priority)
{
    // Iterate QueueVoice order. The native comparison is asymmetric: an
    // older held note may replace a release candidate selected earlier.
    u16 order[256];
    u32 count = 0;
    for (u32 i = 0; i < nVoices_; ++i)
    {
        if (!voices_[i].inUse || voices_[i].priority > priority) continue;
        u32 at = count;
        while (at > 0)
        {
            const SynthVoice &previous = voices_[order[at - 1u]];
            if (previous.priority < voices_[i].priority ||
                (previous.priority == voices_[i].priority && previous.serial < voices_[i].serial)) break;
            order[at] = order[at - 1u];
            --at;
        }
        order[at] = (u16)i;
        ++count;
    }
    SynthVoice *best = 0;
    for (u32 i = 0; i < count; ++i)
    {
        SynthVoice &v = voices_[order[i]];
        if (!best || (!v.noteOn && (best->noteOn || best->egVrel > v.egVrel)) ||
            (v.noteOn && best->startFrame > v.startFrame)) best = &v;
    }
    if (best)
    {
        stats_.voicesStolen++;
        clearVoice(*best); // [MS] ClearVoice（その場で切る）
    }
    return best;
}

SynthVoice *Synth::allocVoice(u8 priority)
{
    for (u32 i = 0; i < nVoices_; i++)
    {
        if (!voices_[i].inUse) return &voices_[i];
    }
    return stealVoice(priority);
}

void Synth::stealNotes(u32 horizon)
{
    // [MS] StealNotes: 予備の声（既定 8）が足りなければ、優先度が低く古い声から素早く止める
    if (cfg_.reserveVoices == 0) return;
    u32 freeCount = 0, tagged = 0;
    for (u32 i = 0; i < nVoices_; i++)
    {
        if (!voices_[i].inUse) freeCount++;
        else if (voices_[i].tagged) tagged++;
    }
    if (freeCount + tagged >= cfg_.reserveVoices) return;
    u32 need = cfg_.reserveVoices - freeCount - tagged;
    while (need--)
    {
        SynthVoice *best = 0;
        for (u32 i = 0; i < nVoices_; i++)
        {
            SynthVoice &v = voices_[i];
            if (!v.inUse || v.tagged) continue;
            if (!best)
            {
                best = &v;
                continue;
            }
            if (v.priority != best->priority)
            {
                if (v.priority < best->priority) best = &v;
                continue;
            }
            bool vRel = !v.noteOn, bRel = !best->noteOn;
            if (vRel != bRel)
            {
                if (vRel) best = &v;
            }
            else if (vRel)
            {
                // OldestVoice 2ba55..2bae2 keeps the first creation-order
                // list entry on equal cached EG; physical slot reuse differs.
                if (v.egVrel < best->egVrel ||
                    (v.egVrel == best->egVrel && v.serial < best->serial)) best = &v;
            }
            else if (v.serial < best->serial)
            {
                best = &v;
            }
        }
        if (!best) break;
        quickStopAt(*best, best->t + horizon);
    }
}

void Synth::stopVoice(SynthVoice &v, bool quick)
{
    u64 absolute = clock_ + (recordingEvents_ ? eventOffset_ : 0u);
    u32 when = absolute > v.startFrame ? (u32)(absolute - v.startFrame) : 1u;
    if (quick) msgs::egQuickStop(v.veg, when, cfg_.sampleRate);
    else msgs::egStop(v.veg, when);
    msgs::egStop(v.peg, when);
    v.noteOn = 0;
    v.sustained = 0;
}

void Synth::quickStop(SynthVoice &v)
{
    u64 absolute = clock_ + (recordingEvents_ ? eventOffset_ : 0u);
    u32 when = absolute > v.startFrame ? (u32)(absolute - v.startFrame) : 1u;
    quickStopAt(v, when);
}

void Synth::quickStopAt(SynthVoice &v, u32 when)
{
    stats_.quickStops++;
    v.tagged = 1;
    if (!v.noteOn && !v.sustained)
    {
        // Repeated key / reserve recovery of a released voice uses the ORIGINAL
        // stop time; it must not restart either envelope at the new note time.
        msgs::egQuickStop(v.veg, v.veg.stop, cfg_.sampleRate);
        return;
    }
    msgs::egQuickStop(v.veg, when, cfg_.sampleRate);
    msgs::egStop(v.peg, when);
    v.noteOn = 0;
    v.sustained = 0;
}

void Synth::voiceTargets(SynthVoice &v, u32 tEnd, u32 renderEnd, s32 &volL, s32 &volR, s32 &pitch)
{
    const SynthChannel c = channelAt(v.channel, v.startFrame + tEnd);
    const SynthArt &a = *v.art;
    s64 span = (cfg_.sampleRate + 19u) / 20u;
    // CVoice::Mix reserves the pending stop breakpoint for its NEXT span.
    // This still applies when the target of the present interval is past stop.
    if (v.veg.stop > v.lastMixTime && v.veg.stop < renderEnd && v.veg.stop - v.lastMixTime < span)
        span = v.veg.stop - v.lastMixTime;
    s64 part = span;
    s32 lfo = msgs::lfoLevel(v.lfo, tEnd, part);
    if (part < span) span = part;
    s32 dp = (s32)c.modulation * a.lfoMwPitch / 127 + a.lfoPitch;
    s32 dv = (s32)c.modulation * a.lfoMwVol / 127 + a.lfoVol;
    part = span;
    s32 pe = msgs::egPitch(v.peg, tEnd, part);
    if (part < span) span = part;
    part = span;
    s32 level = msgs::egLevel(v.veg, tEnd, part, true);
    if (part < span) span = part;
    s32 eg = level * 96 / 10 - 9600;
    v.egVrel = eg;
    s32 vol = eg + dv * lfo / 100 + tables::kMidiToVrel[c.expression] +
              tables::kMidiToVrel[c.volume] + cfg_.gainAdjust;
    s32 pan = clampS32((s32)c.pan + a.defaultPan, 0, 127);
    volL = vol + tables::kPanToVrel[127 - pan];
    volR = vol + tables::kPanToVrel[pan];
    s32 bend = (((s32)c.bend - 8192) * c.bendRange) >> 13;
    pitch = dp * lfo / 100 + pe + bend;
    v.span = span < 1 ? 1u : (u32)span;
    v.dying = (msgs::egInRelease(v.veg, tEnd) || v.hold) && eg < -8000 ? 1u : 0u;
    if (v.dying && v.looping)
    {
        // CVoice::Mix 2f31e: CDigitalAudio+0x74 becomes one-shot before final Mix.
        // CopyFromWave 2c7fa retains the looping source length N+1 (including guard).
        // CDLSWave::Loaddata 27a01 zero-fills padding; sample N+1 after guard is zero.
        v.looping = 0;
        v.end = v.samples + 1u;
    }
}

void Synth::startVoice(SynthVoice &v, u8 ch, u8 key, u8 vel, const SynthRegion &r, u32 program)
{
    v.hold = 0;
    const SynthChannel &c = ch_[ch];
    const SynthArt &a = arts_[r.art];
    const SynthWave &w = waves_[r.wave];
    memset(&v, 0, sizeof(v));
    v.inUse = 1;
    v.noteOn = 1;
    v.channel = ch;
    v.key = key;
    v.priority = priorityOf(ch);
    v.group = r.group;
    v.program = program;
    v.serial = ++serial_;
    v.startFrame = clock_ + (recordingEvents_ ? eventOffset_ : 0u);
    v.lastMixTime = -1;
    v.art = &a;
    v.pcm = w.pcm;
    v.fmt = w.fmt;
    v.samples = w.samples;
    v.guard = w.guard;
    v.hasGuard = w.hasGuard;
    if (!r.oneShot)
    {
        v.looping = 1;
        v.loopStart = r.loopStart;
        v.end = r.loopEnd;
        v.loopLen = r.loopEnd - r.loopStart;
    }
    else v.end = w.samples;
    s32 pitch = c.fineTune + (c.rhythm ? 0 : c.coarseTune) + r.fineTune +
                ((s32)key - (s32)r.rootKey) * 100;
    v.basePitch = clampS32(pitch, -4800, 4800);
    v.baseStep = (u32)(((u64)w.rate * (u32)msgs::pitchRatio(v.basePitch)) / cfg_.sampleRate);
    v.step = v.baseStep;
    v.baseVol = masterVrel_ + tables::kMidiToVrel[vel] + r.atten;
    s64 span = (cfg_.sampleRate + 19u) / 20u;
    s64 part = msgs::egStart(v.veg, a.veg, 0, key, vel, cfg_.sampleRate / 1000u);
    if (part < span) span = part;
    part = msgs::egStart(v.peg, a.peg, 0, key, vel, 0);
    if (part < span) span = part;
    part = msgs::lfoStart(v.lfo, (s32)a.lfoInc, a.lfoDelay, 0, a.lfoActive != 0);
    if (part < span) span = part;
    // EG2 may have no attack. A zero initial span evaluates t=0 before rendering.
    v.span = span < 0 ? 0u : (u32)span;
    v.volL = v.volR = -9600;
    v.egVrel = 0; // Native StartVoice 2ff80..2ff84; cached level before first Mix.
    v.ampL = v.ampR = 0;
}

void Synth::noteOn(u8 ch, u8 key, u8 vel)
{
    SynthChannel &c = ch_[ch];
    stats_.noteOns++;
    if (c.mono)
    {
        for (u32 i = 0; i < nVoices_; i++)
        {
            SynthVoice &v = voices_[i];
            if (v.inUse && v.noteOn && v.channel == ch) stopVoice(v, false);
        }
    }
    u32 prog = ((c.rhythm ? 1u : 0u) << 24) | ((u32)c.msb << 16) | ((u32)c.lsb << 8) | c.program;
    s32 ii = c.rhythm ? findKit(c.program) : exactInstrument(gsMode_ ? prog : c.program);
    if (ii < 0 && c.rhythm) ii = findKit(0);
    if (ii < 0)
    {
        stats_.notesUnmapped++;
        return;
    }
    const SynthInstrument &in = ins_[ii];
    const SynthRegion *r = 0;
    for (u32 k = 0; k < in.nRgn; k++)
    {
        const SynthRegion &cand = rgns_[in.firstRgn + k];
        if (key >= cand.keyLo && key <= cand.keyHi) // [MS] 鍵域だけで選ぶ（ベロシティ範囲は見ない）
        {
            r = &cand;
            break;
        }
    }
    if (!r && c.rhythm)
    {
        s32 standard = findKit(0);
        if (standard >= 0)
            for (u32 k = 0; k < ins_[standard].nRgn; ++k)
            {
                const SynthRegion &cand = rgns_[ins_[standard].firstRgn + k];
                if (key >= cand.keyLo && key <= cand.keyHi) { r = &cand; break; }
            }
    }
    if (!r || r->wave == TDSB_NONE)
    {
        stats_.notesNoRegion++;
        return;
    }
    for (u32 i = 0; i < nVoices_; i++)
    {
        SynthVoice &v = voices_[i];
        if (!v.inUse || v.channel != ch) continue;
        if (!r->allowOverlap && v.key == key)
        {
            quickStop(v); // [MS] 同じ鍵の重なりを許さないリージョン
        }
        // QueueNotes performs overlap and key-group QuickStop as independent passes.
        if (r->group != 0 && v.group == r->group && v.program == prog)
        {
            quickStop(v); // [MS] キーグループ（同じチャンネル・同じ音色）
        }
    }
    SynthVoice *v = allocVoice(priorityOf(ch));
    if (!v)
    {
        stats_.notesLost++;
        return;
    }
    startVoice(*v, ch, key, vel, *r, prog);
    stats_.notesStarted++;
    u32 act = activeVoices();
    if (act > stats_.maxActive) stats_.maxActive = act;
}

void Synth::noteOff(u8 ch, u8 key)
{
    // [MS] 発音中・サステインでない同じ鍵のうち一番古い声を 1 つ
    SynthVoice *best = 0;
    for (u32 i = 0; i < nVoices_; i++)
    {
        SynthVoice &v = voices_[i];
        if (v.inUse && v.noteOn && !v.sustained && v.channel == ch && v.key == key)
        {
            if (!best || v.serial < best->serial) best = &v;
        }
    }
    if (!best) return;
    if (cfg_.drumHold && ch_[ch].rhythm)
    {
        // ゲート 1 ティックのドラム（紅魔郷は 289 音中 288 音）を Win7 以降の MSGS のように
        // 切らず、GS 音源と同じく最後まで鳴らす。EG が -80dB を切った時点で声を返す
        // （下の dying 判定）。ループ付きでサステインがそれより大きい音は止まらなく
        // なるので、その音だけは従来どおり Note Off を効かせる。
        // sustain は 0.1% 単位: level*96/10-9600 < -8000  <=>  level < 166.7
        if (!best->looping || best->veg.sustain <= 166)
        {
            best->hold = 1;
            return;
        }
    }
    if (ch_[ch].sustain) best->sustained = 1;
    else stopVoice(*best, false);
}

void Synth::setSustain(u8 ch, bool on)
{
    ch_[ch].sustain = on ? 1 : 0;
    if (on) return;
    for (u32 i = 0; i < nVoices_; i++)
    {
        SynthVoice &w = voices_[i];
        if (w.inUse && w.sustained && w.channel == ch) stopVoice(w, false);
    }
}

void Synth::dataEntry(u8 ch, bool msb, u8 v)
{
    SynthChannel &c = ch_[ch];
    if (msb) c.data = (u16)((c.data & 0x7fu) | ((u32)v << 7));
    else c.data = (u16)((c.data & ~0x7fu) | v);
    if (c.paramIsNrpn)
    {
        if (!msb) return;
        // NRPN は保持だけ（使い道は後で決める）。GS のパート NRPN と ドラム NRPN
        static const u8 kPartLsb[8] = {8, 9, 10, 32, 33, 99, 100, 102};
        if (c.nrpnMsb == 1)
        {
            for (u32 i = 0; i < 8; i++)
            {
                if (kPartLsb[i] == c.nrpnLsb)
                {
                    c.nrpnPart[i] = v;
                    stats_.nrpnStored++;
                    break;
                }
            }
        }
        else if (c.nrpnMsb >= 24 && c.nrpnMsb <= 31 && (c.rhythm == 1 || c.rhythm == 2) && drumNrpn_)
        {
            static const s8 kDrumParam[8] = {0, -1, 1, -1, 2, 3, 4, 5}; // 24,26,28,29,30,31
            s8 p = kDrumParam[c.nrpnMsb - 24];
            if (p >= 0)
            {
                drumNrpn_[((u32)(c.rhythm - 1) * 6u + (u32)p) * 128u + c.nrpnLsb] = v;
                stats_.nrpnStored++;
            }
        }
        return;
    }
    switch (c.rpn)
    {
    case 0x0000: // ベンド幅（[MS] MSB だけ、LSB は Roland と同じく無視）
        if (msb) c.bendRange = (s32)v * 100;
        break;
    case 0x0001: // 微調整（[MS] (data14-8192)*100/8192 セント）
        c.fineTune = (((s32)c.data - 8192) * 100) / 8192;
        break;
    case 0x0002: // 粗調整（[MS] MSB だけ、100*(v-64)）
        if (msb) c.coarseTune = 100 * ((s32)v - 64);
        break;
    default:
        break;
    }
}

void Synth::controlChange(u8 ch, u8 cc, u8 v)
{
    SynthChannel &c = ch_[ch];
    switch (cc)
    {
    case 0: if (gsMode_) c.cc0 = v; break;
    case 32: if (gsMode_) c.cc32 = v; break;
    case 1: c.modulation = v; break;
    case 7: c.volume = v; break;
    case 10: c.pan = v; break;
    case 11: c.expression = v; break;
    case 6: dataEntry(ch, true, v); break;
    case 38: dataEntry(ch, false, v); break;
    case 91: c.reverb = v; break;  // センド量は保持だけ（リバーブ本体は未実装）
    case 93: c.chorus = v; break;
    case 94: c.delay = v; break;
    case 98: c.nrpnLsb = v; c.paramIsNrpn = 1; c.rpn = 0x3fff; break; // [MS] NRPN を選ぶと RPN は無効
    case 99: c.nrpnMsb = v; c.paramIsNrpn = 1; c.rpn = 0x3fff; break;
    case 100: c.rpn = (u16)((c.rpn & 0x3f80u) | v); c.paramIsNrpn = 0; break;
    case 101: c.rpn = (u16)((c.rpn & 0x7fu) | ((u32)v << 7)); c.paramIsNrpn = 0; break;
    case 121: // [MS] Reset All Controllers: 値が 0 以外なら音量 100・パン 64 も。最後にサステイン OFF
        if (v)
        {
            c.volume = 100;
            c.pan = 64;
        }
        c.expression = 127;
        c.bend = 0x2000;
        c.modulation = 0;
        c.reverb = 40;
        setSustain(ch, false);
        break;
    case 64: // wdmaud.drv: any nonzero value enables sustain.
        setSustain(ch, v != 0);
        break;
    case 120: // All Sounds Off（[MS] 通常の解放）
    case 126: // Mono
    case 127: // Poly
        if (cc == 126) c.mono = 1;
        if (cc == 127) c.mono = 0;
        for (u32 i = 0; i < nVoices_; i++)
        {
            SynthVoice &w = voices_[i];
            if (w.inUse && w.noteOn && w.channel == ch) stopVoice(w, false);
        }
        break;
    case 123: // All Notes Off（[MS] サステイン中なら保持に回す）
        for (u32 i = 0; i < nVoices_; i++)
        {
            SynthVoice &w = voices_[i];
            if (w.inUse && w.noteOn && !w.sustained && w.channel == ch)
            {
                if (c.sustain) w.sustained = 1;
                else stopVoice(w, false);
            }
        }
        break;
    default: break;
    }
}

void Synth::shortMessage(u8 status, u8 d1, u8 d2)
{
    if (!ready_ || status < 0x80 || status >= 0xf0) return;
    u8 ch = (u8)(status & 0x0f);
    d1 &= 0x7f;
    d2 &= 0x7f;
    switch (status & 0xf0)
    {
    case 0x80: noteOff(ch, d1); break;
    case 0x90:
        if (d2) noteOn(ch, d1, d2);
        else noteOff(ch, d1);
        break;
    case 0xb0: controlChange(ch, d1, d2); break;
    case 0xc0: // [前任]/[MS] バンクは Program Change の時点で確定
        ch_[ch].msb = gsMode_ ? ch_[ch].cc0 : 0;
        ch_[ch].lsb = gsMode_ ? ch_[ch].cc32 : 0;
        ch_[ch].program = d1;
        break;
    case 0xe0: ch_[ch].bend = (u16)(d1 | ((u32)d2 << 7)); break;
    default: break; // アフタータッチは DLS1 に無い
    }
    if (recordingEvents_ && ((status & 0xf0) == 0xe0 ||
        ((status & 0xf0) == 0xb0 && (d1 == 1 || d1 == 7 || d1 == 10 || d1 == 11 || d1 == 6 || d1 == 121))))
        recordChannel(ch);
}

void Synth::sysEx(const u8 *msg, u32 len)
{
    if (!ready_ || !msg || len < 2 || msg[0] != 0xf0)
    {
        stats_.sysExIgnored++;
        return;
    }
    const u8 *p = msg + 1;
    u32 n = len - 1;
    // [前任] GM System On（7E dev 09 01）で全チャンネルを電源投入時へ
    if (n >= 4 && p[0] == 0x7e && p[2] == 0x09 && p[3] == 0x01)
    {
        resetAll(true);
        stats_.resets++;
        return;
    }
    // マスターボリューム（7F dev 04 01 ll mm）[MS]
    if (n >= 6 && p[0] == 0x7f && p[2] == 0x04 && p[3] == 0x01)
    {
        masterVrel_ = tables::kMidiToVrel[p[5] & 0x7fu];
        return;
    }
    u32 addr;
    const u8 *d;
    u32 dn;
    bool ok;
    if (smfParseGsDt1(p, n, &addr, &d, &dn, &ok))
    {
        // [前任] GS リセット（40 00 7F）と SC-88 のモード設定（00 00 7F）もリセット扱い
        if (addr == 0x40007fu)
        {
            resetAll(true);
            gsMode_ = true;
            stats_.resets++;
            return;
        }
        bool used = false;
        for (u32 i = 0; i < dn; i++)
        {
            u32 a = gsAddrAdd(addr, i);
            if ((a & 0xfff000u) != 0x401000u) continue;
            if (gsMode_ && (a & 0xffu) == 0x15u)
            {
                ch_[gsBlockToChannel((a >> 8) & 0xfu)].rhythm = (u8)(d[i] & 0x7fu);
                used = true;
            }
        }
        if (!used) stats_.sysExIgnored++;
        return;
    }
    stats_.sysExIgnored++;
}

void Synth::renderBlock(s16 *out, u32 n, u32 renderRemaining, u32 renderOffset, u32 renderFrames, u64 nowFrame)
{
    memset(mix_, 0, n * 2u * sizeof(s32));
    // QueueVoice: priority ascending, then insertion time. Slot reuse must not
    // change the order of the saturating additions.
    u16 order[256];
    u32 count = 0;
    for (u32 i = 0; i < nVoices_; ++i)
    {
        if (!voices_[i].inUse) continue;
        u32 at = count;
        while (at > 0)
        {
            const SynthVoice &previous = voices_[order[at - 1u]];
            const SynthVoice &current = voices_[i];
            if (previous.priority < current.priority ||
                (previous.priority == current.priority && previous.serial < current.serial)) break;
            order[at] = order[at - 1u];
            --at;
        }
        order[at] = (u16)i;
        ++count;
    }
    for (u32 oi = 0; oi < count; ++oi)
    {
        SynthVoice &v = voices_[order[oi]];
        u32 offset = 0;
        if (v.startFrame > nowFrame)
        {
            u64 delay = v.startFrame - nowFrame;
            if (delay >= n) continue;
            offset = (u32)delay;
        }
        while (offset < n && v.inUse)
        {
            if (!v.controlRemain)
            {
                u32 interval = v.span < renderRemaining - offset ? v.span : renderRemaining - offset;
                s32 l, r, pitch;
                voiceTargets(v, v.t + interval, v.t + renderRemaining - offset, l, r, pitch);
                if (!interval)
                {
                    // Zero first span: evaluate at note start and try the newly
                    // returned control span, without consuming a sample.
                    v.pitch = pitch;
                    v.volL = l;
                    v.volR = r;
                    v.ampL = vrelToAmp(l + v.baseVol);
                    v.ampR = vrelToAmp(r + v.baseVol);
                    v.step = (u32)(((u64)(u32)msgs::pitchRatio(pitch) * v.baseStep) >> 12);
                    continue;
                }
                u32 dp = (u32)(pitch >= v.pitch ? pitch - v.pitch : v.pitch - pitch);
                u32 dl = (u32)(l >= v.volL ? l - v.volL : v.volL - l);
                u32 dr = (u32)(r >= v.volR ? r - v.volR : v.volR - r);
                u32 dv = dl > dr ? dl : dr;
                u32 d = (2u * dp > dv ? 2u * dp : dv) / 2u;
                u32 period = d ? 8u * interval / d : 512u;
                if (period < 1u) period = 1u;
                if (period > 512u) period = 512u;
                period = (period + 3u) & ~3u;
                s32 goalL = vrelToAmp(l + v.baseVol), goalR = vrelToAmp(r + v.baseVol);
                v.goalStep = (u32)(((u64)(u32)msgs::pitchRatio(pitch) * v.baseStep) >> 12);
                if (v.looping && (v.goalStep >> 12) >= v.loopLen)
                {
                    ++stats_.loopKilled;
                    clearVoice(v);
                    break;
                }
                v.accL = (s64)v.ampL * 256;
                v.accR = (s64)v.ampR * 256;
                v.accStep = (s64)v.step * 256;
                v.deltaL = mulDivNearest(goalL - v.ampL, period * 256u, interval);
                v.deltaR = mulDivNearest(goalR - v.ampR, period * 256u, interval);
                v.deltaStep = mulDivNearest((s64)v.goalStep - v.step, period * 256u, interval);
                v.rampPeriod = v.rampCounter = period;
                v.controlRemain = v.controlTotal = interval;
                v.intervalDone = 0;
                v.mmx = interval > 8u ? 1u : 0u;
                v.pairSecond = 0;
                v.ampL = goalL;
                v.ampR = goalR;
                v.pitch = pitch;
                v.volL = l;
                v.volR = r;
            }
            u32 k = v.controlRemain < n - offset ? v.controlRemain : n - offset;
            u32 done;
            switch (v.fmt)
            {
            case 0: done = mixVoice<RdNative16>(v, mix_ + 2u * offset, k); break;
            case 1: done = mixVoice<RdLe16>(v, mix_ + 2u * offset, k); break;
            case 2: done = mixVoice<RdBe16>(v, mix_ + 2u * offset, k); break;
            default: done = mixVoice<Rd8>(v, mix_ + 2u * offset, k); break;
            }
            v.t += done;
            offset += done;
            v.controlRemain -= done;
            v.intervalDone += done;
            if (done < k || (!v.controlRemain && v.dying)) clearVoice(v);
            else if (!v.controlRemain) v.step = v.goalStep;
        }
    }
    const u32 g = cfg_.masterGainQ16;
    u32 peak = stats_.mixPeak;
    u32 mmxElements = (renderFrames * 2u) & ~3u;
    for (u32 i = 0; i < n * 2u; ++i)
    {
        s32 m = mix_[i] * 2; // FinishMix doubles the saturated per-voice sum.
        u32 magnitude = m < 0 ? (u32)-m : (u32)m;
        if (magnitude > peak) peak = magnitude;
        if (g != 0x10000u) m = (s32)(((s64)m * g) >> 16);
        s32 minimum = renderOffset * 2u + i < mmxElements ? -32768 : -32767;
        if (m < minimum || m > 32767)
        {
            ++stats_.clippedSamples;
            m = clampS32(m, minimum, 32767);
        }
        out[i] = (s16)m;
    }
    stats_.mixPeak = peak;
}

void Synth::beginEventBlock(u32 frames)
{
    // The native engine replenishes reserve voices before queuing new notes.
    stealNotes(frames);
    eventBlockStart_ = clock_;
    eventBlockFrames_ = frames;
    eventOffset_ = eventCount_ = 0;
    memcpy(blockChannels_, ch_, sizeof(ch_));
    recordingEvents_ = eventHistory_ = true;
}

bool Synth::setEventOffset(u32 offset)
{
    if (!recordingEvents_ || offset > eventBlockFrames_) return false;
    eventOffset_ = offset;
    return true;
}

void Synth::endEventBlock()
{
    recordingEvents_ = false;
    eventOffset_ = 0;
}

void Synth::recordChannel(u8 ch)
{
    if (eventCount_ >= kChannelEvents) return;
    ChannelEvent &e = events_[eventCount_++];
    e.offset = eventOffset_;
    e.channel = ch;
    const SynthChannel &c = ch_[ch];
    e.bend = c.bend;
    e.bendRange = c.bendRange;
    e.modulation = c.modulation;
    e.volume = c.volume;
    e.pan = c.pan;
    e.expression = c.expression;
}

SynthChannel Synth::channelAt(u8 ch, u64 frame) const
{
    if (!eventHistory_) return ch_[ch];
    SynthChannel result = blockChannels_[ch];
    for (u32 i = 0; i < eventCount_; ++i)
    {
        const ChannelEvent &e = events_[i];
        if (e.channel != ch || eventBlockStart_ + e.offset > frame) continue;
        result.bend = e.bend;
        result.bendRange = e.bendRange;
        result.modulation = e.modulation;
        result.volume = e.volume;
        result.pan = e.pan;
        result.expression = e.expression;
    }
    return result;
}

void Synth::renderSlice(s16 *out, u32 frames, u32 remainingInBlock, u32 offsetInBlock, u32 blockFrames)
{
    if (!ready_)
    {
        memset(out, 0, frames * 2u * sizeof(s16));
        return;
    }
    if (offsetInBlock == 0 && !eventHistory_) stealNotes(remainingInBlock);
    u32 rendered = 0, total = frames;
    while (frames > 0)
    {
        u32 n = frames > (u32)kMaxBlock ? (u32)kMaxBlock : frames;
        renderBlock(out, n, remainingInBlock, offsetInBlock, blockFrames, clock_ + rendered);
        out += n * 2u;
        frames -= n;
        remainingInBlock -= n;
        offsetInBlock += n;
        rendered += n;
    }
    clock_ += total;
    if (offsetInBlock >= blockFrames)
        for (u32 i = 0; i < nVoices_; ++i)
            if (voices_[i].inUse)
            {
                // CVoice::Mix 2f1a5..2f1cd -> 2f72b..2f75b: a note at
                // the inclusive block end records the block-start clock.
                voices_[i].lastMixTime = voices_[i].startFrame == clock_ ?
                    -(s64)blockFrames : (s64)voices_[i].t;
            }
    if (eventHistory_ && clock_ >= eventBlockStart_ + eventBlockFrames_) eventHistory_ = false;
}

void Synth::render(s16 *out, u32 frames)
{
    renderSlice(out, frames, frames, 0, frames);
}

} // namespace tdls
