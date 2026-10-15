// DLS（Downloadable Sounds）Level 1 の読み取り。Level 2 のチャンクは数えて報告するだけ。
//
// 読むもの（チャンク名はファイル上の FourCC）:
//   RIFF 'DLS ' { colh, vers, ptbl, LIST 'lins' { LIST 'ins ' {
//       insh, LIST 'lrgn' { LIST 'rgn ' { rgnh, wsmp, wlnk, LIST 'lart' { art1 } } },
//       LIST 'lart' { art1 }, LIST 'INFO' { INAM } } },
//     LIST 'wvpl' { LIST 'wave' { fmt , wsmp, data, LIST 'INFO' { INAM } } }, LIST 'INFO' }
// 欄の意味のうち、gm.dls の実データで確かめたもの以外は「未確認」として扱う（報告参照）。
#ifndef TDLS_DLS_HPP
#define TDLS_DLS_HPP

#include "byte_source.hpp"
#include "types.hpp"

namespace tdls {

enum DlsStatus
{
    DLS_OK = 0,
    DLS_ERR_READ,         // 読み口が範囲外/失敗
    DLS_ERR_NOT_DLS,      // RIFF 'DLS ' でない
    DLS_ERR_TRUNCATED,    // チャンクが親の範囲をはみ出す
    DLS_ERR_BAD_CHUNK,    // 必須欄が短い・矛盾
    DLS_ERR_NO_MEMORY,    // Arena 不足
    DLS_ERR_PTBL,         // ptbl の番地が wave に当たらない
};

const u32 DLS_BANK_DRUM_FLAG = 0x80000000u; // ulBank の bit31（F_INSTRUMENT_DRUMS）

struct DlsLoop
{
    u32 type;   // WLOOP_TYPE_FORWARD = 0
    u32 start;  // サンプル単位
    u32 length; // サンプル単位
};

struct DlsWsmp
{
    u16 unityNote;
    s16 fineTune;
    s32 attenuation; // 生値（単位は未確認の扱い）
    u32 options;
    u32 loopCount;   // cSampleLoops（0 ならワンショット）
    DlsLoop loop;    // 先頭の 1 本だけ保持（loopCount>1 は報告で数える）
};

struct DlsConnection
{
    u16 source;
    u16 control;
    u16 destination;
    u16 transform;
    s32 scale;
};

struct DlsArt
{
    u32 firstConn; // DlsFile::conns の添字
    u16 connCount;
    u16 chunkCount; // lart 内の art1 の数
};

struct DlsRegion
{
    u16 keyLo, keyHi;
    u16 velLo, velHi;
    u16 options;    // rgnh.fusOptions
    u16 keyGroup;   // rgnh.usKeyGroup
    u16 layer;      // rgnh が 14 バイトのとき（DLS2）だけ。無ければ 0
    u16 wlnkOptions;
    u16 phaseGroup;
    u32 channel;
    u32 tableIndex; // ptbl の添字
    u32 waveIndex;  // tableIndex を wvpl の並び順に直したもの（DlsFile::waves の添字）
    u8 hasWsmp;
    u8 hasWlnk;
    u8 hasArt;
    u8 pad_;
    DlsWsmp wsmp;
    DlsArt art;
};

struct DlsInstrument
{
    u32 bank;       // insh.Locale.ulBank 生値
    u32 program;    // insh.Locale.ulInstrument 生値
    u32 declaredRegions; // insh.cRegions
    u32 firstRegion;
    u32 regionCount;
    u32 listOffset; // LIST 'ins ' の位置
    u8 hasArt;
    u8 pad_[3];
    DlsArt art;
    char name[32];

    bool isDrum() const { return (bank & DLS_BANK_DRUM_FLAG) != 0; }
    u8 bankMsb() const { return (u8)((bank >> 8) & 0x7fu); }
    u8 bankLsb() const { return (u8)(bank & 0x7fu); }
    u8 prog() const { return (u8)(program & 0x7fu); }
};

struct DlsWave
{
    u32 listOffset; // LIST 'wave' の位置
    u32 dataOffset; // 'data' の中身の位置
    u32 dataBytes;
    u16 formatTag;
    u16 channels;
    u32 sampleRate;
    u16 blockAlign;
    u16 bitsPerSample;
    u8 hasWsmp;
    u8 pad_[3];
    DlsWsmp wsmp;
    char name[32];
};

struct DlsFile
{
    u32 fileSize;
    u32 riffSize;
    u32 colhCount;
    u8 hasVers;
    u8 pad_[3];
    u32 versMs, versLs;
    // ptbl
    u32 ptblCueCount;
    u32 *cueOffsets;   // ptbl の生値
    u32 *cueToWave;    // cue -> waves の添字（当たらなければ 0xffffffff）
    u32 wvplListOffset; // LIST 'wvpl' の位置
    u32 cueBase;        // cueOffsets の基準（wvpl の型 'wvpl' の直後）
    // 本体
    DlsInstrument *instruments;
    u32 instrumentCount;
    DlsRegion *regions;
    u32 regionCount;
    DlsWave *waves;
    u32 waveCount;
    DlsConnection *conns;
    u32 connCount;
    // 集計（Level 判定の証拠など）
    u32 level2Chunks;   // rgn2/lar2/art2/cdl /dlid の数
    u32 unknownChunks;  // 上記以外の知らないチャンク（例: msyn）
    u32 unknownTopIds[8];
    u32 unknownTopCount;
    u32 multiLoopWsmp;  // cSampleLoops > 1 の wsmp の数
    u32 regionsWithoutWsmp;
    u32 regionsWithoutWlnk;
    char infoName[64];
    char infoCopyright[128];
    // 失敗時
    u32 errorOffset;
};

// src 全体を解析する。arena から配列を切り出す（失敗しても arena は戻さない）。
DlsStatus dlsParse(ByteSource &src, Arena &arena, DlsFile &out);
const char *dlsStatusName(DlsStatus s);

// 楽器の探索（完全一致）。見つからなければ -1。
s32 dlsFindInstrument(const DlsFile &f, bool drum, u8 msb, u8 lsb, u8 program);

// 区間の効き: region の wsmp があればそれ、無ければ wave の wsmp。
const DlsWsmp *dlsEffectiveWsmp(const DlsFile &f, const DlsRegion &r);

} // namespace tdls

#endif
