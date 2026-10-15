// SMF（標準 MIDI ファイル）を走査して「鳴る音色とノート」を集める。合成はしない。
//
// 音色の決め方（GS/GM の作法。MSGS が同じかは未確認）:
//  - CC0（バンク MSB）と CC32（バンク LSB）は値を覚えるだけで、次の Program Change で確定する。
//    ノートの音色キー = 直前の Program Change で確定した (MSB, LSB, Program)。
//    Program Change 前のノートは電源投入時の (0,0,0) で数え、notesNoProgram に別計上。
//  - リズムパート: 既定は ch10（添字 9）。GS の DT1「40 1x 15 vv」(USE FOR RHYTHM PART) で切替。
//    パート→チャンネルは SC の慣例 x=0→ch10, x=1..9→ch1..9, x=A..F→ch11..16（未確認の扱い）。
//  - GS リセット（40 00 7F 00）、GM System On（7E xx 09 01）、SC-88 のモード設定（00 00 7F）で
//    全チャンネルを電源投入時の状態に戻す。
//  - GS DT1「40 1x 00 msb prog」(TONE NUMBER) があれば MSB と Program を確定させる。
//
// ループ（東方の BGM 作法。TH06/07/08 の MidiOutput と同じ）:
//  - CC2 = ループ点の記録、CC4 = ループ点へ戻る。CC4 より後ろの事象はゲームでは鳴らない。
//  - SMF_MODE_GAME: 1 周目を CC4 まで、2 周目をループ点から次の CC4 まで（1 周目の最後の
//    状態を引き継ぐ）走査する。状態は 2 周目の終わりで 1 周目の終わりと同じになるので 2 周で尽きる。
//  - SMF_MODE_LINEAR: ループを無視してファイル末尾まで（比較用）。
#ifndef TDLS_SMF_SCAN_HPP
#define TDLS_SMF_SCAN_HPP

#include "types.hpp"

namespace tdls {

enum SmfStatus
{
    SMF_OK = 0,
    SMF_ERR_HEADER,
    SMF_ERR_TRUNCATED,
    SMF_ERR_RUNNING_STATUS, // 走行状態なしでデータバイト
    SMF_ERR_BAD_STATUS,     // SMF に出てはいけない F1-F6/F8-FE
    SMF_ERR_TOO_MANY_TRACKS,
    SMF_ERR_TONE_OVERFLOW,  // UsageSet の容量不足
};

enum SmfMode
{
    SMF_MODE_GAME = 0,
    SMF_MODE_LINEAR = 1,
};

struct ToneKey
{
    u8 drum; // 1 = ノート時点でリズムパート
    u8 msb;
    u8 lsb;
    u8 program;
};

inline u32 toneKeyValue(const ToneKey &k)
{
    return ((u32)k.drum << 24) | ((u32)k.msb << 16) | ((u32)k.lsb << 8) | k.program;
}

struct ToneUse
{
    ToneKey key;
    u16 channelMask; // 鳴ったチャンネル（bit0 = ch1）
    u16 pad_;
    u32 fileMask;    // 鳴ったファイル（SmfScanOptions::fileIndex のビット、32 本まで）
    u32 noteCount;
    u32 noteBits[4]; // 鳴ったノート番号
    u8 noteLo, noteHi, velLo, velHi;
    u8 velMin[128];  // ノート番号ごとの最小/最大ベロシティ（noteBits が立っている所だけ有効）
    u8 velMax[128];

    bool hasNote(u32 n) const { return n < 128 && ((noteBits[n >> 5] >> (n & 31u)) & 1u) != 0; }
};

struct UsageStats
{
    u32 notes;            // 鳴らしたノート（ベロシティ>0 の note on）
    u32 notesNoProgram;   // Program Change 前に鳴ったノート
    u32 notesPendingBank; // CC0/CC32 の現在値が確定済みの値と違うまま鳴ったノート
    u32 gsReset, gmOn, sysModeSet;
    u32 rhythmPartSet;    // 40 1x 15
    u32 toneNumberSet;    // 40 1x 00
    u32 rxChannelSet;     // 40 1x 02
    u32 rxChannelNonIdentity; // 40 1x 02 でパートと違うチャンネルを指定した数（走査は恒等のまま）
    u32 gsChecksumErrors;
    u32 loopStarts, loopJumps;
    u32 loopRunningStatusMismatch; // ループ点の走行状態と CC4 の状態バイトが違った数
    u32 f7Events;
    u32 maxPolyphony;     // 同時に鳴っているノート数の最大（note off まで、全ファイル通しの最大）
};

struct UsageSet
{
    ToneUse *tones; // toneKeyValue の昇順に保つ（決まった順序で書き出すため）
    u32 capacity;
    u32 count;
    u8 overflow;
    u8 pad_[3];
    UsageStats stats;

    void init(ToneUse *storage, u32 cap);
    ToneUse *find(const ToneKey &k);
    const ToneUse *find(const ToneKey &k) const;
    ToneUse *findOrAdd(const ToneKey &k);
};

struct SmfChannelState
{
    u8 cc0, cc32;           // バンクの現在値
    u8 msb, lsb, program;   // 確定済み
    u8 programSet;          // 一度でも確定したか
    u8 rhythm;              // 0=通常, 1/2=リズム（GS MAP1/MAP2）
    u8 rpnMsb, rpnLsb, nrpnMsb, nrpnLsb, paramNrpn; // 最後に選ばれた (N)RPN
};

// 電源投入時の状態（全 ch: バンク 0、Program 0、ch10 だけリズム）。
void smfResetChannels(SmfChannelState st[16]);

// 合成器側で知りたい事象を受ける窓口（PC ツールの集計用。共通部品の結果には影響しない）。
class SmfObserver
{
  public:
    virtual ~SmfObserver() {}
    virtual void onPass(u32 pass) { (void)pass; } // 0=1 周目, 1=ループ 2 周目
    virtual void onNoteOn(u32 tick, u8 ch, u8 note, u8 vel, const ToneKey &key)
    {
        (void)tick; (void)ch; (void)note; (void)vel; (void)key;
    }
    virtual void onNoteOff(u32 tick, u8 ch, u8 note) { (void)tick; (void)ch; (void)note; }
    virtual void onControl(u32 tick, u8 ch, u8 cc, u8 value) { (void)tick; (void)ch; (void)cc; (void)value; }
    virtual void onProgram(u32 tick, u8 ch, u8 program) { (void)tick; (void)ch; (void)program; }
    // data は F0 の直後から（F7 を含む）。escape = F7 事象。
    virtual void onSysEx(u32 tick, const u8 *data, u32 len, bool escape)
    {
        (void)tick; (void)data; (void)len; (void)escape;
    }
    virtual void onMeta(u32 tick, u8 type, const u8 *data, u32 len) { (void)tick; (void)type; (void)data; (void)len; }
    // (N)RPN のデータエントリ（CC6）。
    virtual void onParam(u32 tick, u8 ch, bool nrpn, u8 msb, u8 lsb, u8 value)
    {
        (void)tick; (void)ch; (void)nrpn; (void)msb; (void)lsb; (void)value;
    }
};

struct SmfScanOptions
{
    SmfMode mode;
    u32 fileIndex; // UsageSet::fileMask のビット番号（0..31）
};

struct SmfInfo
{
    u16 format;
    u16 tracks;
    u16 division;
    u16 pad_;
    u32 lastTick;
    u32 errorOffset;
};

// state は呼ぶ前の音源の状態（init.mid の後など）を入れ、走査後の状態が返る。
SmfStatus smfScan(const u8 *data, u32 size, SmfChannelState state[16], UsageSet &usage,
                  const SmfScanOptions &opt, SmfObserver *obs, SmfInfo *info);
const char *smfStatusName(SmfStatus s);

// GS の DT1 なら番地（24bit）を返し、*dataBegin/*dataLen に値の範囲、*checksumOk に判定を入れる。
// data は F0 の直後から。GS DT1 でなければ false。
bool smfParseGsDt1(const u8 *data, u32 len, u32 *address, const u8 **dataBegin, u32 *dataLen, bool *checksumOk);

} // namespace tdls

#endif
