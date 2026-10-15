// 使用音色（UsageSet）と DLS から「使う楽器・リージョン・wave」を選び、
// 合成器向けの詰めた形（ブロブ）に変換する。
//
// 対応づけ規則の候補（MSGS が実際にどれかは未確認。MSGS の録音と比べて後で決める）:
//   MAP_A_CAPITAL    : LSB は無視。(MSB, Prog) が無ければ (0, Prog)（GS のキャピタル音色へ）
//   MAP_B_SUBCAPITAL : LSB は無視。(MSB, Prog) → (MSB を 8 の倍数に切り下げ, Prog) → (0, Prog)
//   MAP_C_STRICT_LSB : (MSB, LSB, Prog) の完全一致、無ければ (0, 0, Prog)
//                      （gm.dls は LSB が全部 0 なので、CC32=3 の曲では全部キャピタル）
// リズムパート（3 規則とも同じ）: バンクは無視して Program = キット番号で完全一致、
//   無ければキット 0（Standard）。
#ifndef TDLS_SUBSET_HPP
#define TDLS_SUBSET_HPP

#include "dls.hpp"
#include "smf_scan.hpp"
#include "types.hpp"

namespace tdls {

enum MapRule
{
    MAP_A_CAPITAL = 0,
    MAP_B_SUBCAPITAL = 1,
    MAP_C_STRICT_LSB = 2,
    MAP_RULE_COUNT = 3,
};

enum MapHow
{
    HOW_EXACT = 0,      // (MSB, LSB, Prog) 完全一致
    HOW_IGNORE_LSB,     // LSB を無視して (MSB, Prog) 一致
    HOW_SUBCAPITAL,     // MSB を 8 の倍数に切り下げて一致
    HOW_CAPITAL,        // (0, 0, Prog)
    HOW_KIT_EXACT,      // リズム: キット完全一致
    HOW_KIT_STANDARD,   // リズム: キット 0 へ
    HOW_NONE,           // 見つからない（無音）
};

const char *mapRuleName(MapRule r);
const char *mapHowName(MapHow h);

// 1 音色を楽器へ。戻り値は DlsFile::instruments の添字、無ければ -1。
s32 mapTone(const DlsFile &f, const ToneKey &k, MapRule rule, MapHow *how);

struct SubsetPlan
{
    MapRule rule;
    u32 toneCount;
    s32 *toneInstrument;  // [toneCount] UsageSet の並び順
    u8 *toneHow;          // [toneCount] MapHow
    u32 *toneUncovered;   // [toneCount*4] 鳴るのにどのリージョンにも入らないノート（ビット）
    u8 *insUsed;          // [instrumentCount]
    u8 *rgnUsed;          // [regionCount]
    u8 *waveUsed;         // [waveCount]
    u32 *waveKeepSamples; // [waveCount] ループ後ろを切るときに残すサンプル数（ワンショットは全長）
    u32 instruments, regions, waves;
    u32 uncoveredNotes;   // 上のビットの総数
    u32 unmappedTones;
};

// arena から配列を切り出して計画を作る。
bool subsetPlan(const DlsFile &f, const UsageSet &u, MapRule rule, Arena &arena, SubsetPlan &out);

// 形式変換（後で合成器が使う）。
enum BlobFlags
{
    BLOB_PCM8 = 1,        // 16bit を 8bit（符号付き、四捨五入・飽和）へ
    BLOB_BIG_ENDIAN = 2,  // 表と PCM をビッグエンディアンで（PS3 PPU/SPU 向け）
    BLOB_TRIM_LOOP = 4,   // 非可逆の実験形式。MSGS は消音直前にループを解除するため製品では使わない。
    BLOB_MSGS_GUARD = 8,  // 各 PCM の samples 直後に MSGS 補間用の標本を 1 個持つ
};

const u32 BLOB_HEADER_BYTES = 64;
const u32 BLOB_MAP_BYTES = 8;
const u32 BLOB_INS_BYTES = 16;
const u32 BLOB_RGN_BYTES = 40;
const u32 BLOB_WAVE_BYTES = 16;
const u32 BLOB_CONN_BYTES = 12;
const u32 BLOB_PCM_ALIGN = 64;

// ブロブの大きさ（書く前に確保量を知るため）。新規作成では BLOB_MSGS_GUARD を自動付与する。
u32 subsetBlobSize(const DlsFile &f, const UsageSet &u, const SubsetPlan &p, u32 flags);

// ブロブを書く。PCM は src（gm.dls の読み口）から読む。dlsSha256 は gm.dls の SHA-256。
// 呼出し側の flags=0/1/2/4 は従来どおり。実際の flags は flags|BLOB_MSGS_GUARD。
bool subsetWriteBlob(ByteSource &src, const DlsFile &f, const UsageSet &u, const SubsetPlan &p, u32 flags,
                     const u8 dlsSha256[32], u8 *out, u32 cap, u32 *written);

} // namespace tdls

#endif
