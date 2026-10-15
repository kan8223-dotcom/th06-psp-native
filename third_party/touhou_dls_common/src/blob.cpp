#include "tdls/blob.hpp"

#include <string.h>

namespace tdls {

const char *tdsbStatusName(TdsbStatus s)
{
    switch (s)
    {
    case TDSB_OK: return "OK";
    case TDSB_ERR_SIZE: return "SIZE";
    case TDSB_ERR_MAGIC: return "MAGIC";
    case TDSB_ERR_VERSION: return "VERSION";
    case TDSB_ERR_LAYOUT: return "LAYOUT";
    case TDSB_ERR_INDEX: return "INDEX";
    case TDSB_ERR_PCM: return "PCM";
    }
    return "?";
}

namespace {

// a + n*w が size を越えないか（掛け算・足し算の桁あふれも見る）
bool fits(u32 a, u32 n, u32 w, u32 size)
{
    if (a > size)
    {
        return false;
    }
    if (n != 0 && w > (size - a) / n)
    {
        return false;
    }
    return true;
}

} // namespace

TdsbStatus tdsbOpen(const void *data, u32 size, TdsbView &v)
{
    memset(&v, 0, sizeof(v));
    const u8 *p = (const u8 *)data;
    if (!p || size < 64)
    {
        return TDSB_ERR_SIZE;
    }
    if (p[0] != 'T' || p[1] != 'D' || p[2] != 'S' || p[3] != 'B')
    {
        return TDSB_ERR_MAGIC;
    }
    v.data = p;
    v.size = size;
    v.bigEndian = !(p[4] == 1 && p[5] == 0);
    v.version = v.r16(4);
    if (v.version != 1)
    {
        return TDSB_ERR_VERSION;
    }
    v.flags = v.r16(6);
    if (((v.flags & 2u) != 0) != v.bigEndian)
    {
        return TDSB_ERR_VERSION;
    }
    v.pcm8 = (v.flags & 1u) != 0;
    v.msgsGuard = (v.flags & 8u) != 0;
    v.nMap = v.r32(8);
    v.nIns = v.r32(12);
    v.nRgn = v.r32(16);
    v.nWave = v.r32(20);
    v.nConn = v.r32(24);
    v.mapOff = v.r32(28);
    v.insOff = v.r32(32);
    v.rgnOff = v.r32(36);
    v.waveOff = v.r32(40);
    v.connOff = v.r32(44);
    v.pcmOff = v.r32(48);
    v.pcmBytes = v.r32(52);
    memcpy(v.dlsSha8, p + 56, 8);

    if (!fits(v.mapOff, v.nMap, 8, size) || !fits(v.insOff, v.nIns, 16, size) || !fits(v.rgnOff, v.nRgn, 40, size) ||
        !fits(v.waveOff, v.nWave, 16, size) || !fits(v.connOff, v.nConn, 12, size) ||
        !fits(v.pcmOff, v.pcmBytes, 1, size))
    {
        return TDSB_ERR_LAYOUT;
    }
    if (v.mapOff < 64 || v.insOff < 64 || v.rgnOff < 64 || v.waveOff < 64 || v.connOff < 64 || v.pcmOff < 64)
    {
        return TDSB_ERR_LAYOUT;
    }
    // 対応表
    for (u32 i = 0; i < v.nMap; i++)
    {
        u32 ins = v.r32(v.mapAt(i) + 4);
        if (ins != TDSB_NONE && ins >= v.nIns)
        {
            return TDSB_ERR_INDEX;
        }
    }
    // 楽器
    for (u32 i = 0; i < v.nIns; i++)
    {
        u32 o = v.insAt(i);
        u32 fr = v.r32(o + 4);
        u32 nr = v.r16(o + 8);
        u32 na = v.r16(o + 10);
        u32 fa = v.r32(o + 12);
        if (fr > v.nRgn || nr > v.nRgn - fr || fa > v.nConn || na > v.nConn - fa)
        {
            return TDSB_ERR_INDEX;
        }
    }
    // リージョン
    for (u32 i = 0; i < v.nRgn; i++)
    {
        u32 o = v.rgnAt(i);
        u32 w = v.r32(o + 8);
        u32 na = v.r16(o + 32);
        u32 fa = v.r32(o + 36);
        if (w != TDSB_NONE && w >= v.nWave)
        {
            return TDSB_ERR_INDEX;
        }
        if (fa > v.nConn || na > v.nConn - fa)
        {
            return TDSB_ERR_INDEX;
        }
    }
    // wave
    u32 bps = v.pcm8 ? 1u : 2u;
    for (u32 i = 0; i < v.nWave; i++)
    {
        u32 o = v.waveAt(i);
        u32 pos = v.r32(o);
        u32 n = v.r32(o + 4);
        u32 bits = v.r16(o + 12);
        if (bits != bps * 8u)
        {
            return TDSB_ERR_PCM;
        }
        if ((v.msgsGuard && n == 0xffffffffu) ||
            !fits(pos, n + (v.msgsGuard ? 1u : 0u), bps, v.pcmBytes))
        {
            return TDSB_ERR_PCM;
        }
        if (!v.pcm8 && (pos & 1u) != 0)
        {
            return TDSB_ERR_PCM;
        }
    }
    return TDSB_OK;
}

} // namespace tdls
