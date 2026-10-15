#include "tdls/subset.hpp"

#include <string.h>

namespace tdls {

namespace {

s32 findMelodic(const DlsFile &f, u8 msb, u8 lsb, u8 prog)
{
    return dlsFindInstrument(f, false, msb, lsb, prog);
}

// リズム: バンクは見ない。同じキット番号が複数バンクにあれば (0,0) を優先、次にファイル順で最初。
s32 findKit(const DlsFile &f, u8 prog)
{
    s32 first = -1;
    for (u32 i = 0; i < f.instrumentCount; i++)
    {
        const DlsInstrument &ins = f.instruments[i];
        if (!ins.isDrum() || ins.prog() != prog)
        {
            continue;
        }
        if (ins.bankMsb() == 0 && ins.bankLsb() == 0)
        {
            return (s32)i;
        }
        if (first < 0)
        {
            first = (s32)i;
        }
    }
    return first;
}

u32 waveTotalSamples(const DlsWave &w)
{
    u32 align = w.blockAlign ? w.blockAlign : 1;
    return w.dataBytes / align;
}

struct Put
{
    u8 *p;
    bool be;
    void u8at(u32 off, u8 v) { p[off] = v; }
    void u16at(u32 off, u16 v)
    {
        if (be) wr16be(p + off, v);
        else wr16le(p + off, v);
    }
    void u32at(u32 off, u32 v)
    {
        if (be) wr32be(p + off, v);
        else wr32le(p + off, v);
    }
};

u32 alignUp(u32 v, u32 a) { return (v + (a - 1)) & ~(a - 1); }

struct Layout
{
    u32 mapOff, insOff, rgnOff, waveOff, connOff, pcmOff, pcmBytes, total;
    u32 nMap, nIns, nRgn, nWave, nConn;
};

u32 pcmBytesFor(const DlsFile &f, const SubsetPlan &p, u32 w, u32 flags)
{
    u32 samples = (flags & BLOB_TRIM_LOOP) ? p.waveKeepSamples[w] : waveTotalSamples(f.waves[w]);
    return (samples + ((flags & BLOB_MSGS_GUARD) ? 1u : 0u)) * ((flags & BLOB_PCM8) ? 1u : 2u);
}

// CopyFromWave writes a shared wave's guard for every downloaded region, before
// Verify changes short/invalid loops. Include regions omitted from the subset.
u32 guardSourceSample(const DlsFile &f, u32 wave)
{
    const u32 total = waveTotalSamples(f.waves[wave]);
    u32 sample = 0xffffffffu; // Zero until a loop copies a source sample here.
    for (u32 i = 0; i < f.instrumentCount; ++i)
    {
        const DlsInstrument &ins = f.instruments[i];
        for (u32 k = 0; k < ins.regionCount; ++k)
        {
            const DlsRegion &r = f.regions[ins.firstRegion + k];
            if (r.waveIndex != wave) continue;
            const DlsWsmp *ws = dlsEffectiveWsmp(f, r);
            if (!ws || ws->loopCount == 0)
            {
                sample = 0xffffffffu;
                continue;
            }
            u32 start = ws->loop.start;
            if (start > total) start = 0; // Reference compares against wave.len=N+1.
            if (start < total) sample = start;
            // start==N copies the previous guard to itself; keep its value.
        }
    }
    return sample;
}

void computeLayout(const DlsFile &f, const UsageSet &u, const SubsetPlan &p, u32 flags, Layout &L)
{
    memset(&L, 0, sizeof(L));
    L.nMap = u.count;
    for (u32 i = 0; i < f.instrumentCount; i++)
    {
        if (!p.insUsed[i])
        {
            continue;
        }
        L.nIns++;
        const DlsInstrument &ins = f.instruments[i];
        if (ins.hasArt)
        {
            L.nConn += ins.art.connCount;
        }
        for (u32 k = 0; k < ins.regionCount; k++)
        {
            u32 r = ins.firstRegion + k;
            if (p.rgnUsed[r])
            {
                L.nRgn++;
                if (f.regions[r].hasArt)
                {
                    L.nConn += f.regions[r].art.connCount;
                }
            }
        }
    }
    for (u32 w = 0; w < f.waveCount; w++)
    {
        if (p.waveUsed[w])
        {
            L.nWave++;
            L.pcmBytes = alignUp(L.pcmBytes, BLOB_PCM_ALIGN) + pcmBytesFor(f, p, w, flags);
        }
    }
    L.mapOff = BLOB_HEADER_BYTES;
    L.insOff = L.mapOff + L.nMap * BLOB_MAP_BYTES;
    L.rgnOff = L.insOff + L.nIns * BLOB_INS_BYTES;
    L.waveOff = L.rgnOff + L.nRgn * BLOB_RGN_BYTES;
    L.connOff = L.waveOff + L.nWave * BLOB_WAVE_BYTES;
    L.pcmOff = alignUp(L.connOff + L.nConn * BLOB_CONN_BYTES, BLOB_PCM_ALIGN);
    L.total = L.pcmOff + L.pcmBytes;
}

s16 rd16src(const u8 *b) { return rds16le(b); }

s8 to8(s32 v16)
{
    s32 v = v16 + 128;
    s32 q = v >= 0 ? v / 256 : -((-v + 255) / 256); // 床関数の割り算（負数の右シフトに頼らない）
    if (q > 127) q = 127;
    if (q < -128) q = -128;
    return (s8)q;
}

} // namespace

const char *mapRuleName(MapRule r)
{
    switch (r)
    {
    case MAP_A_CAPITAL: return "A_capital";
    case MAP_B_SUBCAPITAL: return "B_subcapital";
    case MAP_C_STRICT_LSB: return "C_strict_lsb";
    case MAP_RULE_COUNT: break;
    }
    return "?";
}

const char *mapHowName(MapHow h)
{
    switch (h)
    {
    case HOW_EXACT: return "exact";
    case HOW_IGNORE_LSB: return "ignore_lsb";
    case HOW_SUBCAPITAL: return "subcapital";
    case HOW_CAPITAL: return "capital";
    case HOW_KIT_EXACT: return "kit_exact";
    case HOW_KIT_STANDARD: return "kit_standard";
    case HOW_NONE: return "none";
    }
    return "?";
}

s32 mapTone(const DlsFile &f, const ToneKey &k, MapRule rule, MapHow *how)
{
    s32 i;
    if (k.drum)
    {
        i = findKit(f, k.program);
        if (i >= 0)
        {
            *how = HOW_KIT_EXACT;
            return i;
        }
        i = findKit(f, 0);
        *how = i >= 0 ? HOW_KIT_STANDARD : HOW_NONE;
        return i;
    }
    i = findMelodic(f, k.msb, k.lsb, k.program);
    if (i >= 0)
    {
        *how = HOW_EXACT;
        return i;
    }
    if (rule == MAP_A_CAPITAL || rule == MAP_B_SUBCAPITAL)
    {
        i = findMelodic(f, k.msb, 0, k.program);
        if (i >= 0)
        {
            *how = HOW_IGNORE_LSB;
            return i;
        }
        u8 sub = (u8)(k.msb & ~7u);
        if (rule == MAP_B_SUBCAPITAL && sub != 0 && sub != k.msb)
        {
            i = findMelodic(f, sub, 0, k.program);
            if (i >= 0)
            {
                *how = HOW_SUBCAPITAL;
                return i;
            }
        }
    }
    i = findMelodic(f, 0, 0, k.program);
    *how = i >= 0 ? HOW_CAPITAL : HOW_NONE;
    return i;
}

bool subsetPlan(const DlsFile &f, const UsageSet &u, MapRule rule, Arena &arena, SubsetPlan &p)
{
    memset(&p, 0, sizeof(p));
    p.rule = rule;
    p.toneCount = u.count;
    p.toneInstrument = (s32 *)arena.alloc(u.count * 4u, 4);
    p.toneHow = (u8 *)arena.alloc(u.count, 1);
    p.toneUncovered = (u32 *)arena.alloc(u.count * 16u, 4);
    p.insUsed = (u8 *)arena.alloc(f.instrumentCount, 1);
    p.rgnUsed = (u8 *)arena.alloc(f.regionCount, 1);
    p.waveUsed = (u8 *)arena.alloc(f.waveCount, 1);
    p.waveKeepSamples = (u32 *)arena.alloc(f.waveCount * 4u, 4);
    if (!p.toneInstrument || !p.toneHow || !p.toneUncovered || !p.insUsed || !p.rgnUsed || !p.waveUsed ||
        !p.waveKeepSamples)
    {
        return false;
    }
    memset(p.toneUncovered, 0, u.count * 16u);
    memset(p.insUsed, 0, f.instrumentCount);
    memset(p.rgnUsed, 0, f.regionCount);
    memset(p.waveUsed, 0, f.waveCount);
    memset(p.waveKeepSamples, 0, f.waveCount * 4u);

    for (u32 t = 0; t < u.count; t++)
    {
        const ToneUse &tu = u.tones[t];
        MapHow how = HOW_NONE;
        s32 i = mapTone(f, tu.key, rule, &how);
        p.toneInstrument[t] = i;
        p.toneHow[t] = (u8)how;
        u32 covered[4] = {0, 0, 0, 0};
        if (i >= 0)
        {
            p.insUsed[i] = 1;
            const DlsInstrument &ins = f.instruments[i];
            for (u32 k = 0; k < ins.regionCount; k++)
            {
                u32 r = ins.firstRegion + k;
                const DlsRegion &rg = f.regions[r];
                bool sel = false;
                u32 hi = rg.keyHi > 127 ? 127u : rg.keyHi;
                for (u32 n = rg.keyLo; n <= hi; n++)
                {
                    if (tu.hasNote(n) && tu.velMin[n] <= rg.velHi && tu.velMax[n] >= rg.velLo)
                    {
                        sel = true;
                        covered[n >> 5] |= 1u << (n & 31u);
                    }
                }
                if (!sel)
                {
                    continue;
                }
                p.rgnUsed[r] = 1;
                if (rg.waveIndex >= f.waveCount)
                {
                    continue;
                }
                const DlsWave &w = f.waves[rg.waveIndex];
                p.waveUsed[rg.waveIndex] = 1;
                u32 total = waveTotalSamples(w);
                u32 keep = total;
                const DlsWsmp *ws = dlsEffectiveWsmp(f, rg);
                if (ws && ws->loopCount > 0)
                {
                    u32 endS = ws->loop.start + ws->loop.length;
                    if (endS >= ws->loop.start && endS < total)
                    {
                        keep = endS;
                    }
                }
                if (keep > p.waveKeepSamples[rg.waveIndex])
                {
                    p.waveKeepSamples[rg.waveIndex] = keep;
                }
            }
        }
        else
        {
            p.unmappedTones++;
        }
        for (u32 q = 0; q < 4; q++)
        {
            u32 unc = tu.noteBits[q] & ~covered[q];
            p.toneUncovered[t * 4 + q] = unc;
            for (; unc; unc &= unc - 1)
            {
                p.uncoveredNotes++;
            }
        }
    }
    for (u32 i = 0; i < f.instrumentCount; i++) p.instruments += p.insUsed[i];
    for (u32 r = 0; r < f.regionCount; r++) p.regions += p.rgnUsed[r];
    for (u32 w = 0; w < f.waveCount; w++) p.waves += p.waveUsed[w];
    return true;
}

u32 subsetBlobSize(const DlsFile &f, const UsageSet &u, const SubsetPlan &p, u32 flags)
{
    Layout L;
    computeLayout(f, u, p, flags | BLOB_MSGS_GUARD, L);
    return L.total;
}

bool subsetWriteBlob(ByteSource &src, const DlsFile &f, const UsageSet &u, const SubsetPlan &p, u32 flags,
                     const u8 dlsSha256[32], u8 *out, u32 cap, u32 *written)
{
    flags |= BLOB_MSGS_GUARD;
    Layout L;
    computeLayout(f, u, p, flags, L);
    *written = 0;
    if (cap < L.total)
    {
        return false;
    }
    memset(out, 0, L.total);
    Put o;
    o.p = out;
    o.be = (flags & BLOB_BIG_ENDIAN) != 0;
    // 見出し
    out[0] = 'T';
    out[1] = 'D';
    out[2] = 'S';
    out[3] = 'B';
    o.u16at(4, 1);
    o.u16at(6, (u16)flags);
    o.u32at(8, L.nMap);
    o.u32at(12, L.nIns);
    o.u32at(16, L.nRgn);
    o.u32at(20, L.nWave);
    o.u32at(24, L.nConn);
    o.u32at(28, L.mapOff);
    o.u32at(32, L.insOff);
    o.u32at(36, L.rgnOff);
    o.u32at(40, L.waveOff);
    o.u32at(44, L.connOff);
    o.u32at(48, L.pcmOff);
    o.u32at(52, L.pcmBytes);
    memcpy(out + 56, dlsSha256, 8); // 元の gm.dls を取り違えないための目印（先頭 8 バイト）

    // DLS の添字 -> ブロブの添字
    // （配列を持たずに数え直す。数百件なので二重ループで足りる）
    u32 nIns = 0, nRgn = 0, nConn = 0;
    for (u32 i = 0; i < f.instrumentCount; i++)
    {
        if (!p.insUsed[i])
        {
            continue;
        }
        const DlsInstrument &ins = f.instruments[i];
        u32 io = L.insOff + nIns * BLOB_INS_BYTES;
        o.u8at(io + 0, ins.isDrum() ? 1 : 0);
        o.u8at(io + 1, ins.bankMsb());
        o.u8at(io + 2, ins.bankLsb());
        o.u8at(io + 3, ins.prog());
        o.u32at(io + 4, nRgn);
        u32 artFirst = nConn;
        u32 artCount = ins.hasArt ? ins.art.connCount : 0u;
        for (u32 k = 0; k < artCount; k++)
        {
            const DlsConnection &c = f.conns[ins.art.firstConn + k];
            u32 co = L.connOff + nConn * BLOB_CONN_BYTES;
            o.u16at(co, c.source);
            o.u16at(co + 2, c.control);
            o.u16at(co + 4, c.destination);
            o.u16at(co + 6, c.transform);
            o.u32at(co + 8, (u32)c.scale);
            nConn++;
        }
        u32 rgnCount = 0;
        for (u32 k = 0; k < ins.regionCount; k++)
        {
            u32 r = ins.firstRegion + k;
            if (!p.rgnUsed[r])
            {
                continue;
            }
            const DlsRegion &rg = f.regions[r];
            u32 ro = L.rgnOff + nRgn * BLOB_RGN_BYTES;
            o.u8at(ro + 0, (u8)(rg.keyLo > 127 ? 127 : rg.keyLo));
            o.u8at(ro + 1, (u8)(rg.keyHi > 127 ? 127 : rg.keyHi));
            o.u8at(ro + 2, (u8)(rg.velLo > 127 ? 127 : rg.velLo));
            o.u8at(ro + 3, (u8)(rg.velHi > 127 ? 127 : rg.velHi));
            o.u16at(ro + 4, rg.keyGroup);
            o.u16at(ro + 6, rg.options);
            // wave のブロブ添字 = 自分より前の使用 wave の数
            u32 wb = 0xffffffffu;
            if (rg.waveIndex < f.waveCount && p.waveUsed[rg.waveIndex])
            {
                wb = 0;
                for (u32 w = 0; w < rg.waveIndex; w++) wb += p.waveUsed[w];
            }
            o.u32at(ro + 8, wb);
            const DlsWsmp *ws = dlsEffectiveWsmp(f, rg);
            u16 rflags = 0;
            if (ws)
            {
                o.u16at(ro + 12, ws->unityNote);
                o.u16at(ro + 14, (u16)ws->fineTune);
                o.u32at(ro + 16, (u32)ws->attenuation);
                if (ws->loopCount > 0)
                {
                    o.u32at(ro + 20, ws->loop.type);
                    o.u32at(ro + 24, ws->loop.start);
                    o.u32at(ro + 28, ws->loop.length);
                    rflags |= 1;
                }
            }
            u32 rArtCount = rg.hasArt ? rg.art.connCount : 0u;
            o.u16at(ro + 32, (u16)rArtCount);
            o.u16at(ro + 34, rflags);
            o.u32at(ro + 36, rArtCount ? nConn : 0u);
            for (u32 k2 = 0; k2 < rArtCount; k2++)
            {
                const DlsConnection &c = f.conns[rg.art.firstConn + k2];
                u32 co = L.connOff + nConn * BLOB_CONN_BYTES;
                o.u16at(co, c.source);
                o.u16at(co + 2, c.control);
                o.u16at(co + 4, c.destination);
                o.u16at(co + 6, c.transform);
                o.u32at(co + 8, (u32)c.scale);
                nConn++;
            }
            nRgn++;
            rgnCount++;
        }
        o.u16at(io + 8, (u16)rgnCount);
        o.u16at(io + 10, (u16)artCount);
        o.u32at(io + 12, artCount ? artFirst : 0u);
        nIns++;
    }

    // 対応表（UsageSet の順）
    for (u32 t = 0; t < u.count; t++)
    {
        u32 mo = L.mapOff + t * BLOB_MAP_BYTES;
        const ToneKey &k = u.tones[t].key;
        o.u8at(mo + 0, k.drum);
        o.u8at(mo + 1, k.msb);
        o.u8at(mo + 2, k.lsb);
        o.u8at(mo + 3, k.program);
        u32 bi = 0xffffffffu;
        s32 di = p.toneInstrument[t];
        if (di >= 0)
        {
            bi = 0;
            for (s32 i = 0; i < di; i++) bi += p.insUsed[i];
        }
        o.u32at(mo + 4, bi);
    }

    // wave と PCM
    u32 nWave = 0;
    u32 pcmPos = 0;
    u8 buf[1024];
    for (u32 w = 0; w < f.waveCount; w++)
    {
        if (!p.waveUsed[w])
        {
            continue;
        }
        const DlsWave &wv = f.waves[w];
        if (wv.channels != 1 || (wv.bitsPerSample != 16 && wv.bitsPerSample != 8) || wv.formatTag != 1)
        {
            return false; // gm.dls は全部 PCM モノラル 16bit。違う形は今は扱わない
        }
        pcmPos = alignUp(pcmPos, BLOB_PCM_ALIGN);
        u32 samples = (flags & BLOB_TRIM_LOOP) ? p.waveKeepSamples[w] : waveTotalSamples(wv);
        const u32 originalSamples = waveTotalSamples(wv);
        if (samples > originalSamples) return false;
        u32 wo = L.waveOff + nWave * BLOB_WAVE_BYTES;
        o.u32at(wo, pcmPos);
        o.u32at(wo + 4, samples);
        o.u32at(wo + 8, wv.sampleRate);
        o.u16at(wo + 12, (u16)((flags & BLOB_PCM8) ? 8 : 16));
        o.u16at(wo + 14, (u16)w);
        u32 srcBps = wv.bitsPerSample / 8u;
        u32 done = 0;
        while (done < samples)
        {
            u32 chunk = (u32)sizeof(buf) / srcBps;
            if (chunk > samples - done) chunk = samples - done;
            if (!src.read(wv.dataOffset + done * srcBps, buf, chunk * srcBps))
            {
                return false;
            }
            for (u32 s = 0; s < chunk; s++)
            {
                s32 v16 = srcBps == 2 ? (s32)rd16src(buf + s * 2) : ((s32)buf[s] - 128) * 256;
                u32 dst = L.pcmOff + pcmPos + (done + s) * ((flags & BLOB_PCM8) ? 1u : 2u);
                if (flags & BLOB_PCM8)
                {
                    out[dst] = (u8)to8(v16);
                }
                else
                {
                    o.u16at(dst, (u16)(s16)v16);
                }
            }
            done += chunk;
        }
        // Trimming preserves the real next sample at the new boundary. At the
        // original end, use the final shared guard from full-DLS download order.
        const u32 guardSample = samples < originalSamples ? samples : guardSourceSample(f, w);
        s32 guard16 = 0;
        if (guardSample != 0xffffffffu)
        {
            if (!src.read(wv.dataOffset + guardSample * srcBps, buf, srcBps)) return false;
            guard16 = srcBps == 2 ? (s32)rd16src(buf) : ((s32)buf[0] - 128) * 256;
        }
        const u32 dst = L.pcmOff + pcmPos + samples * ((flags & BLOB_PCM8) ? 1u : 2u);
        if (flags & BLOB_PCM8) out[dst] = (u8)to8(guard16);
        else o.u16at(dst, (u16)(s16)guard16);
        pcmPos += (samples + 1u) * ((flags & BLOB_PCM8) ? 1u : 2u);
        nWave++;
    }
    *written = L.total;
    return nIns == L.nIns && nRgn == L.nRgn && nConn == L.nConn && nWave == L.nWave && pcmPos == L.pcmBytes;
}

} // namespace tdls
