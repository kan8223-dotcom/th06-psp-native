#include "tdls/zun_seq.hpp"

#include <string.h>

namespace tdls {

namespace {

u32 bitLength(u64 x)
{
    u32 n = 0;
    while (x)
    {
        n++;
        x >>= 1;
    }
    return n;
}

// x を有効 sig ビットへ最近接偶数丸め。*dropped に捨てたビット数（繰り上がりで 1 増えることもある）。
u64 roundSig(u64 x, u32 sig, u32 *dropped)
{
    u32 len = bitLength(x);
    if (len <= sig)
    {
        *dropped = 0;
        return x;
    }
    u32 d = len - sig;
    u64 q = x >> d;
    u64 r = x & ((1ull << d) - 1);
    u64 half = 1ull << (d - 1);
    if (r > half || (r == half && (q & 1u)))
    {
        q++;
        if (q >> sig)
        {
            q >>= 1;
            d++;
        }
    }
    *dropped = d;
    return q;
}

} // namespace

ZunFadeMul zunFadeMultiplier(u32 e, u32 d)
{
    ZunFadeMul m;
    if (e == 0 || d == 0 || e >= d)
    {
        // e == 0 は 1.0。e >= d は呼ばれない（原作はその前に 0 にする）が、0 を返しておく。
        m.mant = (e == 0 && d != 0) ? (1u << 23) : 0u;
        m.shift = 23;
        return m;
    }
    // q = e/d を double（53bit、最近接偶数）へ: 2^-k ≤ q < 2^-(k-1)
    u32 k = 1;
    while ((((u64)e) << k) < (u64)d) k++;
    u64 rem = e, mq = 0;
    for (u32 i = 0; i < 52u + k; i++)
    {
        rem <<= 1;
        mq <<= 1;
        if (rem >= d)
        {
            rem -= d;
            mq |= 1u;
        }
    }
    if (2u * rem > (u64)d || (2u * rem == (u64)d && (mq & 1u)))
    {
        mq++;
        if (mq >> 53)
        {
            mq >>= 1;
            k--;
        }
    }
    // q = mq * 2^-(52+k)、mq は [2^52, 2^53)
    u64 r53; // r = 1 - q を double へ丸めた値 = r53 * 2^-53
    if (k == 1)
    {
        r53 = (1ull << 53) - mq; // q ≥ 1/2 なら 1 - q はそのまま表せる
    }
    else
    {
        // r * 2^53 = 2^53 - mq / 2^(k-1) を整数へ最近接偶数丸め
        u32 sh = k - 1;
        u64 f = mq >> sh;
        u64 fr = mq & ((1ull << sh) - 1);
        u64 half = 1ull << (sh - 1);
        u64 hi = (1ull << 53) - f;
        if (fr == 0) r53 = hi;
        else if (fr < half) r53 = hi;
        else if (fr > half) r53 = hi - 1;
        else r53 = (hi & 1u) ? hi - 1 : hi;
    }
    // float（24bit）へ
    u32 dropped;
    u64 m24 = roundSig(r53, 24, &dropped);
    m.mant = (u32)m24;
    m.shift = 53u - dropped;
    return m;
}

u32 zunFadeApply(const ZunFadeMul &m, u32 v)
{
    if (m.mant == 0 || m.shift >= 64) return 0;
    return (u32)(((u64)v * m.mant) >> m.shift);
}

ZunSequencer::ZunSequencer()
{
    synth_ = 0;
    trace_ = 0;
    traceUser_ = 0;
    output_ = 0;
    data_ = 0;
    size_ = 0;
    memset(tracks_, 0, sizeof(tracks_));
    numTracks_ = 0;
    division_ = 0;
    tempo_ = 1000000;
    ms_ = 0;
    tickBase_ = 0;
    loopTempo_ = 1000000; // 原作は未初期化のまま前の曲の値が残る。CC2 より前の CC4 は曲に無い
    loopMs_ = 0;
    loopTickBase_ = 0;
    fadeActive_ = false;
    fadeDuration_ = 0;
    fadeElapsed_ = 0;
    fadeLastSet_ = 0;
    fadeMul_.mant = 0; // 原作のコンストラクタは 0.0
    fadeMul_.shift = 23;
    memset(channelVolume_, 0, sizeof(channelVolume_)); // 原作は未初期化（曲は全 ch に CC7 を送る）
    timerRunning_ = false;
    timerArmed_ = false;
    tempoQuirk_ = true;
    stopAtEnd_ = false;
    limitReached_ = false;
    maxLoopJumps_ = 0xffffffffu;
    memset(&stats_, 0, sizeof(stats_));
    memset(sysexBuf_, 0, sizeof(sysexBuf_));
}

void ZunSequencer::clearStats() { memset(&stats_, 0, sizeof(stats_)); }

bool ZunSequencer::parseFile(const u8 *smf, u32 size)
{
    // ClearTracks
    numTracks_ = 0;
    data_ = 0;
    size_ = 0;
    if (!smf || size < 14 || memcmp(smf, "MThd", 4) != 0)
    {
        return false;
    }
    u32 hdrLen = rd32be(smf + 4);
    if (hdrLen < 6 || hdrLen > size - 8)
    {
        return false;
    }
    u32 ntr = rd16be(smf + 10);
    if (ntr > kMaxTracks)
    {
        return false;
    }
    division_ = rd16be(smf + 12); // 原作も u16 をそのまま（SMPTE は考えていない）
    u32 pos = 8 + hdrLen;
    memset(tracks_, 0, sizeof(tracks_));
    for (u32 i = 0; i < ntr; i++)
    {
        // 原作は "MTrk" かどうかを見ない
        if (size - pos < 8)
        {
            return false;
        }
        u32 len = rd32be(smf + pos + 4);
        if (len > size - pos - 8)
        {
            return false;
        }
        tracks_[i].begin = pos + 8;
        tracks_[i].end = pos + 8 + len;
        tracks_[i].playing = 1;
        pos += 8 + len;
    }
    data_ = smf;
    size_ = size;
    numTracks_ = (s32)ntr;
    tempo_ = 1000000; // 原作の ParseFile
    return true;
}

u8 ZunSequencer::byteAt(Track &t, u32 pos)
{
    if (pos >= t.end)
    {
        t.overrun = 1;
        return 0;
    }
    return data_[pos];
}

u32 ZunSequencer::readVlq(Track &t)
{
    u32 v = 0;
    for (u32 i = 0; i < 5; i++)
    {
        u8 b = byteAt(t, t.cursor);
        if (t.overrun) return v;
        t.cursor++;
        v = v * 0x80u + (u32)(b & 0x7fu);
        if ((b & 0x80u) == 0) break;
    }
    return v;
}

void ZunSequencer::loadTracks()
{
    fadeMul_.mant = 1u << 23; // 1.0
    fadeMul_.shift = 23;
    fadeActive_ = false;
    ms_ = 0;
    tickBase_ = 0;
    for (s32 i = 0; i < numTracks_; i++)
    {
        Track &t = tracks_[i];
        t.cursor = t.begin;
        t.loopCursor = t.cursor; // 原作: loop = 最初の間隔の手前（loopNextTick は触らない）
        t.playing = 1;
        t.overrun = 0;
        t.nextTick = (s32)readVlq(t);
        if (t.overrun) t.playing = 0;
    }
}

void ZunSequencer::play()
{
    if (numTracks_ == 0) return; // 原作: tracks が無ければ何もしない
    loadTracks();
    // OpenDevice（何もしない）→ StartTimer(1)
    timerRunning_ = true;
    timerArmed_ = false;
    limitReached_ = false;
}

void ZunSequencer::stopPlayback()
{
    if (numTracks_ == 0) return; // 原作: tracks == NULL なら何もしない
    timerRunning_ = false;
    timerArmed_ = false;
    if (synth_) synth_->deviceReset(); // midiOutReset（中身は未確認）
}

void ZunSequencer::setFadeOut(u32 ms)
{
    fadeMul_.mant = 0; // 0.0
    fadeMul_.shift = 23;
    fadeDuration_ = (s32)ms;
    fadeElapsed_ = 0;
    fadeActive_ = true;
}

u64 ZunSequencer::now() const
{
    // tickBase + ms * division * 1000 / tempo（原作と同じ u64 の計算）
    u64 t = tempo_ == 0 ? 1u : (u64)(s64)tempo_;
    return tickBase_ + (ms_ * (u64)division_ * 1000u) / t;
}

void ZunSequencer::send(u8 status, u8 a, u8 b)
{
    stats_.messages++;
    if ((status & 0xf0u) == 0x90u && b != 0) stats_.noteOns++;
#ifndef TDLS_DISABLE_TRACE
    if (trace_)
    {
        u8 m[3] = {status, a, b};
        trace_(traceUser_, stats_.timerCalls, m, 3);
    }
#endif
    if (output_)
    {
        u8 m[3] = {status, a, b};
        output_->receiveOutput(m, 3);
    }
    else if (synth_) synth_->shortMessage(status, a, b);
}

void ZunSequencer::fadeOutSetVolume(s32 offset)
{
    stats_.fadeVolumeSends++;
    for (u32 ch = 0; ch < 16; ch++)
    {
        s32 v = (s32)zunFadeApply(fadeMul_, channelVolume_[ch]) + offset;
        if (v < 0) v = 0;
        else if (v > 127) v = 127;
        send((u8)(0xb0u + ch), 7, (u8)v);
    }
}

void ZunSequencer::processMsg(Track &t)
{
    u8 status = byteAt(t, t.cursor);
    if (t.overrun)
    {
        t.playing = 0;
        stats_.overruns++;
        return;
    }
    if (status < 0x80u) status = t.status;
    else t.cursor++;
    u8 hi = (u8)(status & 0xf0u), lo = (u8)(status & 0x0fu);
    u8 a1 = 0, a2 = 0;
    switch (hi)
    {
    case 0xf0:
        if (status == 0xf0u)
        {
            u32 len = readVlq(t);
            u32 avail = t.cursor <= t.end ? t.end - t.cursor : 0u;
            if (len > avail)
            {
                t.overrun = 1;
                len = avail;
            }
            if (len + 1u <= (u32)kMaxSysEx)
            {
                sysexBuf_[0] = 0xf0;
                memcpy(sysexBuf_ + 1, data_ + t.cursor, len);
                stats_.sysEx++;
#ifndef TDLS_DISABLE_TRACE
                if (trace_) trace_(traceUser_, stats_.timerCalls, sysexBuf_, len + 1u);
#endif
                if (output_) output_->receiveOutput(sysexBuf_, len + 1u);
                else if (synth_) synth_->sysEx(sysexBuf_, len + 1u);
            }
            t.cursor += len;
        }
        else if (status == 0xffu)
        {
            u8 type = byteAt(t, t.cursor);
            t.cursor++;
            u32 len = readVlq(t);
            if (type == 0x2fu)
            {
                t.playing = 0; // End of Track（ここで戻る。走行状態・次の間隔は触らない）
                return;
            }
            if (type == 0x51u)
            {
                stats_.tempoEvents++;
                u64 tt = tempo_ == 0 ? 1u : (u64)(s64)tempo_;
                tickBase_ += (ms_ * (u64)division_ * 1000u) / tt;
                ms_ = 0;
                u32 tempo = 0;
                for (u32 i = 0; i < len; i++)
                {
                    u32 b = byteAt(t, t.cursor);
                    t.cursor++;
                    if (tempoQuirk_) tempo += (tempo << 8) + b; // 原作: tempo += (tempo << 8) + byte
                    else tempo = (tempo << 8) + b;
                }
                tempo_ = (s32)tempo;
                if (tempo_ == 0) tempo_ = 1; // 原作は 0 除算になる（曲には無い）
                break;
            }
            t.cursor += len;
        }
        // F1..FE（F7 を含む）は原作と同じく何も読まない
        break;
    case 0x80:
    case 0x90:
    case 0xa0:
    case 0xb0:
    case 0xe0:
        a1 = byteAt(t, t.cursor++);
        a2 = byteAt(t, t.cursor++);
        break;
    case 0xc0:
    case 0xd0:
        a1 = byteAt(t, t.cursor++);
        a2 = 0;
        break;
    default:
        break; // 走行状態 0（曲の頭がデータバイト）: 原作も何も読まない
    }
    if (hi == 0xb0u)
    {
        switch (a1)
        {
        case 7:
            channelVolume_[lo] = a2;
            {
                u32 v = zunFadeApply(fadeMul_, a2); // 原作: (int)((float)arg2 * mult)
                a2 = (u8)(v > 127u ? 127u : v);
            }
            break;
        case 2: // ループ点を保存（全トラック）
            stats_.loopSaves++;
            for (s32 i = 0; i < numTracks_; i++)
            {
                tracks_[i].loopCursor = tracks_[i].cursor;
                tracks_[i].loopNextTick = (u32)tracks_[i].nextTick;
            }
            loopTempo_ = tempo_;
            loopMs_ = ms_;
            loopTickBase_ = tickBase_;
            break;
        case 4: // ループ点へ戻る（全トラック）
            if (stats_.loopJumps >= maxLoopJumps_)
            {
                limitReached_ = true;
                timerRunning_ = false;
                return;
            }
            if (stats_.loopJumps < 2) stats_.noteOnsAtJump[stats_.loopJumps] = stats_.noteOns;
            stats_.loopJumps++;
            for (s32 i = 0; i < numTracks_; i++)
            {
                tracks_[i].cursor = tracks_[i].loopCursor;
                tracks_[i].nextTick = (s32)tracks_[i].loopNextTick;
            }
            tempo_ = loopTempo_;
            ms_ = loopMs_;
            tickBase_ = loopTickBase_;
            break;
        default:
            break;
        }
    }
    if (status >= 0x80u && status < 0xf0u)
    {
        send(status, a1, a2);
    }
    t.status = status;
    t.nextTick = (s32)((u32)t.nextTick + readVlq(t));
    if (t.overrun)
    {
        t.playing = 0;
        stats_.overruns++;
    }
}

void ZunSequencer::onTimerElapsed()
{
    if (numTracks_ == 0) return;
    stats_.timerCalls++;
    u64 cur = now();
    if (fadeActive_)
    {
        if (fadeElapsed_ < fadeDuration_)
        {
            fadeMul_ = zunFadeMultiplier((u32)fadeElapsed_, (u32)fadeDuration_);
            u32 v128 = zunFadeApply(fadeMul_, 128);
            if (v128 != fadeLastSet_) fadeOutSetVolume(0);
            fadeLastSet_ = v128;
            fadeElapsed_++;
        }
        else
        {
            fadeMul_.mant = 0;
            fadeMul_.shift = 23;
            return;
        }
    }
    bool trackLoaded = false;
    for (s32 i = 0; i < numTracks_ && !limitReached_; i++)
    {
        Track &t = tracks_[i];
        if (!t.playing) continue;
        trackLoaded = true;
        while (t.playing && !limitReached_ && (u64)(s64)t.nextTick <= cur)
        {
            processMsg(t);
            cur = now();
        }
    }
    ms_++;
    if (!trackLoaded)
    {
        if (stopAtEnd_)
        {
            limitReached_ = true;
            timerRunning_ = false;
            return;
        }
        stats_.restarts++;
        loadTracks();
    }
}

void ZunSequencer::timerTick()
{
    if (!timerRunning_) return;
    if (!timerArmed_)
    {
        timerArmed_ = true; // timeSetEvent(1) の最初の呼び出しは 1ms 後
        return;
    }
    onTimerElapsed();
}

ZunCommandQueue::ZunCommandQueue()
{
    memset(slots_, 0, sizeof(slots_));
    head_ = 0;
    tail_ = 0;
}

bool ZunCommandQueue::push(const ZunCommand &c)
{
    u32 h = head_;
    if (h - tail_ >= (u32)kCapacity) return false;
    slots_[h & (kCapacity - 1u)] = c;
    TDLS_MEMORY_BARRIER();
    head_ = h + 1u;
    return true;
}

bool ZunCommandQueue::pop(ZunCommand &c)
{
    u32 t = tail_;
    if (t == head_) return false;
    TDLS_MEMORY_BARRIER();
    c = slots_[t & (kCapacity - 1u)];
    TDLS_MEMORY_BARRIER();
    tail_ = t + 1u;
    return true;
}

ZunMidiPlayer::ZunMidiPlayer()
{
    synth_ = 0;
    seq_ = 0;
    queue_ = 0;
    rate_ = 0;
    acc_ = 0;
    ms_ = 0;
    frames_ = 0;
    periodMs_ = phaseMs_ = 0;
    nextTimerMs_ = 0;
    stopTimerMs_ = stopFrame_ = ~0ull;
    eventFrame_ = 0;
    frameLimit_ = ~0ull;
    stopPending_ = false;
    clearPending();
    pendingPeak_ = pendingOverflows_ = 0;
    pendingFailed_ = false;
    bufPos_ = bufLen_ = 0;
    memset(buf_, 0, sizeof(buf_));
}

bool ZunMidiPlayer::init(Synth *synth, ZunSequencer *seq, ZunCommandQueue *queue)
{
    synth_ = synth;
    seq_ = seq;
    queue_ = queue;
    rate_ = synth ? synth->sampleRate() : 0;
    acc_ = 0;
    ms_ = 0;
    frames_ = 0;
    periodMs_ = phaseMs_ = 0;
    nextTimerMs_ = 0;
    stopTimerMs_ = stopFrame_ = ~0ull;
    eventFrame_ = 0;
    frameLimit_ = ~0ull;
    stopPending_ = false;
    clearPending();
    pendingPeak_ = pendingOverflows_ = 0;
    pendingFailed_ = false;
    bufPos_ = bufLen_ = 0;
    if (!synth || !seq || rate_ < 8000 || rate_ > (u32)kMaxTickFrames * 1000u) return false;
    seq->attach(synth);
    seq->attachPlayer(this);
    return true;
}

bool ZunMidiPlayer::setEventQuantization(u32 periodMs, u32 phaseMs)
{
    if (frames_ != 0 || (periodMs != 0 && periodMs != 1 && periodMs != 10) ||
        (periodMs == 0 ? phaseMs != 0 : phaseMs >= periodMs)) return false;
    periodMs_ = periodMs;
    phaseMs_ = phaseMs;
    return true;
}

bool ZunMidiPlayer::setRenderFrameLimit(u64 limit)
{
    if (!synth_ || frames_ != 0 || limit == 0) return false;
    frameLimit_ = limit;
    return true;
}

void ZunMidiPlayer::apply(const ZunCommand &c)
{
    switch (c.type)
    {
    case ZUN_CMD_PLAY: // PlayFile: StopPlayback → ParseFile → Play
        clearPending();
        stopPending_ = false;
        stopTimerMs_ = stopFrame_ = ~0ull;
        seq_->stopPlayback();
        if (seq_->parseFile(c.data, c.size)) seq_->play();
        break;
    case ZUN_CMD_STOP: clearPending(); seq_->stopPlayback(); break;
    case ZUN_CMD_FADE: seq_->setFadeOut(c.arg); break;
    case ZUN_CMD_GAIN: synth_->setMasterGainQ16(c.arg); break;
    case ZUN_CMD_POWER_ON: clearPending(); synth_->powerOn(); break;
    default: break;
    }
}

void ZunMidiPlayer::tick()
{
    ZunCommand c;
    while (queue_ && queue_->pop(c)) apply(c);
    seq_->timerTick();
}

void ZunMidiPlayer::clearPending()
{
    pendingHead_ = pendingCount_ = pendingWrite_ = pendingUsed_ = 0;
}

void ZunMidiPlayer::receiveOutput(const u8 *msg, u32 len)
{
    if (periodMs_) enqueue(msg, len);
    else if (len == 3 && msg[0] != 0xf0u) synth_->shortMessage(msg[0], msg[1], msg[2]);
    else synth_->sysEx(msg, len);
}

void ZunMidiPlayer::enqueue(const u8 *msg, u32 len)
{
    if (pendingFailed_) return;
    if (pendingCount_ == kPendingCapacity || len > ZunSequencer::kMaxSysEx ||
        len > (u32)kPendingBytes - pendingUsed_)
    {
        ++pendingOverflows_;
        pendingFailed_ = true;
        clearPending();
        seq_->stopPlayback();
        synth_->powerOn(); // reset is normally release-only; failure discards voices immediately.
        return;
    }
    Pending &p = pending_[(pendingHead_ + pendingCount_) % kPendingCapacity];
    p.frameLow = (u32)eventFrame_;
    p.begin = (u16)pendingWrite_;
    p.size = (u16)len;
    for (u32 i = 0; i < len; ++i)
    {
        pendingBytes_[pendingWrite_] = msg[i];
        pendingWrite_ = (pendingWrite_ + 1u) % kPendingBytes;
    }
    pendingUsed_ += len;
    ++pendingCount_;
    if (pendingCount_ > pendingPeak_) pendingPeak_ = pendingCount_;
}

s32 ZunMidiPlayer::pendingFrameDelta(u32 frameLow, u64 origin)
{
    const u32 d = frameLow - (u32)origin;
    // Both casts below are <=INT32_MAX, avoiding implementation-defined u32->s32 conversion.
    return (d & 0x80000000u) ? -1 - (s32)(~d) : (s32)d;
}

void ZunMidiPlayer::flushPending(u64 end)
{
    while (pendingCount_ && pendingFrameDelta(pending_[pendingHead_].frameLow, end) <= 0)
    {
        const Pending &p = pending_[pendingHead_];
        u8 msg[ZunSequencer::kMaxSysEx];
        for (u32 i = 0; i < p.size; ++i) msg[i] = pendingBytes_[((u32)p.begin + i) % kPendingBytes];
        const s32 delta = pendingFrameDelta(p.frameLow, frames_);
        const u32 at = delta > 0 ? (u32)delta : 0u;
        synth_->setEventOffset(at);
        if (p.size == 3 && msg[0] != 0xf0u) synth_->shortMessage(msg[0], msg[1], msg[2]);
        else synth_->sysEx(msg, p.size);
        pendingUsed_ -= p.size;
        pendingHead_ = (pendingHead_ + 1u) % kPendingCapacity;
        --pendingCount_;
    }
}

u32 ZunMidiPlayer::prepareQuantum()
{
    // MSGS と同じ固定 rate/100 標本の Render。MIDI はこのブロックを合成する前に予約する。
    // 原作のシーケンサは各 k ms のまま、合成器へ渡す時刻だけを整数の格子に丸める。
    u32 block = rate_ / 100u;
    if (frameLimit_ - frames_ < block) block = (u32)(frameLimit_ - frames_);
    u64 blockEnd = frames_ + block;
    synth_->beginEventBlock(block);
    synth_->setEventOffset(0);
    eventFrame_ = frames_;
    ZunCommand c;
    while (queue_ && queue_->pop(c)) apply(c);
    flushPending(frames_);
    // Native msgs_render: raw timer arrival is strict < blockEnd. The time in
    // the delivered MIDI remains quantized, and can be past or still future.
    while (!pendingFailed_ && nextTimerMs_ * rate_ / 1000u < blockEnd)
    {
        u64 q = ((nextTimerMs_ + phaseMs_) / periodMs_) * periodMs_;
        eventFrame_ = q * rate_ / 1000u;
        if (eventFrame_ < frames_) eventFrame_ = frames_;
        if (seq_->timerRunning())
        {
            seq_->onTimerElapsed();
            if (seq_->limitReached() && stopTimerMs_ == ~0ull)
            {
                stopTimerMs_ = nextTimerMs_;
                stopFrame_ = nextTimerMs_ * rate_ / 1000u;
                stopPending_ = true;
            }
        }
        // Native RecordMIDINote sorts timestamps. A later unquantized terminal
        // CC123 may precede MIDI just received with a future quantized stamp.
        // Until the timer batch is known, commit only up to its raw arrival.
        flushPending(nextTimerMs_ * rate_ / 1000u);
        ++nextTimerMs_;
        if (stopPending_)
        {
            // No more callbacks belong to this stopped timer batch. Still
            // advance its raw clock, so a PLAY at the next quantum cannot
            // replay the old batch's remaining callbacks into the new song.
            // Do not flush here: terminal CC123 must precede future stamps.
            while (nextTimerMs_ * rate_ / 1000u < blockEnd) ++nextTimerMs_;
            break;
        }
    }
    if (stopPending_ && stopFrame_ < blockEnd)
    {
        // オフライン MSGS は停止した timer の丸めない時刻で CC123 を送る。
        synth_->setEventOffset(stopFrame_ > frames_ ? (u32)(stopFrame_ - frames_) : 0u);
        for (u32 ch = 0; ch < 16; ch++) synth_->shortMessage((u8)(0xb0u | ch), 123, 0);
        stopPending_ = false;
    }
    flushPending(blockEnd);
    synth_->endEventBlock();
    return block;
}

void ZunMidiPlayer::render(s16 *out, u32 frames)
{
    while (frames > 0)
    {
        if (bufPos_ < bufLen_)
        {
            u32 k = bufLen_ - bufPos_;
            if (k > frames) k = frames;
            memcpy(out, buf_ + bufPos_ * 2u, k * 2u * sizeof(s16));
            bufPos_ += k;
            out += k * 2u;
            frames -= k;
            continue;
        }
        if (frames_ >= frameLimit_ || pendingFailed_)
        {
            memset(out, 0, frames * 2u * sizeof(s16));
            return;
        }
        u32 n;
        if (periodMs_)
        {
            n = prepareQuantum();
            ms_ = (frames_ + n) * 1000u / rate_;
        }
        else
        {
            // 従来の 1ms の境目: 指示を取り込み、それから 1ms 分を合成
            tick();
            acc_ += rate_;
            n = acc_ / 1000u;
            acc_ -= n * 1000u;
            ms_++;
            if (frameLimit_ - frames_ < n) n = (u32)(frameLimit_ - frames_);
        }
        frames_ += n;
        if (frames >= n)
        {
            synth_->render(out, n);
            out += n * 2u;
            frames -= n;
        }
        else
        {
            synth_->render(buf_, n);
            bufPos_ = 0;
            bufLen_ = n;
        }
    }
}

} // namespace tdls
