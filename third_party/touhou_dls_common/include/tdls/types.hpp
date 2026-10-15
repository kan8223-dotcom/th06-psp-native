// touhou_dls_common: 機種に依存しない土台（型、バイト順、非整列安全な読み書き）。
//
// 方針:
//  - gm.dls はリトルエンディアン、PS3 の PPU/SPU はビッグエンディアン。
//  - RIFF のチャンクは 2 バイト境界なので 4 バイト欄はしばしば非整列。
//    PSP 実機は非整列 lw で固まる（memory: psp-misaligned-load-freeze）。
//  => 多バイト欄は必ず rd16le/rd32le でバイト単位に読む。構造体キャスト禁止。
//  - 例外・RTTI・iostream・filesystem・STL を共通部品では使わない。
#ifndef TDLS_TYPES_HPP
#define TDLS_TYPES_HPP

#include <stddef.h>
#include <stdint.h>

namespace tdls {

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

inline u16 rd16le(const u8 *p) { return (u16)((u16)p[0] | (u16)((u16)p[1] << 8)); }
inline u32 rd32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
inline s16 rds16le(const u8 *p) { return (s16)rd16le(p); }
inline s32 rds32le(const u8 *p) { return (s32)rd32le(p); }
inline u32 rd32be(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
inline u16 rd16be(const u8 *p) { return (u16)(((u16)p[0] << 8) | (u16)p[1]); }

inline void wr16le(u8 *p, u16 v)
{
    p[0] = (u8)(v & 0xffu);
    p[1] = (u8)(v >> 8);
}
inline void wr32le(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xffu);
    p[1] = (u8)((v >> 8) & 0xffu);
    p[2] = (u8)((v >> 16) & 0xffu);
    p[3] = (u8)(v >> 24);
}
inline void wr16be(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v & 0xffu);
}
inline void wr32be(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)((v >> 16) & 0xffu);
    p[2] = (u8)((v >> 8) & 0xffu);
    p[3] = (u8)(v & 0xffu);
}

// RIFF の FourCC を、ファイル上の 4 バイトを rd32le で読んだ値として作る。
inline u32 fourcc(char a, char b, char c, char d)
{
    return (u32)(u8)a | ((u32)(u8)b << 8) | ((u32)(u8)c << 16) | ((u32)(u8)d << 24);
}

// 呼び出し側が用意したメモリから線形に切り出す（解放は全体ごと）。
// 機種側でヒープの断片化を避けるため、共通部品は malloc を呼ばない。
struct Arena
{
    u8 *base;
    u32 capacity;
    u32 used;
    u32 peak;

    void init(void *mem, u32 cap)
    {
        base = (u8 *)mem;
        capacity = cap;
        used = 0;
        peak = 0;
    }
    void *alloc(u32 bytes, u32 align)
    {
        u32 a = align ? align : 1;
        u32 start = (used + (a - 1)) & ~(a - 1);
        if (start < used || start > capacity || bytes > capacity - start)
        {
            return 0;
        }
        used = start + bytes;
        if (used > peak)
        {
            peak = used;
        }
        return base + start;
    }
};

} // namespace tdls

#endif
