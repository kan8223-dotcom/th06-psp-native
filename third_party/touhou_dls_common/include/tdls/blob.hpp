// TDSB v1（subset.hpp の subsetWriteBlob が書くブロブ）の読み口。合成器はこれを通して読む。
//
// 形（多バイト欄は flags の BLOB_BIG_ENDIAN でエンディアンが決まる。見出しの version は
// LE なら 01 00、BE なら 00 01 なので先頭 6 バイト目で判別できる）:
//   +0  "TDSB"  +4 u16 version(=1)  +6 u16 flags
//   +8  nMap nIns nRgn nWave nConn（u32 x5）
//   +28 mapOff insOff rgnOff waveOff connOff pcmOff pcmBytes（u32 x7）  +56 gm.dls の SHA-256 先頭 8 バイト
//   map  8B : drum msb lsb prog, u32 ins（0xffffffff = 無し）       並びは toneKeyValue の昇順
//   ins 16B : drum msb lsb prog, u32 firstRgn, u16 nRgn, u16 nArt, u32 firstArt
//   rgn 40B : keyLo keyHi velLo velHi, u16 keyGroup, u16 options, u32 wave, u16 unity, s16 fine,
//             s32 atten, u32 loopType, u32 loopStart, u32 loopLen, u16 nArt, u16 rflags(bit0=ループ有), u32 firstArt
//   wave 16B: u32 pcmPos（pcmOff からの位置）, u32 samples, u32 rate, u16 bits, u16 dlsIndex
//   flags&8 のとき PCM は samples+1 個。最後の 1 個は補間用で、samples には含めない。
//   古い flags&8=0 のブロブも読み込める（補間用標本は持たない）。
//   conn 12B: u16 src, u16 ctl, u16 dst, u16 trn, s32 scale
// 読み出しは全部バイト単位（非整列でも安全）。tdsbOpen が範囲と添字を全部検査する。
#ifndef TDLS_BLOB_HPP
#define TDLS_BLOB_HPP

#include "types.hpp"

namespace tdls {

enum TdsbStatus
{
    TDSB_OK = 0,
    TDSB_ERR_SIZE,     // 見出しより短い
    TDSB_ERR_MAGIC,    // "TDSB" でない
    TDSB_ERR_VERSION,  // version != 1
    TDSB_ERR_LAYOUT,   // 表の位置・大きさが矛盾
    TDSB_ERR_INDEX,    // 添字が範囲外
    TDSB_ERR_PCM,      // wave の PCM が範囲外、ビット数が flags と違う
};

const char *tdsbStatusName(TdsbStatus s);

const u32 TDSB_NONE = 0xffffffffu;

struct TdsbView
{
    const u8 *data;
    u32 size;
    u16 version;
    u16 flags;
    bool bigEndian;
    bool pcm8;
    bool msgsGuard;
    u32 nMap, nIns, nRgn, nWave, nConn;
    u32 mapOff, insOff, rgnOff, waveOff, connOff, pcmOff, pcmBytes;
    u8 dlsSha8[8];

    u16 r16(u32 off) const { return bigEndian ? rd16be(data + off) : rd16le(data + off); }
    u32 r32(u32 off) const { return bigEndian ? rd32be(data + off) : rd32le(data + off); }
    s32 rs32(u32 off) const { return (s32)r32(off); }
    u8 r8(u32 off) const { return data[off]; }

    u32 mapAt(u32 i) const { return mapOff + i * 8u; }
    u32 insAt(u32 i) const { return insOff + i * 16u; }
    u32 rgnAt(u32 i) const { return rgnOff + i * 40u; }
    u32 waveAt(u32 i) const { return waveOff + i * 16u; }
    u32 connAt(u32 i) const { return connOff + i * 12u; }
};

// data の中身を検査して view を作る。data は呼び出し側が保持し続けること（合成器は PCM を直接読む）。
TdsbStatus tdsbOpen(const void *data, u32 size, TdsbView &out);

} // namespace tdls

#endif
