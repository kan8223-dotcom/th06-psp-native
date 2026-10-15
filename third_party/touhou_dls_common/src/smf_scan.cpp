#include "tdls/smf_scan.hpp"

#include <string.h>

namespace tdls {

namespace {

const u32 kMaxTracks = 32;

struct Track
{
    u32 pos;
    u32 end;
    u32 tick;
    u8 running;
    u8 done;
    u8 pad_[2];
};

bool readVlq(const u8 *d, u32 end, u32 &pos, u32 &out)
{
    u32 v = 0;
    for (u32 i = 0; i < 4; i++)
    {
        if (pos >= end)
        {
            return false;
        }
        u8 b = d[pos++];
        v = (v << 7) | (u32)(b & 0x7fu);
        if ((b & 0x80u) == 0)
        {
            out = v;
            return true;
        }
    }
    return false;
}

// Roland の番地は 1 バイト 7bit。a + i を 7bit 桁上がりで足す。
u32 gsAddrAdd(u32 a, u32 i)
{
    u32 b0 = (a & 0x7fu) + i;
    u32 b1 = ((a >> 8) & 0x7fu) + (b0 >> 7);
    u32 b2 = ((a >> 16) & 0x7fu) + (b1 >> 7);
    return ((b2 & 0x7fu) << 16) | ((b1 & 0x7fu) << 8) | (b0 & 0x7fu);
}

// GS のパート番地のブロック（40 1x yy の x）-> MIDI チャンネル添字（SC の慣例）
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

void resetAll(SmfChannelState st[16], UsageStats &s, u32 *counter)
{
    (void)s;
    smfResetChannels(st);
    (*counter)++;
}

void handleSysEx(const u8 *p, u32 len, SmfChannelState st[16], UsageStats &s)
{
    // GM System On: 7E dev 09 01 F7
    if (len >= 4 && p[0] == 0x7e && p[2] == 0x09 && p[3] == 0x01)
    {
        resetAll(st, s, &s.gmOn);
        return;
    }
    u32 addr;
    const u8 *d;
    u32 dn;
    bool ok;
    if (!smfParseGsDt1(p, len, &addr, &d, &dn, &ok))
    {
        return;
    }
    if (!ok)
    {
        s.gsChecksumErrors++;
    }
    if (addr == 0x40007fu)
    {
        resetAll(st, s, &s.gsReset);
        return;
    }
    if (addr == 0x00007fu)
    {
        resetAll(st, s, &s.sysModeSet);
        return;
    }
    if ((addr & 0xfff000u) == 0x401000u && (addr & 0xffu) == 0x00u && dn >= 2)
    {
        // TONE NUMBER: CC#0 の値と Program を確定させる
        u8 ch = gsBlockToChannel((addr >> 8) & 0xfu);
        st[ch].cc0 = d[0] & 0x7fu;
        st[ch].msb = d[0] & 0x7fu;
        st[ch].lsb = st[ch].cc32;
        st[ch].program = d[1] & 0x7fu;
        st[ch].programSet = 1;
        s.toneNumberSet++;
        return;
    }
    for (u32 i = 0; i < dn; i++)
    {
        u32 a = gsAddrAdd(addr, i);
        if ((a & 0xfff000u) != 0x401000u)
        {
            continue;
        }
        u32 block = (a >> 8) & 0xfu;
        u32 off = a & 0xffu;
        u8 ch = gsBlockToChannel(block);
        u8 v = d[i] & 0x7fu;
        if (off == 0x02u)
        {
            s.rxChannelSet++;
            if (v != ch)
            {
                s.rxChannelNonIdentity++;
            }
        }
        else if (off == 0x15u)
        {
            st[ch].rhythm = v;
            s.rhythmPartSet++;
        }
    }
}

} // namespace

void UsageSet::init(ToneUse *storage, u32 cap)
{
    tones = storage;
    capacity = cap;
    count = 0;
    overflow = 0;
    pad_[0] = pad_[1] = pad_[2] = 0;
    memset(&stats, 0, sizeof(stats));
}

const ToneUse *UsageSet::find(const ToneKey &k) const
{
    u32 key = toneKeyValue(k);
    u32 lo = 0, hi = count;
    while (lo < hi)
    {
        u32 mid = lo + (hi - lo) / 2;
        u32 mk = toneKeyValue(tones[mid].key);
        if (mk == key)
        {
            return &tones[mid];
        }
        if (mk < key)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return 0;
}

ToneUse *UsageSet::find(const ToneKey &k) { return const_cast<ToneUse *>(static_cast<const UsageSet *>(this)->find(k)); }

ToneUse *UsageSet::findOrAdd(const ToneKey &k)
{
    ToneUse *u = find(k);
    if (u)
    {
        return u;
    }
    if (count >= capacity)
    {
        overflow = 1;
        return 0;
    }
    u32 key = toneKeyValue(k);
    u32 at = count;
    while (at > 0 && toneKeyValue(tones[at - 1].key) > key)
    {
        tones[at] = tones[at - 1];
        at--;
    }
    ToneUse &n = tones[at];
    memset(&n, 0, sizeof(n));
    n.key = k;
    n.noteLo = 127;
    n.velLo = 127;
    count++;
    return &n;
}

void smfResetChannels(SmfChannelState st[16])
{
    for (u32 ch = 0; ch < 16; ch++)
    {
        SmfChannelState &c = st[ch];
        c.cc0 = c.cc32 = 0;
        c.msb = c.lsb = c.program = 0;
        c.programSet = 0;
        c.rhythm = ch == 9 ? 1 : 0;
        c.rpnMsb = c.rpnLsb = c.nrpnMsb = c.nrpnLsb = 127;
        c.paramNrpn = 0;
    }
}

bool smfParseGsDt1(const u8 *p, u32 len, u32 *address, const u8 **dataBegin, u32 *dataLen, bool *checksumOk)
{
    // 41 dev 42 12 a1 a2 a3 data... cs F7
    if (len < 9 || p[0] != 0x41 || p[2] != 0x42 || p[3] != 0x12 || p[len - 1] != 0xf7)
    {
        return false;
    }
    *address = ((u32)p[4] << 16) | ((u32)p[5] << 8) | p[6];
    *dataBegin = p + 7;
    *dataLen = len - 9;
    u32 sum = 0;
    for (u32 i = 4; i < len - 1; i++)
    {
        sum += p[i];
    }
    *checksumOk = (sum & 0x7fu) == 0;
    return true;
}

const char *smfStatusName(SmfStatus s)
{
    switch (s)
    {
    case SMF_OK: return "OK";
    case SMF_ERR_HEADER: return "HEADER";
    case SMF_ERR_TRUNCATED: return "TRUNCATED";
    case SMF_ERR_RUNNING_STATUS: return "RUNNING_STATUS";
    case SMF_ERR_BAD_STATUS: return "BAD_STATUS";
    case SMF_ERR_TOO_MANY_TRACKS: return "TOO_MANY_TRACKS";
    case SMF_ERR_TONE_OVERFLOW: return "TONE_OVERFLOW";
    }
    return "?";
}

SmfStatus smfScan(const u8 *data, u32 size, SmfChannelState st[16], UsageSet &usage, const SmfScanOptions &opt,
                  SmfObserver *obs, SmfInfo *info)
{
    SmfInfo localInfo;
    SmfInfo &inf = info ? *info : localInfo;
    memset(&inf, 0, sizeof(inf));
    UsageStats &s = usage.stats;
    if (size < 14 || memcmp(data, "MThd", 4) != 0)
    {
        return SMF_ERR_HEADER;
    }
    u32 hlen = rd32be(data + 4);
    if (hlen < 6 || hlen > size - 8)
    {
        return SMF_ERR_HEADER;
    }
    inf.format = rd16be(data + 8);
    inf.tracks = rd16be(data + 10);
    inf.division = rd16be(data + 12);
    if (inf.tracks > kMaxTracks)
    {
        return SMF_ERR_TOO_MANY_TRACKS;
    }
    Track tr[kMaxTracks];
    u32 n = 0;
    u32 pos = 8 + hlen;
    while (n < inf.tracks && size - pos >= 8)
    {
        u32 len = rd32be(data + pos + 4);
        if (len > size - pos - 8)
        {
            inf.errorOffset = pos;
            return SMF_ERR_TRUNCATED;
        }
        if (memcmp(data + pos, "MTrk", 4) == 0)
        {
            Track &t = tr[n++];
            t.pos = pos + 8;
            t.end = pos + 8 + len;
            t.tick = 0;
            t.running = 0;
            t.done = 0;
            t.pad_[0] = t.pad_[1] = 0;
            u32 dt;
            if (!readVlq(data, t.end, t.pos, dt))
            {
                t.done = 1;
            }
            else
            {
                t.tick = dt;
            }
        }
        pos += 8 + len;
    }
    if (n != inf.tracks)
    {
        inf.errorOffset = pos;
        return SMF_ERR_TRUNCATED;
    }

    Track startTr[kMaxTracks];
    Track loopTr[kMaxTracks];
    memcpy(startTr, tr, sizeof(Track) * n);
    bool loopValid = false;
    u32 pass = 0;
    if (obs)
    {
        obs->onPass(0);
    }
    u8 active[16][128];
    memset(active, 0, sizeof(active));
    u32 poly = 0;

    for (;;)
    {
        u32 sel = kMaxTracks;
        for (u32 i = 0; i < n; i++)
        {
            if (!tr[i].done && (sel == kMaxTracks || tr[i].tick < tr[sel].tick))
            {
                sel = i;
            }
        }
        if (sel == kMaxTracks)
        {
            break;
        }
        Track &t = tr[sel];
        if (t.pos >= t.end)
        {
            t.done = 1;
            continue;
        }
        u32 tick = t.tick;
        if (tick > inf.lastTick)
        {
            inf.lastTick = tick;
        }
        u8 status = data[t.pos];
        if (status < 0x80u)
        {
            if (!t.running)
            {
                inf.errorOffset = t.pos;
                return SMF_ERR_RUNNING_STATUS;
            }
            status = t.running;
        }
        else
        {
            t.pos++;
        }
        bool wantLoopStart = false, wantJump = false;
        if (status < 0xf0u)
        {
            u8 type = (u8)(status & 0xf0u);
            u8 ch = (u8)(status & 0x0fu);
            u32 nData = (type == 0xc0u || type == 0xd0u) ? 1u : 2u;
            if (t.end - t.pos < nData)
            {
                inf.errorOffset = t.pos;
                return SMF_ERR_TRUNCATED;
            }
            u8 d1 = data[t.pos];
            u8 d2 = nData == 2 ? data[t.pos + 1] : 0;
            if ((d1 | d2) & 0x80u)
            {
                inf.errorOffset = t.pos;
                return SMF_ERR_BAD_STATUS;
            }
            t.pos += nData;
            t.running = status;
            SmfChannelState &c = st[ch];
            if (type == 0x90u && d2 > 0)
            {
                ToneKey k;
                k.drum = c.rhythm ? 1 : 0;
                k.msb = c.msb;
                k.lsb = c.lsb;
                k.program = c.program;
                s.notes++;
                if (!c.programSet)
                {
                    s.notesNoProgram++;
                }
                if (c.cc0 != c.msb || c.cc32 != c.lsb)
                {
                    s.notesPendingBank++;
                }
                ToneUse *u = usage.findOrAdd(k);
                if (!u)
                {
                    inf.errorOffset = t.pos;
                    return SMF_ERR_TONE_OVERFLOW;
                }
                if (u->noteCount == 0 || d1 < u->noteLo) u->noteLo = d1;
                if (u->noteCount == 0 || d1 > u->noteHi) u->noteHi = d1;
                if (u->noteCount == 0 || d2 < u->velLo) u->velLo = d2;
                if (u->noteCount == 0 || d2 > u->velHi) u->velHi = d2;
                if (!u->hasNote(d1))
                {
                    u->noteBits[d1 >> 5] |= 1u << (d1 & 31u);
                    u->velMin[d1] = d2;
                    u->velMax[d1] = d2;
                }
                else
                {
                    if (d2 < u->velMin[d1]) u->velMin[d1] = d2;
                    if (d2 > u->velMax[d1]) u->velMax[d1] = d2;
                }
                u->noteCount++;
                u->channelMask = (u16)(u->channelMask | (1u << ch));
                if (opt.fileIndex < 32)
                {
                    u->fileMask |= 1u << opt.fileIndex;
                }
                if (active[ch][d1] < 255)
                {
                    active[ch][d1]++;
                }
                poly++;
                if (poly > s.maxPolyphony)
                {
                    s.maxPolyphony = poly;
                }
                if (obs)
                {
                    obs->onNoteOn(tick, ch, d1, d2, k);
                }
            }
            else if (type == 0x80u || type == 0x90u)
            {
                if (active[ch][d1] > 0)
                {
                    active[ch][d1]--;
                    poly--;
                }
                if (obs)
                {
                    obs->onNoteOff(tick, ch, d1);
                }
            }
            else if (type == 0xb0u)
            {
                switch (d1)
                {
                case 0: c.cc0 = d2; break;
                case 32: c.cc32 = d2; break;
                case 99: c.nrpnMsb = d2; c.paramNrpn = 1; break;
                case 98: c.nrpnLsb = d2; c.paramNrpn = 1; break;
                case 101: c.rpnMsb = d2; c.paramNrpn = 0; break;
                case 100: c.rpnLsb = d2; c.paramNrpn = 0; break;
                case 6:
                    if (obs)
                    {
                        obs->onParam(tick, ch, c.paramNrpn != 0, c.paramNrpn ? c.nrpnMsb : c.rpnMsb,
                                     c.paramNrpn ? c.nrpnLsb : c.rpnLsb, d2);
                    }
                    break;
                case 2: wantLoopStart = true; s.loopStarts++; break;
                case 4: wantJump = true; s.loopJumps++; break;
                default: break;
                }
                if (obs)
                {
                    obs->onControl(tick, ch, d1, d2);
                }
            }
            else if (type == 0xc0u)
            {
                c.msb = c.cc0;
                c.lsb = c.cc32;
                c.program = d1;
                c.programSet = 1;
                if (obs)
                {
                    obs->onProgram(tick, ch, d1);
                }
            }
        }
        else if (status == 0xf0u || status == 0xf7u)
        {
            u32 len;
            if (!readVlq(data, t.end, t.pos, len) || len > t.end - t.pos)
            {
                inf.errorOffset = t.pos;
                return SMF_ERR_TRUNCATED;
            }
            if (status == 0xf0u)
            {
                handleSysEx(data + t.pos, len, st, s);
            }
            else
            {
                s.f7Events++;
            }
            if (obs)
            {
                obs->onSysEx(tick, data + t.pos, len, status == 0xf7u);
            }
            t.pos += len;
            t.running = 0;
        }
        else if (status == 0xffu)
        {
            u32 len;
            if (t.pos >= t.end)
            {
                inf.errorOffset = t.pos;
                return SMF_ERR_TRUNCATED;
            }
            u8 mt = data[t.pos++];
            if (!readVlq(data, t.end, t.pos, len) || len > t.end - t.pos)
            {
                inf.errorOffset = t.pos;
                return SMF_ERR_TRUNCATED;
            }
            if (obs)
            {
                obs->onMeta(tick, mt, data + t.pos, len);
            }
            t.pos += len;
            t.running = 0;
            if (mt == 0x2fu)
            {
                t.done = 1;
            }
        }
        else
        {
            inf.errorOffset = t.pos - 1;
            return SMF_ERR_BAD_STATUS;
        }

        // 次の事象までの間隔を読む（ゲームも事象ごとに直後の間隔を読む）
        if (!t.done)
        {
            if (t.pos >= t.end)
            {
                t.done = 1;
            }
            else
            {
                u32 dt;
                if (!readVlq(data, t.end, t.pos, dt))
                {
                    inf.errorOffset = t.pos;
                    return SMF_ERR_TRUNCATED;
                }
                t.tick += dt;
            }
        }
        if (opt.mode != SMF_MODE_GAME)
        {
            continue;
        }
        if (wantLoopStart)
        {
            memcpy(loopTr, tr, sizeof(Track) * n);
            loopValid = true;
        }
        if (wantJump)
        {
            if (pass >= 1)
            {
                break; // 2 周目の終わり。以後は同じ状態の繰り返し
            }
            pass = 1;
            u8 jumpStatus = status;
            memcpy(tr, loopValid ? loopTr : startTr, sizeof(Track) * n);
            // 戻った先の最初の事象が走行状態に頼っていて、しかも状態が違うときだけ数える
            if (tr[sel].pos < tr[sel].end && data[tr[sel].pos] < 0x80u && tr[sel].running != jumpStatus)
            {
                s.loopRunningStatusMismatch++;
            }
            tr[sel].running = jumpStatus; // ゲームは CC4 の状態バイトを走行状態に残す
            if (obs)
            {
                obs->onPass(1);
            }
        }
    }
    return SMF_OK;
}

} // namespace tdls
