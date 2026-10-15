// DLS Level 1 の合成器（TDSB ブロブで鳴らす）。整数演算だけで書いてあり、浮動小数を使わない。
// 同じ入力なら PC/PSP/PS3 で出力がビット単位で一致することを狙う（未定義動作を避け、
// 負数の右シフトは GCC の算術シフトに頼る＝3 機種とも GCC）。
//
// 制御とミックスは利用者の wdmaud.drv x86 の振る舞いを整数で再実装したもの。
// 短い piano/square/drums の MSGS 基準 PCM と全標本一致を確認済み。
// 曲全体の PCM 一致は未達で、比較値と残差は TUNING_STATUS に記録する。
// 表は汎用の数式から生成する。gm.dls の波形・楽器パラメータはソースへ含めない。
//
// 主な単位: 音量 VREL = 1/100 dB（MS と同じ）、音程 = セント、時間 = 出力の標本数。
// 位相 Q12、音量 Q12、EG/LFO の折れ点と最長 ceil(rate/20) 標本で制御を更新。
// MSGS の MMX/scalar の丸め、音量・音程ランプ、声ごとの16bit飽和を再現する。
// ZunMidiPlayer の setEventQuantization(10,0) は1msでシーケンサを進め、事象を10msに量子化し、
// Render区間の未来の事象を先に投入する。kMaxBlockは内部scratch bufferの分割だけ。
//
// 対応している DLS1 の要素:
//  リージョン選択（鍵域のみ。DLS1 どおりベロシティ範囲は見ない）、キーグループ（同じ ch・同じ音色で排他）、
//  SELFNONEXCLUSIVE（無ければ同じ鍵の前の音を素早く止める）、wsmp（ループ 1 本、ユニティノート、
//  微調整は「足す」向き、減衰）、art1 の接続: EG1/EG2 の A/D/S/R、ベロシティ→アタック、鍵盤→ディケイ、
//  EG2→音程、LFO の周波数・遅延・音程・音量、CC1×LFO→音程・音量、既定パン。線形補間。
//  読まない接続: ベロシティ→音量（固定の 40log10 曲線を使う＝[MS]）、予約の 0x0002/0x0208/0x030c
//  （gm.dls ではそれぞれパン/EG1 サステイン/EG2 サステインと同じ値の生値だった）。
// MIDI: Note On/Off、Program Change（CC0/32 はここで確定）、CC1/6/7/10/11/38/64/91/93/94/98-101/120/121/
//  123/126/127、ピッチベンド、RPN 0/1/2、NRPN は保持だけ、GM System On・GS リセット、
//  GS Reset後のバンク選択と USE FOR RHYTHM PART、マスターボリューム。
//  CC64は0以外でON。SC-88モード設定とTONE NUMBERはMSGS同様に無視する。
// 未対応: リバーブ・コーラス本体（センド量は保持）、GS のその他の SysEx（エフェクト・EQ・スケールチューン・
//  受信チャンネル）、アフタータッチ、DLS2 の要素（フィルタ・DAHDSR など）。
//
// 使い方（機種側）:
//   TdsbView b; tdsbOpen(blobMem, blobSize, b);
//   SynthConfig c; synthDefaultConfig(c); c.voices = 40;
//   Arena a; a.init(mem, Synth::memoryNeeded(b, c)); Synth s; s.init(b, c, a);
//   ZunMidiPlayer player; player.init(&s, &seq, &queue); player.setEventQuantization(10,0);
#ifndef TDLS_SYNTH_HPP
#define TDLS_SYNTH_HPP

#include "blob.hpp"
#include "types.hpp"
#include "msgs_control.hpp"

namespace tdls {

struct SynthConfig
{
    u32 sampleRate;     // 合成の標本化周波数（既定 22050。8000〜96000）
    u32 voices;         // 発音スロットの総数（既定 40 = [MS] 32 + 予備 8）
    u32 reserveVoices;  // 常に空けておく数（既定 8 = [MS] NUM_EXTRA_VOICES）。0 で無効
    u32 mapRule;        // subset作成の規則（既定 MAP_C_STRICT_LSB、実際の銀行選択はMSGS仕様）
    s32 gainAdjust;     // 声ごとの持ち上げ（1/100 dB。既定 0 = MSGS CSynth::Open）
    u32 masterGainQ16;  // 最終段の倍率（Q16。既定 0x10000）
    u32 drumHold;       // 1: リズム ch は Note Off を無視して最後まで鳴らす（GS 音源・XP の MSGS の動き）。
                        // 既定 0 = Win7 以降の MSGS どおり（ゲート 1 のドラムが切れる）。ループ付きで
                        // サステインが -80dB より大きい音だけは Note Off を効かせる（止まらなくなるため）
};

void synthDefaultConfig(SynthConfig &c);

struct SynthStats
{
    u32 noteOns;        // 受けた Note On（ベロシティ>0）
    u32 notesStarted;   // 実際に声を当てた数
    u32 notesUnmapped;  // 楽器が見つからない（対応表にも読み替えにも無い）
    u32 notesFallback;  // 対応表に無く、読み替え規則で楽器を決めた
    u32 notesNoRegion;  // 楽器はあるが鍵域に合うリージョンが無い
    u32 notesLost;      // 空き声も奪える声も無く鳴らせなかった
    u32 voicesStolen;   // 発音時に他の声を奪った回数
    u32 quickStops;     // 予備確保・重なり・キーグループで素早く止めた回数
    u32 resets;         // GS/GM リセット系 SysEx
    u32 sysExIgnored;   // 処理しなかった SysEx
    u32 nrpnStored;     // NRPN を保持した回数
    u32 maxActive;      // 同時に使っていた声の最大
    u32 loopKilled;     // ループが増分より短く止めた声（[MS] と同じ扱い）
    u32 mixPeak;        // 最終段の前の |和| の最大（32767 を越えた分が音割れ。音量合わせの目安）
    u32 clippedSamples; // 最終段で飽和した標本（L/R 別に数える）
};

// 内部の表（init で arena から切り出す）。ヘッダに置くのは大きさを測るためだけ。
typedef msgs::EgDef SynthEgDef;

struct SynthArt
{
    SynthEgDef veg;   // EG1（音量）
    SynthEgDef peg;   // EG2（音程）
    u32 lfoInc;       // 256-entry phase index with 16 fractional bits
    u32 lfoDelay;     // 標本数
    s32 lfoVol;       // LFO→音量（VREL）
    s32 lfoPitch;     // LFO→音程（セント）
    s32 lfoMwVol;     // LFO×CC1→音量（VREL）
    s32 lfoMwPitch;   // LFO×CC1→音程（セント）
    s32 defaultPan;   // MIDI のパン値に足す量（[MS] (scale>>12)/125）
    u8 lfoActive;
    u8 pad_[3];
};

struct SynthWave
{
    const u8 *pcm;
    u32 samples;
    u32 rateRatioQ24; // 元の標本化周波数 / 出力の標本化周波数（Q24）
    u32 rate;
    s32 guard;
    u8 fmt;           // 0=16bit そのまま読める, 1=16bit LE をバイトで, 2=16bit BE をバイトで, 3=8bit
    u8 hasGuard, pad_[2];
};

struct SynthRegion
{
    u8 keyLo, keyHi, group, allowOverlap;
    u8 rootKey, oneShot;
    s16 fineTune;     // セント
    s32 atten;        // VREL（≤0）
    u32 wave;         // SynthWave の添字（TDSB_NONE = 無し）
    u32 loopStart, loopEnd;
    u32 art;          // SynthArt の添字
};

struct SynthInstrument
{
    u32 key;          // drum<<24 | msb<<16 | lsb<<8 | prog
    u32 firstRgn;
    u32 nRgn;
};

struct SynthMapEntry
{
    u32 key;
    u32 ins;          // SynthInstrument の添字（TDSB_NONE = 無し）
};

typedef msgs::Eg SynthEgState;

struct SynthVoice
{
    u8 inUse, noteOn, sustained, tagged;
    u8 channel, key, priority, group;
    u8 looping, fmt, dying, hasGuard;
    u32 program;
    u32 serial;
    const SynthArt *art;
    const u8 *pcm;
    u32 end;          // ループ有り: ループ終わり、無し: 標本数
    u32 loopStart, loopLen;
    u32 pos, frac;    // 整数部と 12bit 小数部
    u32 incInt, incFrac;
    u32 t;            // 発音からの標本数
    SynthEgState veg, peg;
    msgs::Lfo lfo;
    s32 basePitch;    // セント（±4800 に制限済み）
    u32 rateRatioQ24;
    s32 baseVol;      // VREL: マスター + ベロシティ + リージョン減衰
    s32 egVrel;       // EG1 の VREL（LFO 前。奪う声の選択用）
    s32 ampL, ampR;   // last target Q12 (0..4095)
    u32 samples;
    s32 guard;
    u32 baseStep, step, goalStep;
    u32 span, controlRemain, controlTotal;
    u32 rampPeriod, rampCounter, intervalDone;
    s32 pitch, volL, volR; // dynamic targets, before static base volume
    s64 accL, accR, accStep, deltaL, deltaR, deltaStep; // Q8 ramps
    u8 mmx, pairSecond, hold, pad2_;   // hold: drumHold で Note Off を無視した声
    u64 startFrame;
    s64 lastMixTime; // fixed previous CVoice::Mix cursor throughout this Render
};

struct SynthChannel
{
    u8 cc0, cc32, msb, lsb, program, rhythm, mono, sustain;
    u8 volume, pan, expression, modulation;
    u8 reverb, chorus, delay, paramIsNrpn;
    u8 nrpnMsb, nrpnLsb, pad_[2];
    u16 bend, rpn, data, pad2_;
    s32 bendRange;    // セント
    s32 fineTune;     // セント（RPN 1）
    s32 coarseTune;   // セント（RPN 2）
    u8 nrpnPart[8];   // GS パート NRPN（1,8/9/10/32/33/99/100/102）の値。0xff = 未設定
};

class Synth
{
  public:
    enum
    {
        kChannels = 16,
        kMaxBlock = 128, // render() 1 回の上限（これを越える呼び出しは中で分ける）
    };

    Synth();

    // init に要る arena の大きさ（おおよそではなく、init が切り出す合計）。
    static u32 memoryNeeded(const TdsbView &blob, const SynthConfig &cfg);
    // blob は tdsbOpen 済みで、PCM を含む元のメモリを生かしておくこと。
    bool init(const TdsbView &blob, const SynthConfig &cfg, Arena &arena);

    void powerOn();                               // 電源投入時の状態（全声を即止め）
    void shortMessage(u8 status, u8 d1, u8 d2);   // チャンネルメッセージ
    void sysEx(const u8 *msg, u32 len);           // msg[0] = 0xF0 から（F7 込み）
    void deviceReset();                           // midiOutReset 相当: 全チャンネルの音を解放へ
    void render(s16 *outStereo, u32 frames);      // frames 標本（LRLR）。1 回 = 制御ブロック 1 個
    // A slice of a larger MSGS Render block, for MIDI timestamp boundaries.
    void renderSlice(s16 *outStereo, u32 frames, u32 remainingInBlock,
                     u32 offsetInBlock, u32 blockFrames);
    // Submit all MIDI events of a Render block before rendering it. MSGS uses
    // future events in that block when it evaluates control targets.
    void beginEventBlock(u32 frames);
    bool setEventOffset(u32 offset);
    void endEventBlock();
    void setMasterGainQ16(u32 g) { cfg_.masterGainQ16 = g; }

    const SynthStats &stats() const { return stats_; }
    void clearStats();
    u32 activeVoices() const;
    u32 sampleRate() const { return cfg_.sampleRate; }
    const SynthChannel &channel(u32 ch) const { return ch_[ch & 15u]; }
    u8 drumNrpn(u32 map, u32 param, u32 note) const;  // 保持した GS ドラム NRPN（未使用、後で決める）
    // Read-only diagnostics for matching downloaded DLS parameters.
    u32 instrumentCount() const { return nIns_; }
    const SynthInstrument *instrument(u32 i) const { return i < nIns_ ? &ins_[i] : 0; }
    const SynthRegion *region(u32 i) const { return i < nRgns_ ? &rgns_[i] : 0; }
    const SynthArt *articulation(u32 i) const { return i < nArts_ ? &arts_[i] : 0; }
    const SynthWave *wave(u32 i) const { return i < nWaves_ ? &waves_[i] : 0; }
    u32 voiceCount() const { return nVoices_; }
    const SynthVoice *voice(u32 i) const { return i < nVoices_ ? &voices_[i] : 0; }

  private:
    void zero();
    void setSustain(u8 ch, bool on);
    void resetChannels();
    void resetAll(bool clearVoices);
    void noteOn(u8 ch, u8 key, u8 vel);
    void noteOff(u8 ch, u8 key);
    void controlChange(u8 ch, u8 cc, u8 v);
    void dataEntry(u8 ch, bool msb, u8 v);
    s32 findInstrument(u32 key);
    s32 fallbackInstrument(u32 key);
    s32 exactInstrument(u32 key);
    s32 findKit(u8 prog);
    SynthVoice *allocVoice(u8 priority);
    SynthVoice *stealVoice(u8 priority);
    void stealNotes(u32 horizon);
    void startVoice(SynthVoice &v, u8 ch, u8 key, u8 vel, const SynthRegion &r, u32 program);
    void stopVoice(SynthVoice &v, bool quick);
    void quickStop(SynthVoice &v);
    void quickStopAt(SynthVoice &v, u32 when);
    void clearVoice(SynthVoice &v);
    void voiceTargets(SynthVoice &v, u32 tEnd, u32 renderEnd, s32 &volL, s32 &volR, s32 &pitch);
    void renderBlock(s16 *out, u32 n, u32 renderRemaining, u32 renderOffset, u32 renderFrames, u64 nowFrame);
    SynthChannel channelAt(u8 ch, u64 frame) const;
    void recordChannel(u8 ch);

    SynthConfig cfg_;
    SynthStats stats_;
    SynthChannel ch_[kChannels];
    s32 masterVrel_;
    u32 serial_;
    bool ready_;
    bool hostLittle_;
    bool gsMode_;
    u64 clock_, eventBlockStart_;
    u32 eventOffset_, eventBlockFrames_, eventCount_;
    bool recordingEvents_, eventHistory_;
    SynthChannel blockChannels_[kChannels];
    struct ChannelEvent
    {
        u32 offset, channel;
        s32 bendRange;
        u16 bend;
        u8 modulation, volume, pan, expression;
    };
    enum { kChannelEvents = 256 };
    ChannelEvent events_[kChannelEvents];
    u32 quickRelease_;   // [MS] 標本化周波数 / 70
    u32 maxEgSamples_;   // [MS] 1764000 標本（44.1kHz で 40 秒）相当
    // arena から
    SynthArt *arts_;
    u32 nArts_;
    SynthWave *waves_;
    u32 nWaves_;
    SynthRegion *rgns_;
    u32 nRgns_;
    SynthInstrument *ins_;
    u32 nIns_;
    SynthMapEntry *map_;
    u32 nMap_;
    SynthVoice *voices_;
    u32 nVoices_;
    u8 *drumNrpn_;       // [2][6][128]
    s32 mix_[kMaxBlock * 2];
};

} // namespace tdls

#endif
