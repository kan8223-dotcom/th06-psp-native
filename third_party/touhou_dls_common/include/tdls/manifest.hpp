// 一覧（テキスト）の書き出し。PC と機種で同じ入力から同じバイト列・同じ SHA-256 を出すための決まり:
//  - 1 行 1 項目、LF、ASCII、整数だけ（浮動小数・ロケール・パスを入れない）。
//  - 行の順序は決まった順（音色は toneKeyValue の昇順、楽器・リージョン・wave は DLS の並び順）。
//  - 最後の行 "sha256 <64 桁>" は、それより前の全バイトの SHA-256。
//
// 使用音色一覧（tdls-usage 1）:
//   tdls-usage 1
//   work <作品名>
//   mode game|linear
//   init <名前> <バイト数> <sha256>        （init.mid を先に流すときだけ）
//   file <名前> <バイト数> <sha256>        （呼び出し側が渡した順。PC ツールは名前順）
//   stats notes=.. no_program=.. pending_bank=.. gs_reset=.. gm_on=.. sys_mode=.. rhythm_set=..
//         tone_number=.. rx_channel=.. rx_nonidentity=.. gs_checksum_err=.. loop_start=..
//         loop_jump=.. loop_rs_mismatch=.. f7=.. max_poly=..   （実際は 1 行）
//   tone d=<0|1> msb=.. lsb=.. prog=.. notes=.. key=<lo>-<hi> vel=<lo>-<hi> ch=<4 桁 16 進>
//        files=<8 桁 16 進> bits=<noteBits[0..3] を各 8 桁 16 進で>   （実際は 1 行）
//   end <音色数>
//   sha256 <...>
//
// サブセット一覧（tdls-subset 1）:
//   tdls-subset 1
//   dls <バイト数> <sha256>
//   usage <使用音色一覧の sha256>
//   rule <規則名>
//   map d=.. msb=.. lsb=.. prog=.. -> ins=<DLS 添字|-1> how=<MapHow 名>
//   uncovered d=.. msb=.. lsb=.. prog=.. bits=<..>       （鳴るのにリージョンが無いノート）
//   ins <DLS 添字> d=.. msb=.. lsb=.. prog=.. regions=<選んだ数>/<全数>
//   rgn <DLS 添字> key=<lo>-<hi> wave=<DLS 添字>
//   wave <DLS 添字> bytes=<元の data バイト数> rate=.. bits=.. samples=.. keep=<ループ後ろを切った時のサンプル数>
//   total ins=.. rgn=.. wave=.. pcm16=.. pcm8=.. pcm16_trim=.. pcm8_trim=..
//   blob flags=<BlobFlags> size=.. sha256=..           （呼び出し側が渡したものだけ）
//   sha256 <...>
#ifndef TDLS_MANIFEST_HPP
#define TDLS_MANIFEST_HPP

#include "dls.hpp"
#include "sha256.hpp"
#include "smf_scan.hpp"
#include "subset.hpp"
#include "types.hpp"

namespace tdls {

class TextSink
{
  public:
    virtual ~TextSink() {}
    virtual void put(const char *s, u32 n) = 0;
};

class ManifestWriter
{
  public:
    void begin(TextSink *sink); // sink は 0 でもよい（ハッシュだけ取る）
    void line(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void finish(char hexOut[65]); // "sha256 <hex>" 行を書いて hex も返す
    bool overflowed() const { return overflow_; }

  private:
    TextSink *sink_;
    Sha256 sha_;
    bool overflow_;
};

struct InputFile
{
    const char *name;
    u32 size;
    u8 sha256[32];
};

struct BlobInfo
{
    u32 flags;
    u32 size;
    u8 sha256[32];
};

void writeUsageList(ManifestWriter &w, const char *work, SmfMode mode, const InputFile *init,
                    const InputFile *files, u32 fileCount, const UsageSet &u);

void writeSubsetManifest(ManifestWriter &w, const DlsFile &f, const u8 dlsSha256[32], const char *usageSha256Hex,
                         const UsageSet &u, const SubsetPlan &p, const BlobInfo *blobs, u32 blobCount);

// 計画から PCM の合計（各 wave の 64 バイト揃えを含まない素の合計）を出す。
void subsetPcmTotals(const DlsFile &f, const SubsetPlan &p, u32 *pcm16, u32 *pcm8, u32 *pcm16Trim, u32 *pcm8Trim);

} // namespace tdls

#endif
