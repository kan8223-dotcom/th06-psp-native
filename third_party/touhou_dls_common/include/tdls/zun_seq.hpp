// 東方（TH06 紅魔郷 1.02h / TH07 妖々夢 / TH08 永夜抄 の Windows 版）の MidiOutput と同じ進め方で
// SMF を鳴らすシーケンサと、1ms ごとにそれを回しながら合成する駆動部。
//
// 元にしたもの（読んだだけ・写していない）:
//  - TH06: happyhavoc/th06（1.02h の一致型デコンパイル）src/MidiOutput.cpp
//  - TH07: ~/th07-speed-20261007/src/MidiOutput.cpp（コメントアウトされた原作の処理）、~/th07_upstream
//  - TH08: ~/th08-psp-port/th08-psp-native/src/Midi.cpp（ProcessMsg はバイト一致済みと docs/RE_HANDOFF.md）
//  3 作とも同じ処理だった。PC 版は timeSetEvent の 1ms 周期で OnTimerElapsed を呼び、その中で
//    now = tickBase + ms * division * 1000 / tempo   （u64、整数の割り算）
//  まで来たイベントをトラック順に処理して合成器へ送り、最後に ms++ する。
//  ここではそれを「出力音声の 1ms（44.1kHz なら 44.1 標本、端数は累積させない）」ごとに 1 回走らせ、
//  出たイベントは次に合成する標本から効く。時間は合成した標本数だけで進む（PS3 紅魔郷と同じ考え方）。
//
// 原作どおりにしている癖:
//  - テンポは tempo += (tempo << 8) + byte で読む（＝ tempo*257 + byte。正しい値より約 0.75% 遅い）。
//    setTempoQuirk(false) で正しい読み方にもできる（比較用）。
//  - テンポ変更では tickBase += ms*division*1000/tempo（切り捨て）してから ms = 0。
//  - CC2 で全トラックの位置・次の時刻・tempo/ms/tickBase を保存、CC4 で戻す（今のトラックの位置は
//    CC2 の直後＝次の間隔の手前を保存する。原作と同じ）。CC2/CC4 自体も合成器へ送る。
//  - 全トラックが終わると LoadTracks で頭から（テンポは戻さない）。
//  - フェード: 1ms ごとに m = (float)(1 - (double)e/d)（x87 53bit で計算して float へ）、
//    (u32)(m*128) が変わったら全チャンネルへ CC7 = (int)(曲の CC7 * m)。フェード中に来た CC7 も m 倍。
//    e が d に達したら m = 0 にしてそれ以降イベントを処理しない。浮動小数は整数で正確に再現する
//    （zunFadeMultiplier。timeSetEvent のスレッドの x87 精度が既定の 53bit であることは未確認）。
//  - 停止（StopPlayback）はタイマーを止めて midiOutReset。midiOutReset で MSGS が何をするかは
//    未確認なので、合成器の deviceReset（発音中の音を通常の解放へ、サステイン OFF）にしている。
// 使い方（ME 側で鳴らす例）:
//   static Synth synth; static ZunSequencer seq; static ZunCommandQueue q（非キャッシュ領域に置く）;
//   static ZunMidiPlayer player; player.init(&synth, &seq, &q);
//   音声スレッド: player.render(buf, 256);   // 長さは何でもよい（出力は呼び方に依らない）
//   ゲーム側: ZunCommand c = {ZUN_CMD_PLAY, 0, smf, smfSize}; q.push(c);   // 次の 1ms 境界で効く
//            ZUN_CMD_FADE(arg=ms) / ZUN_CMD_STOP も同じ
// 原作と違うところ（壊れたデータへの備えだけ）:
//  - トラックの終わりを越えて読まない（越えたらそのトラックは終わり扱い）。tempo が 0 になったら 1 にする。
//  - SysEx は 256 バイトまで送る（それより長いものは送らず読み飛ばす）。
#ifndef TDLS_ZUN_SEQ_HPP
#define TDLS_ZUN_SEQ_HPP

#include "synth.hpp"
#include "types.hpp"

#ifndef TDLS_MEMORY_BARRIER
#define TDLS_MEMORY_BARRIER() __sync_synchronize()
#endif

namespace tdls {

// m = mant / 2^shift。原作の fadeOutVolumeMultiplier（float）をそのまま表す。
struct ZunFadeMul
{
    u32 mant;
    u32 shift;
};

// x87（53bit）で 1.0f - (f32)e/(f32)d を計算して float に入れた値（0 ≤ e < d < 2^31）。
ZunFadeMul zunFadeMultiplier(u32 e, u32 d);
// floor(v * m)。v は 0..255。
u32 zunFadeApply(const ZunFadeMul &m, u32 v);

struct ZunSeqStats
{
    u32 timerCalls;      // OnTimerElapsed の回数（タイマー開始後）
    u32 messages;        // 合成器へ送った短いメッセージ（CC7 のフェード送信を含む）
    u32 noteOns;         // そのうち Note On（ベロシティ>0）
    u32 sysEx;           // 送った SysEx
    u32 tempoEvents;
    u32 loopSaves;       // CC2
    u32 loopJumps;       // CC4
    u32 noteOnsAtJump[2]; // 1 回目・2 回目の CC4 の時点での noteOns（前任の notes_game と比べる用）
    u32 restarts;        // 全トラック終了で LoadTracks した回数
    u32 fadeVolumeSends; // FadeOutSetVolume の回数
    u32 overruns;        // トラックの終わりを越えて読もうとした
};

class ZunMidiPlayer;

class ZunSequencer
{
  public:
    enum { kMaxTracks = 32, kMaxSysEx = 256 };

    ZunSequencer();
    void attach(Synth *synth) { synth_ = synth; }
    // 検証用: 送ったメッセージを (OnTimerElapsed の通し番号, 中身) で受け取る。0 で外す。
    // TDLS_DISABLE_TRACE removes both callback call sites (PSP ME build); API remains available.
    typedef void (*TraceFn)(void *user, u32 timerCall, const u8 *msg, u32 len);
    // Concrete nonvirtual transport: no function-pointer call in the ME path.
    // External trace remains the original timer/message stream.
    void attachPlayer(ZunMidiPlayer *player) { output_ = player; }
    void setTrace(TraceFn fn, void *user)
    {
        trace_ = fn;
        traceUser_ = user;
    }
    void setTempoQuirk(bool on) { tempoQuirk_ = on; }
    // PC の MSGS オフライン基準との比較用。既定では回数無制限、全トラック終了で頭へ戻る。
    // maxLoopJumps 回戻った後の CC4 は送らず停止する。stopAtEnd なら全トラック終了でも停止。
    void setPlaybackLimit(u32 maxLoopJumps, bool stopAtEnd)
    {
        maxLoopJumps_ = maxLoopJumps;
        stopAtEnd_ = stopAtEnd;
    }
    bool limitReached() const { return limitReached_; }
    bool playbackLimited() const { return stopAtEnd_ || maxLoopJumps_ != 0xffffffffu; }

    bool parseFile(const u8 *smf, u32 size); // ParseFile（smf は鳴らしている間ずっと保持すること）
    void play();                             // Play: LoadTracks → デバイスを開く → 1ms タイマー開始
    void stopPlayback();                     // StopPlayback: タイマー停止 → midiOutReset 相当
    void setFadeOut(u32 ms);                 // SetFadeOut
    void timerTick();                        // 1ms ごとに駆動部が呼ぶ（開始直後の 1 回目は待つ）
    void onTimerElapsed();                   // OnTimerElapsed そのもの

    bool timerRunning() const { return timerRunning_; }
    bool loaded() const { return numTracks_ > 0; }
    s32 tempo() const { return tempo_; }
    const ZunSeqStats &stats() const { return stats_; }
    void clearStats();

  private:
    struct Track
    {
        u32 begin, end;
        u32 cursor;
        u32 loopCursor;
        s32 nextTick;
        u32 loopNextTick;
        u8 playing;
        u8 status;
        u8 overrun;
        u8 pad_;
    };

    u64 now() const;
    void loadTracks();
    void processMsg(Track &t);
    u8 byteAt(Track &t, u32 pos);
    u32 readVlq(Track &t);
    void send(u8 status, u8 a, u8 b);
    void fadeOutSetVolume(s32 offset);

    Synth *synth_;
    TraceFn trace_;
    void *traceUser_;
    ZunMidiPlayer *output_;
    const u8 *data_;
    u32 size_;
    Track tracks_[kMaxTracks];
    s32 numTracks_;
    u32 division_;  // 原作は i32 だが u16 から入れるので常に 0 以上
    s32 tempo_;
    u64 ms_;        // 原作 playcount1 / elapsedMillisecondsAtCurrentTempo
    u64 tickBase_;  // 原作 playcount2 / elapsedTicksBeforeTempoChange
    s32 loopTempo_;
    u64 loopMs_;
    u64 loopTickBase_;
    bool fadeActive_;
    s32 fadeDuration_;
    s32 fadeElapsed_;
    u32 fadeLastSet_;
    ZunFadeMul fadeMul_;
    u8 channelVolume_[16];
    bool timerRunning_;
    bool timerArmed_;
    bool tempoQuirk_;
    bool stopAtEnd_;
    bool limitReached_;
    u32 maxLoopJumps_;
    ZunSeqStats stats_;
    u8 sysexBuf_[kMaxSysEx];
};

// ゲーム側（別スレッド）からの指示の受け箱。単一の書き手・単一の読み手。
// PSP の SC→ME のようにキャッシュが共有されない場合は、置き場所を非キャッシュにするのは機種側の仕事。
enum ZunCommandType
{
    ZUN_CMD_NONE = 0,
    ZUN_CMD_PLAY,      // data/size の SMF を鳴らす（StopPlayback → ParseFile → Play）
    ZUN_CMD_STOP,      // StopPlayback
    ZUN_CMD_FADE,      // SetFadeOut(arg ms)
    ZUN_CMD_GAIN,      // 合成器の最終段の倍率（arg = Q16）。ゲームの処理には無い、移植用
    ZUN_CMD_POWER_ON,  // 合成器を電源投入時へ（移植用）
};

struct ZunCommand
{
    u32 type;
    u32 arg;
    const u8 *data;
    u32 size;
};

class ZunCommandQueue
{
  public:
    enum { kCapacity = 16 };
    ZunCommandQueue();
    bool push(const ZunCommand &c); // ゲーム側。満杯なら false
    bool pop(ZunCommand &c);        // 音声側

  private:
    ZunCommand slots_[kCapacity];
    volatile u32 head_;
    volatile u32 tail_;
};

// 1ms 刻みの駆動部。render() は好きな長さで呼んでよく、出力は呼び方に関係なく同じになる
// （1ms 分を必ずまとめて合成し、余りは次の呼び出しへ持ち越す）。
class ZunMidiPlayer
{
  public:
    enum { kMaxTickFrames = 96, kMaxQuantumFrames = 960 };
    ZunMidiPlayer();
    bool init(Synth *synth, ZunSequencer *seq, ZunCommandQueue *queue);
    // 0: 従来の 1ms 駆動。1/10: MSGS 比較（最初の timer k=0、事象の時刻は
    // floor((k+phaseMs)/periodMs)*periodMs）。MSGS の Render と同じ rate/100 標本でも区切る。
    // 初期化直後、render の前に指定する。PSP の既存の呼び出し方は変わらない。
    bool setEventQuantization(u32 periodMs, u32 phaseMs = 0);
    // PC 比較の既知の終端。MSGS と同じく、最後の Render だけを残り標本数に縮める。
    // 初期化直後、render の前に指定。指定しなければ終端を設けない。
    bool setRenderFrameLimit(u64 limit);
    void render(s16 *outStereo, u32 frames);
    u64 elapsedMs() const { return ms_; }
    u64 renderedFrames() const { return frames_; }
    u64 eventSampleFrame() const { return periodMs_ ? eventFrame_ : frames_; }
    u64 limitStopTimerMs() const { return stopTimerMs_; } // 未停止なら ~0ull
    // Overflow latches failure until init(); queued PLAY does not recover.
    // A platform adapter must disable MIDI/fall back and discard queued data safely.
    bool outputFailed() const { return pendingFailed_; }
    u32 pendingPeak() const { return pendingPeak_; }
    u32 pendingOverflows() const { return pendingOverflows_; }
    enum { kPendingCapacity = 256, kPendingBytes = 1472 };

  private:
    void tick();
    void apply(const ZunCommand &c);
    u32 prepareQuantum();
    friend class ZunSequencer;
#ifdef TDLS_TEST_RAW_ARRIVAL
    friend struct ZunMidiPlayerTest;
#endif
    void receiveOutput(const u8 *msg, u32 len);
    void enqueue(const u8 *msg, u32 len);
    void flushPending(u64 end);
    void clearPending();
    static s32 pendingFrameDelta(u32 frameLow, u64 origin);

    struct Pending
    {
        u32 frameLow;
        u16 begin, size;
    };
    static_assert(sizeof(Pending) == 8, "pending MIDI descriptor must remain 8 bytes");

    Synth *synth_;
    ZunSequencer *seq_;
    ZunCommandQueue *queue_;
    u32 rate_;
    u32 acc_;
    u64 ms_;
    u64 frames_;
    u32 periodMs_, phaseMs_;
    u64 nextTimerMs_, stopTimerMs_, stopFrame_, eventFrame_, frameLimit_;
    bool stopPending_;
    // 256 descriptors + 1472 payload bytes: 3520 bytes on every supported ABI.
    // Pending events are at most phase9ms ahead: modular frame distance <2^31.
    // Overflow is fail-closed (reset + stop), never silent MIDI truncation.
    Pending pending_[kPendingCapacity];
    u8 pendingBytes_[kPendingBytes];
    u32 pendingHead_, pendingCount_, pendingWrite_, pendingUsed_;
    u32 pendingPeak_, pendingOverflows_;
    bool pendingFailed_;
    u32 bufPos_, bufLen_;
    s16 buf_[kMaxQuantumFrames * 2];
};

} // namespace tdls

#endif
