#include "MidiScript.hpp"

#include <new>
#include <stdlib.h>
#include <string.h>

#include "tdls/blob.hpp"
#include "tdls/synth.hpp"

namespace th06midi
{

namespace
{

using tdls::s16;
using tdls::u64;

void HashPcm(tdls::Sha256 &hash, const s16 *pcm, u32 samples)
{
    // Little-endian bytes, like dls_render_tool's pcmSha (machine independent).
    u8 buffer[512];
    u32 i = 0;
    while (i < samples)
    {
        u32 k = 0;
        for (; k < sizeof(buffer) / 2 && i < samples; ++k, ++i)
            tdls::wr16le(buffer + k * 2, static_cast<tdls::u16>(pcm[i]));
        hash.update(buffer, k * 2);
    }
}

class SmfWriter
{
  public:
    SmfWriter(u8 *out, u32 cap) : out_(out), cap_(cap), size_(0), ok_(true) {}
    void Byte(u32 v)
    {
        if (size_ < cap_)
            out_[size_++] = static_cast<u8>(v);
        else
            ok_ = false;
    }
    void Bytes(const u8 *p, u32 n)
    {
        for (u32 i = 0; i < n; ++i)
            Byte(p[i]);
    }
    void Vlq(u32 v)
    {
        u8 tmp[5];
        u32 n = 0;
        tmp[n++] = static_cast<u8>(v & 0x7f);
        while ((v >>= 7) != 0)
            tmp[n++] = static_cast<u8>(0x80 | (v & 0x7f));
        while (n)
            Byte(tmp[--n]);
    }
    u32 size() const { return size_; }
    bool ok() const { return ok_; }
    u8 *at(u32 offset) { return out_ + offset; }

  private:
    u8 *out_;
    u32 cap_;
    u32 size_;
    bool ok_;
};

class TrackBuilder
{
  public:
    explicit TrackBuilder(SmfWriter &w) : w_(w), tick_(0) {}
    void At(u32 tick)
    {
        w_.Vlq(tick - tick_);
        tick_ = tick;
    }
    void Short(u32 tick, u32 status, u32 a, u32 b)
    {
        At(tick);
        w_.Byte(status);
        w_.Byte(a);
        if ((status & 0xf0u) != 0xc0u && (status & 0xf0u) != 0xd0u)
            w_.Byte(b);
    }
    void SysEx(u32 tick, const u8 *body, u32 n) // body excludes F0, includes F7
    {
        At(tick);
        w_.Byte(0xf0);
        w_.Vlq(n);
        w_.Bytes(body, n);
    }
    void Meta(u32 tick, u32 type, const u8 *data, u32 n)
    {
        At(tick);
        w_.Byte(0xff);
        w_.Byte(type);
        w_.Vlq(n);
        w_.Bytes(data, n);
    }

  private:
    SmfWriter &w_;
    u32 tick_;
};

// Plain SC sink: one synthesizer, sequencer, player and queue.
class LocalSink : public ScheduleSink
{
  public:
    LocalSink() : arena_(0), upsample_(false)
    {
        memset(&up_, 0, sizeof(up_));
    }
    ~LocalSink() { free(arena_); }
    bool Init(const u8 *blob, u32 bytes, const Settings &settings, bool upsample)
    {
        upsample_ = upsample && UpsampleFactor(settings) == 2u;
        if (tdls::tdsbOpen(blob, bytes, view_) != tdls::TDSB_OK)
            return false;
        tdls::SynthConfig config;
        ToSynthConfig(settings, config);
        const u32 need = tdls::Synth::memoryNeeded(view_, config);
        arena_ = static_cast<u8 *>(malloc(need));
        if (!arena_)
            return false;
        tdls::Arena arena;
        arena.init(arena_, need);
        return synth_.init(view_, config, arena) && player_.init(&synth_, &seq_, &queue_) &&
               player_.setEventQuantization(settings.quantMs, 0u);
    }
    bool Push(const tdls::ZunCommand &command) { return queue_.push(command); }
    bool Render(s16 *out, u32 frames)
    {
        if (upsample_)
            RenderUpsampled2x(player_, up_, out, frames, tmp_);
        else
            player_.render(out, frames);
        return !player_.outputFailed();
    }
    const tdls::SynthStats &stats() const { return synth_.stats(); }

  private:
    tdls::TdsbView view_;
    tdls::Synth synth_;
    tdls::ZunSequencer seq_;
    tdls::ZunCommandQueue queue_;
    tdls::ZunMidiPlayer player_;
    u8 *arena_;
    bool upsample_;
    Upsample2x up_;
    s16 tmp_[(1024 / 2 + 1) * 2];
};

const ScheduleEvent kSelfTestEvents[] = {
    {250u, tdls::ZUN_CMD_FADE, 120u},
    {420u, tdls::ZUN_CMD_STOP, 0u},
};

const ScheduleEvent kFadeStopEvents[] = {
    {60000u, tdls::ZUN_CMD_FADE, 3000u},
    {80000u, tdls::ZUN_CMD_STOP, 0u},
};

} // namespace

const Schedule kSelfTestSchedule = {460u, 512u, kSelfTestEvents, 2u};

bool RunSchedule(const Schedule &schedule, const u8 *smf, u32 smfBytes, u32 sampleRate, ScheduleSink &sink,
                 s16 *pcm, tdls::Sha256 &hash, u32 *frames, const char **error)
{
    u64 frame = 0;
    const u64 rate = sampleRate;
    const char *dummy = 0;
    const char **why = error ? error : &dummy;
    auto renderTo = [&](u64 target) -> bool {
        while (frame < target)
        {
            u64 n = target - frame;
            if (n > schedule.chunkFrames)
                n = schedule.chunkFrames;
            if (!sink.Render(pcm, static_cast<u32>(n)))
            {
                *why = "render failed";
                return false;
            }
            HashPcm(hash, pcm, static_cast<u32>(n) * 2u);
            frame += n;
        }
        return true;
    };
    tdls::ZunCommand play = {tdls::ZUN_CMD_PLAY, 0u, smf, smfBytes};
    if (schedule.chunkFrames == 0 || !sink.Push(play))
    {
        *why = "cannot start";
        return false;
    }
    for (u32 i = 0; i < schedule.eventCount; ++i)
    {
        const ScheduleEvent &e = schedule.events[i];
        if (!renderTo(static_cast<u64>(e.ms) * rate / 1000u))
            return false;
        tdls::ZunCommand c = {e.type, e.arg, 0, 0};
        if (!sink.Push(c))
        {
            *why = "queue full";
            return false;
        }
    }
    if (!renderTo(static_cast<u64>(schedule.totalMs) * rate / 1000u))
        return false;
    if (frames)
        *frames = static_cast<u32>(frame);
    return true;
}

u32 BuildSelfTestSmf(u8 *out, u32 cap)
{
    SmfWriter w(out, cap);
    static const u8 kHeader[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0};
    w.Bytes(kHeader, sizeof(kHeader));
    static const u8 kTrack[] = {'M', 'T', 'r', 'k', 0, 0, 0, 0};
    w.Bytes(kTrack, sizeof(kTrack));
    const u32 trackStart = w.size();
    TrackBuilder t(w);

    static const u8 kGsReset[] = {0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7};
    t.SysEx(0, kGsReset, sizeof(kGsReset));
    static const u8 kTempo[] = {0x07, 0xa1, 0x20};
    t.Meta(0, 0x51, kTempo, sizeof(kTempo));
    // Programs the TH06 subset contains (bank LSB 3 like the game's songs).
    static const u8 kPrograms[8] = {0, 33, 48, 56, 80, 61, 24, 1};
    static const u8 kPans[8] = {64, 32, 96, 0, 127, 48, 80, 64};
    for (u32 ch = 0; ch < 8; ++ch)
    {
        t.Short(0, 0xb0 | ch, 0, 0);
        t.Short(0, 0xb0 | ch, 32, 3);
        t.Short(0, 0xc0 | ch, kPrograms[ch], 0);
        t.Short(0, 0xb0 | ch, 7, 100 - ch * 4);
        t.Short(0, 0xb0 | ch, 10, kPans[ch]);
        t.Short(0, 0xb0 | ch, 11, 127);
        t.Short(0, 0xb0 | ch, 91, 40);
    }
    t.Short(0, 0xb9, 0, 0);
    t.Short(0, 0xb9, 32, 3);
    t.Short(0, 0xc9, 8, 0); // room kit
    t.Short(0, 0xb9, 7, 110);
    // RPN 0: bend range 12 semitones on ch2 (index 1)
    t.Short(0, 0xb1, 101, 0);
    t.Short(0, 0xb1, 100, 0);
    t.Short(0, 0xb1, 6, 12);
    t.Short(0, 0xb1, 38, 0);

    t.Short(12, 0x90, 60, 100);
    t.Short(12, 0x90, 64, 90);
    t.Short(12, 0x90, 67, 80);
    t.Short(12, 0x91, 40, 110);
    t.Short(12, 0x99, 36, 120);
    t.Short(12, 0x99, 42, 90);
    t.Short(30, 0x92, 72, 127);
    t.Short(30, 0x93, 76, 70);
    t.Short(30, 0xb0, 64, 127); // sustain on
    t.Short(50, 0x80, 60, 0);
    t.Short(50, 0x80, 64, 0);
    t.Short(50, 0x90, 67, 0); // note-on velocity 0 = off
    t.Short(50, 0x94, 69, 100);
    t.Short(50, 0xe1, 0x00, 0x50);
    t.Short(50, 0xb2, 1, 127); // modulation -> LFO
    t.Short(80, 0x99, 38, 110);
    t.Short(80, 0x99, 42, 80);
    t.Short(80, 0x95, 84, 90);
    t.Short(80, 0x96, 79, 100);
    t.Short(80, 0xe1, 0x7f, 0x7f);
    t.Short(110, 0xe1, 0x00, 0x00);
    t.Short(110, 0x97, 57, 95);
    t.Short(110, 0xb3, 11, 64);
    t.Short(110, 0xb4, 10, 0);
    t.Short(140, 0xb0, 64, 0); // sustain off
    t.Short(140, 0x99, 49, 127);
    t.Short(140, 0x81, 40, 64);
    t.Short(170, 0xc5, 81, 0);
    t.Short(170, 0x95, 88, 100);
    t.Short(170, 0x99, 46, 100);
    t.Short(200, 0x82, 72, 0);
    t.Short(200, 0x83, 76, 0);
    t.Short(200, 0x84, 69, 0);
    t.Short(200, 0xe1, 0x00, 0x40);
    t.Short(230, 0x90, 62, 60);
    t.Short(230, 0x90, 65, 60);
    t.Short(230, 0x90, 69, 60);
    t.Short(230, 0x99, 36, 100);
    t.Short(260, 0xb0, 7, 60);
    t.Short(260, 0xb0, 10, 0);
    static const u8 kMasterVolume[] = {0x7f, 0x7f, 0x04, 0x01, 0x00, 0x60, 0xf7};
    t.SysEx(280, kMasterVolume, sizeof(kMasterVolume));
    t.Short(300, 0x80, 62, 0);
    t.Short(300, 0x80, 65, 0);
    t.Short(300, 0x80, 69, 0);
    t.Short(300, 0x85, 88, 0);
    t.Short(300, 0x86, 79, 0);
    t.Short(300, 0x87, 57, 0);
    t.Short(340, 0xb2, 121, 0);
    t.Short(340, 0xb9, 123, 0);
    static const u8 kEnd[] = {0};
    t.Meta(380, 0x2f, kEnd, 0);
    (void)kEnd;

    const u32 trackBytes = w.size() - trackStart;
    if (!w.ok() || trackStart < 4)
        return 0;
    tdls::wr32be(w.at(trackStart - 4), trackBytes);
    return w.size();
}

// ---- PCM scripts.  Each mirrors one dls_render_tool command (pcCommand).
const Script kScripts[] = {
    {"plain_th06_02", "th06_02.mid", {60000u, 256u, 0, 0u}, "--mid th06_02.mid --seconds 60 --chunk 256"},
    {"fade_stop_th06_10", "th06_10.mid", {90000u, 512u, kFadeStopEvents, 2u},
     "--mid th06_10.mid --seconds 90 --fade-at-ms 60000 --fade-ms 3000 --stop-at-ms 80000 --chunk 512"},
    {"odd_chunk_th06_14", "th06_14.mid", {45000u, 37u, 0, 0u}, "--mid th06_14.mid --seconds 45 --chunk 37"},
};
const u32 kScriptCount = sizeof(kScripts) / sizeof(kScripts[0]);

bool RunScript(const Script &script, const ScriptHost &host, const Settings &settings, ScriptResult &out)
{
    memset(&out, 0, sizeof(out));
    const u8 *smf = 0;
    u32 smfBytes = 0;
    if (!host.findSong || !host.findSong(host.user, script.song, &smf, &smfBytes))
    {
        out.error = "song not found";
        return false;
    }
    LocalSink *sink = new (std::nothrow) LocalSink();
    s16 *pcm = static_cast<s16 *>(malloc(script.schedule.chunkFrames * 4u));
    bool ok = sink && pcm && script.schedule.chunkFrames <= 1024u &&
              sink->Init(host.blob, host.blobBytes, settings, false);
    if (!ok)
        out.error = "synth init failed";
    tdls::Sha256 hash;
    hash.init();
    if (ok)
        ok = RunSchedule(script.schedule, smf, smfBytes, settings.synthRate, *sink, pcm, hash, &out.frames,
                         &out.error);
    if (ok)
    {
        hash.finish(out.pcmSha256);
        out.noteOns = sink->stats().noteOns;
        out.notesLost = sink->stats().notesLost;
        out.maxActive = sink->stats().maxActive;
    }
    free(pcm);
    delete sink;
    return ok;
}

bool RunSelfTestScript(const ScriptHost &host, const Settings &settings, ScriptResult &out)
{
    memset(&out, 0, sizeof(out));
    u8 *smf = static_cast<u8 *>(malloc(kSelfTestSmfMaxBytes));
    const u32 smfBytes = smf ? BuildSelfTestSmf(smf, kSelfTestSmfMaxBytes) : 0;
    LocalSink *sink = new (std::nothrow) LocalSink();
    s16 *pcm = static_cast<s16 *>(malloc(kSelfTestSchedule.chunkFrames * 4u));
    bool ok = smfBytes != 0 && sink && pcm && sink->Init(host.blob, host.blobBytes, settings, true);
    if (!ok)
        out.error = "self-test init failed";
    tdls::Sha256 hash;
    hash.init();
    if (ok)
        ok = RunSchedule(kSelfTestSchedule, smf, smfBytes, 44100u, *sink, pcm, hash, &out.frames, &out.error);
    if (ok)
    {
        hash.finish(out.pcmSha256);
        out.noteOns = sink->stats().noteOns;
    }
    free(pcm);
    delete sink;
    free(smf);
    return ok;
}

} // namespace th06midi
