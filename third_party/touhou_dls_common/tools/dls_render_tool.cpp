// PC 用ツール（STL 可）: TDSB ブロブと SMF から、ゲームと同じ 1ms 駆動で WAV を作る。
//   render    : 1 曲を鳴らして WAV/ハッシュ/集計を出す（--out を付けなければ WAV は書かない）
//   fadecheck : フェードの倍率（x87 の double→float）を整数で再現できているか PC の浮動小数と全数照合
// 出力（WAV）は gm.dls の派生物なので private/ にだけ置くこと。何も公開しない。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "tdls/blob.hpp"
#include "tdls/sha256.hpp"
#include "tdls/subset.hpp"
#include "tdls/synth.hpp"
#include "tdls/zun_seq.hpp"

using namespace tdls;

namespace {

bool loadFile(const std::string &path, std::vector<u8> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0)
    {
        fclose(f);
        return false;
    }
    out.resize((size_t)n);
    size_t got = n ? fread(out.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    return got == (size_t)n;
}

bool writeWav(const std::string &path, const std::vector<s16> &pcm, u32 rate)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    u32 dataBytes = (u32)(pcm.size() * 2);
    u8 h[44];
    memcpy(h, "RIFF", 4);
    wr32le(h + 4, 36 + dataBytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    wr32le(h + 16, 16);
    wr16le(h + 20, 1);
    wr16le(h + 22, 2);
    wr32le(h + 24, rate);
    wr32le(h + 28, rate * 4);
    wr16le(h + 32, 4);
    wr16le(h + 34, 16);
    memcpy(h + 36, "data", 4);
    wr32le(h + 40, dataBytes);
    fwrite(h, 1, 44, f);
    std::vector<u8> le(dataBytes);
    for (size_t i = 0; i < pcm.size(); i++) wr16le(&le[i * 2], (u16)pcm[i]);
    fwrite(le.data(), 1, le.size(), f);
    fclose(f);
    return true;
}

std::vector<s16> doubleRate(const std::vector<s16> &pcm, bool linear)
{
    std::vector<s16> result(pcm.size() * 2u);
    for (size_t i = 0; i < pcm.size(); i += 2u)
    {
        for (size_t ch = 0; ch < 2u; ch++)
        {
            s16 v = pcm[i + ch];
            result[i * 2u + ch] = v;
            s32 next = i + 2u < pcm.size() ? pcm[i + 2u + ch] : v;
            // 繰り返し: TH08 SC の先例。線形: 中間標本を整数の平均（0 方向の切り捨て）で作る比較候補。
            result[i * 2u + 2u + ch] = linear ? (s16)(((s32)v + next) / 2) : v;
        }
    }
    return result;
}

std::string pcmSha(const std::vector<s16> &pcm)
{
    // 機種に依存しないよう LE バイト列のハッシュ
    Sha256 s;
    s.init();
    u8 buf[4096];
    size_t i = 0;
    while (i < pcm.size())
    {
        size_t k = 0;
        for (; k < sizeof(buf) / 2 && i < pcm.size(); k++, i++) wr16le(buf + k * 2, (u16)pcm[i]);
        s.update(buf, (u32)(k * 2));
    }
    u8 d[32];
    s.finish(d);
    char hex[65];
    sha256Hex(d, hex);
    return hex;
}

void usage()
{
    fprintf(stderr,
            "usage:\n"
            "  dls_render_tool render --blob <x.tdsb> --mid <song.mid> [--init <init.mid> --init-ms N]\n"
            "      [--seconds S | --until-jump K [--tail-ms T]] [--fade-at-ms A --fade-ms D] [--stop-at-ms X]\n"
            "      [--rule A|B|C] [--voices N] [--reserve R] [--gain-adjust VREL] [--master-q16 G]\n"
            "      [--rate HZ] [--chunk FRAMES] [--no-tempo-quirk] [--out <x.wav>] [--event-trace <x.txt>]\n"
            "      [--quantize-ms 1|10] [--phase 0..9] [--msgs-offline [--loops N]] [--frames N]\n"
            "      [--output-rate HZ --upsample repeat|linear] (output-rate must be twice the synth rate)\n"
            "        --phase implies --quantize-ms 10; --msgs-offline stops at end/loop limit, sends CC123,\n"
            "        and starts timer k=0 (default comparison rate=22050, gain-adjust=0, quantize-ms=1).\n"
            "  dls_render_tool fadecheck\n");
}

struct Renderer
{
    Synth synth;
    ZunSequencer seq;
    ZunCommandQueue queue;
    ZunMidiPlayer player;
    std::vector<u8> arenaMem;
    Arena arena;
    std::vector<s16> pcm;
    u64 frame = 0;
    u32 rate = 44100;
    u32 chunk = 256;
    double renderSec = 0;
    u32 maxActive = 0;

    u64 frameOfMs(u64 ms) const { return ms * rate / 1000u; }

    void renderTo(u64 target)
    {
        while (frame < target)
        {
            u64 n = target - frame;
            if (n > chunk) n = chunk;
            size_t at = pcm.size();
            pcm.resize(at + (size_t)n * 2);
            auto t0 = std::chrono::steady_clock::now();
            player.render(&pcm[at], (u32)n);
            auto t1 = std::chrono::steady_clock::now();
            renderSec += std::chrono::duration<double>(t1 - t0).count();
            u32 act = synth.activeVoices();
            if (act > maxActive) maxActive = act;
            frame += n;
        }
    }

    void push(u32 type, u32 arg, const std::vector<u8> *data)
    {
        ZunCommand c;
        c.type = type;
        c.arg = arg;
        c.data = data ? data->data() : nullptr;
        c.size = data ? (u32)data->size() : 0;
        if (!queue.push(c))
        {
            fprintf(stderr, "command queue full\n");
            exit(1);
        }
    }
};

int cmdRender(int argc, char **argv)
{
    std::string blobPath, midPath, initPath, outPath, tracePath, eventTracePath;
    double seconds = 0;
    u64 exactFrames = 0;
    long untilJump = -1, untilEnd = 0, tailMs = 3000, initMs = 2000, fadeAt = -1, fadeMs = 0, stopAt = -1;
    char rule = 'A';
    SynthConfig cfg;
    synthDefaultConfig(cfg);
    u32 chunk = 256;
    u32 quantizeMs = 0, phase = 0, loops = 1, outputRate = 0;
    bool tempoQuirk = true, quiet = false, msgsOffline = false, rateSet = false, gainSet = false;
    bool linearUpsample = false;
    for (int i = 2; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&](void) -> const char * {
            if (i + 1 >= argc)
            {
                usage();
                exit(64);
            }
            return argv[++i];
        };
        if (a == "--blob") blobPath = next();
        else if (a == "--drum-hold") cfg.drumHold = 1;
        else if (a == "--mid") midPath = next();
        else if (a == "--init") initPath = next();
        else if (a == "--init-ms") initMs = atol(next());
        else if (a == "--seconds") seconds = atof(next());
        else if (a == "--frames") exactFrames = (u64)strtoull(next(), nullptr, 0);
        else if (a == "--until-jump") untilJump = atol(next());
        else if (a == "--until-end") untilEnd = 1; // CC4 2 回目か、ループの無い曲は頭に戻った所まで
        else if (a == "--tail-ms") tailMs = atol(next());
        else if (a == "--fade-at-ms") fadeAt = atol(next());
        else if (a == "--fade-ms") fadeMs = atol(next());
        else if (a == "--stop-at-ms") stopAt = atol(next());
        else if (a == "--rule") rule = next()[0];
        else if (a == "--voices") cfg.voices = (u32)atol(next());
        else if (a == "--reserve") cfg.reserveVoices = (u32)atol(next());
        else if (a == "--gain-adjust") { cfg.gainAdjust = (s32)atol(next()); gainSet = true; }
        else if (a == "--master-q16") cfg.masterGainQ16 = (u32)strtoul(next(), nullptr, 0);
        else if (a == "--rate") { cfg.sampleRate = (u32)atol(next()); rateSet = true; }
        else if (a == "--chunk") chunk = (u32)atol(next());
        else if (a == "--quantize-ms") quantizeMs = (u32)atol(next());
        else if (a == "--phase" || a == "--quantize-phase")
        {
            phase = (u32)atol(next());
            if (!quantizeMs) quantizeMs = 10;
        }
        else if (a == "--msgs-offline") msgsOffline = true;
        else if (a == "--loops") loops = (u32)atol(next());
        else if (a == "--output-rate") outputRate = (u32)atol(next());
        else if (a == "--upsample")
        {
            std::string method = next();
            if (method != "repeat" && method != "linear")
            {
                fprintf(stderr, "upsample must be repeat or linear\n");
                return 64;
            }
            linearUpsample = method == "linear";
        }
        else if (a == "--no-tempo-quirk") tempoQuirk = false;
        else if (a == "--out") outPath = next();
        else if (a == "--trace") tracePath = next();
        else if (a == "--event-trace") eventTracePath = next();
        else if (a == "--quiet") quiet = true;
        else
        {
            fprintf(stderr, "bad arg %s\n", a.c_str());
            usage();
            return 64;
        }
    }
    if (msgsOffline)
    {
        if (!rateSet) cfg.sampleRate = 22050;
        if (!gainSet) cfg.gainAdjust = 0;
        if (!quantizeMs) quantizeMs = 1;
    }
    if (untilEnd) untilJump = 2;
    if (blobPath.empty() || midPath.empty() || (seconds <= 0 && exactFrames == 0 && untilJump < 0) || chunk == 0 ||
        (quantizeMs != 0 && quantizeMs != 1 && quantizeMs != 10) ||
        (quantizeMs == 0 ? phase != 0 : phase >= quantizeMs) ||
        (outputRate && outputRate != cfg.sampleRate && outputRate != cfg.sampleRate * 2u) ||
        (exactFrames != 0 && (untilJump >= 0 || !initPath.empty() || fadeAt >= 0 || stopAt >= 0)))
    {
        usage();
        return 64;
    }
    cfg.mapRule = rule == 'B' ? MAP_B_SUBCAPITAL : (rule == 'C' ? MAP_C_STRICT_LSB : MAP_A_CAPITAL);
    std::vector<u8> blob, mid, init;
    if (!loadFile(blobPath, blob) || !loadFile(midPath, mid) || (!initPath.empty() && !loadFile(initPath, init)))
    {
        fprintf(stderr, "cannot read input\n");
        return 1;
    }
    TdsbView view;
    TdsbStatus st = tdsbOpen(blob.data(), (u32)blob.size(), view);
    if (st != TDSB_OK)
    {
        fprintf(stderr, "tdsbOpen: %s\n", tdsbStatusName(st));
        return 1;
    }
    Renderer r;
    r.rate = cfg.sampleRate;
    r.chunk = chunk;
    u32 need = Synth::memoryNeeded(view, cfg);
    r.arenaMem.resize(need);
    r.arena.init(r.arenaMem.data(), need);
    if (!r.synth.init(view, cfg, r.arena))
    {
        fprintf(stderr, "synth init failed (need %u)\n", need);
        return 1;
    }
    r.seq.setTempoQuirk(tempoQuirk);
    if (msgsOffline) r.seq.setPlaybackLimit(loops, true);
    if (!r.player.init(&r.synth, &r.seq, &r.queue))
    {
        fprintf(stderr, "player init failed\n");
        return 1;
    }
    if (!r.player.setEventQuantization(quantizeMs, phase))
    {
        fprintf(stderr, "bad event quantization\n");
        return 64;
    }
    if (exactFrames && !r.player.setRenderFrameLimit(exactFrames)) return 64;
    // ゲームの流れ: （init.mid を鳴らす）→ 曲を PlayFile
    u64 songStartMs = 0;
    if (!init.empty())
    {
        r.push(ZUN_CMD_PLAY, 0, &init);
        songStartMs = (u64)initMs;
        r.renderTo(r.frameOfMs(songStartMs));
    }
    r.seq.clearStats();
    r.synth.clearStats();
    std::string traceText, eventTraceText;
    struct TraceState
    {
        std::string *legacy;
        std::string *events;
        ZunMidiPlayer *player;
    } traceState = {&traceText, &eventTraceText, &r.player};
    if (!tracePath.empty() || !eventTracePath.empty())
    {
        // 曲の分だけ: 「OnTimerElapsed の通し番号（曲の頭から） バイト列」
        r.seq.setTrace(
            [](void *u, u32 call, const u8 *m, u32 len) {
                TraceState &state = *(TraceState *)u;
                std::string &t = *state.legacy, &events = *state.events;
                char b[48];
                snprintf(b, sizeof(b), "%u", call);
                t += b;
                snprintf(b, sizeof(b), "%u %llu", call, (unsigned long long)state.player->eventSampleFrame());
                events += b;
                for (u32 i = 0; i < len; i++)
                {
                    snprintf(b, sizeof(b), " %02X", m[i]);
                    t += b;
                    events += b;
                }
                t += '\n';
                events += '\n';
            },
            &traceState);
    }
    r.push(ZUN_CMD_PLAY, 0, &mid);
    // 予定（曲の頭からの ms）
    struct Ev
    {
        u64 ms;
        u32 type;
        u32 arg;
    };
    std::vector<Ev> evs;
    if (fadeAt >= 0) evs.push_back({songStartMs + (u64)fadeAt, ZUN_CMD_FADE, (u32)fadeMs});
    if (stopAt >= 0) evs.push_back({songStartMs + (u64)stopAt, ZUN_CMD_STOP, 0});
    std::sort(evs.begin(), evs.end(), [](const Ev &a, const Ev &b) { return a.ms < b.ms; });
    size_t evi = 0;
    // targetMs の境目まで進める。途中の予定はその ms の境目で受け箱へ入れる（次の 1ms 境界で効く）
    auto stepTo = [&](u64 targetMs) {
        while (evi < evs.size() && evs[evi].ms <= targetMs)
        {
            r.renderTo(r.frameOfMs(evs[evi].ms));
            r.push(evs[evi].type, evs[evi].arg, nullptr);
            evi++;
        }
        r.renderTo(r.frameOfMs(targetMs));
    };
    u64 endMs;
    if (untilJump >= 0)
    {
        // 1ms ずつ進めて CC4 の回数を見る（停止位置は 1ms 境界で決まるので chunk に依らない）
        u64 ms = songStartMs;
        u64 limit = songStartMs + 20u * 60u * 1000u;
        while (r.seq.stats().loopJumps < (u32)untilJump && ms < limit &&
               !(untilEnd && r.seq.stats().restarts > 0) && !r.seq.limitReached())
        {
            ms++;
            stepTo(ms);
        }
        endMs = ms + (u64)tailMs;
    }
    else
    {
        endMs = songStartMs + (u64)(seconds * 1000.0 + 0.5);
    }
    if (exactFrames) r.renderTo(exactFrames);
    else stepTo(endMs);

    // 集計
    s32 peak = 0;
    u64 clipped = 0;
    for (s16 v : r.pcm)
    {
        s32 a = v < 0 ? -(s32)v : (s32)v;
        if (a > peak) peak = a;
        if (v == 32767 || v == -32768) clipped++;
    }
    double audioSec = (double)(r.pcm.size() / 2) / r.rate;
    const SynthStats &ss = r.synth.stats();
    const ZunSeqStats &qs = r.seq.stats();
    printf("pcm_sha256 %s\n", pcmSha(r.pcm).c_str());
    printf("frames %llu seconds %.3f rate %u chunk %u arena %u\n", (unsigned long long)(r.pcm.size() / 2), audioSec,
           r.rate, chunk, need);
    if (quantizeMs)
    {
        printf("timing quantize_ms=%u phase=%u render_block=%u offline=%u", quantizeMs, phase, r.rate / 100u,
               msgsOffline ? 1u : 0u);
        if (r.player.limitStopTimerMs() != ~0ull)
            printf(" stopped_timer=%llu", (unsigned long long)r.player.limitStopTimerMs());
        printf("\n");
    }
    printf("peak %d clipped %llu max_active %u voices %u reserve %u\n", peak, (unsigned long long)clipped, r.maxActive,
           cfg.voices, cfg.reserveVoices);
    printf("synth note_on=%u started=%u unmapped=%u fallback=%u no_region=%u lost=%u stolen=%u quick_stops=%u "
           "resets=%u sysex_ignored=%u nrpn_stored=%u loop_killed=%u max_active=%u\n",
           ss.noteOns, ss.notesStarted, ss.notesUnmapped, ss.notesFallback, ss.notesNoRegion, ss.notesLost,
           ss.voicesStolen, ss.quickStops, ss.resets, ss.sysExIgnored, ss.nrpnStored, ss.loopKilled, ss.maxActive);
    printf("seq timer_calls=%u messages=%u note_ons=%u sysex=%u tempo_events=%u loop_saves=%u loop_jumps=%u "
           "notes_at_jump1=%u notes_at_jump2=%u restarts=%u fade_sends=%u overruns=%u tempo=%d\n",
           qs.timerCalls, qs.messages, qs.noteOns, qs.sysEx, qs.tempoEvents, qs.loopSaves, qs.loopJumps,
           qs.noteOnsAtJump[0], qs.noteOnsAtJump[1], qs.restarts, qs.fadeVolumeSends, qs.overruns, r.seq.tempo());
    printf("level mix_peak=%u (%.2f dBFS before clip) clipped_samples=%u notes_at_end=%u\n", ss.mixPeak,
           20.0 * std::log10((ss.mixPeak ? ss.mixPeak : 1) / 32767.0), ss.clippedSamples,
           qs.loopJumps >= 2 ? qs.noteOnsAtJump[1] : qs.noteOns);
    printf("cpu render_ms %.1f per_audio_sec_ms %.3f\n", r.renderSec * 1000.0, r.renderSec * 1000.0 / audioSec);
    if (!tracePath.empty())
    {
        FILE *tf = fopen(tracePath.c_str(), "wb");
        if (!tf) return 1;
        fwrite(traceText.data(), 1, traceText.size(), tf);
        fclose(tf);
    }
    if (!eventTracePath.empty())
    {
        FILE *tf = fopen(eventTracePath.c_str(), "wb");
        if (!tf) return 1;
        const char header[] = "# timer_call (1-based) applied_sample_frame bytes\n";
        fwrite(header, 1, sizeof(header) - 1u, tf);
        fwrite(eventTraceText.data(), 1, eventTraceText.size(), tf);
        fclose(tf);
    }
    if (!outPath.empty())
    {
        std::vector<s16> upsampled;
        const std::vector<s16> *wavPcm = &r.pcm;
        if (outputRate == r.rate * 2u)
        {
            upsampled = doubleRate(r.pcm, linearUpsample);
            wavPcm = &upsampled;
            printf("output frames=%llu rate=%u upsample=%s pcm_sha256=%s\n",
                   (unsigned long long)(upsampled.size() / 2u), outputRate,
                   linearUpsample ? "linear" : "repeat", pcmSha(upsampled).c_str());
        }
        if (!writeWav(outPath, *wavPcm, outputRate ? outputRate : r.rate))
        {
            fprintf(stderr, "cannot write %s\n", outPath.c_str());
            return 1;
        }
        if (!quiet) printf("wav %s\n", outPath.c_str());
    }
    printf("player pending_peak=%u pending_capacity=%u pending_bytes=%u pending_overflows=%u failed=%u\n",
           r.player.pendingPeak(), (unsigned)ZunMidiPlayer::kPendingCapacity,
           (unsigned)ZunMidiPlayer::kPendingBytes, r.player.pendingOverflows(),
           r.player.outputFailed() ? 1u : 0u);
    return r.player.outputFailed() ? 1 : 0;
}

int cmdFadeCheck()
{
    // PC（x86-64 の SSE2 = IEEE double/float）で 1.0f - (f32)e/(f32)d を 53bit で計算して float にした値と比べる
    const u32 durs[] = {1, 2, 3, 5, 7, 10, 100, 127, 128, 129, 255, 256, 500, 999, 1000, 1001, 1500, 2000, 2500,
                        3000, 4000, 5000, 7777, 10000, 65537, 100000, 1000000};
    u64 checked = 0, bad = 0;
    for (u32 d : durs)
    {
        for (u32 e = 0; e < d; e += (d > 100000 ? 7u : 1u))
        {
            volatile double q = (double)e / (double)d;
            volatile double rr = 1.0 - q;
            volatile float f = (float)rr;
            ZunFadeMul m = zunFadeMultiplier(e, d);
            double mine = std::ldexp((double)m.mant, -(int)m.shift);
            if (mine != (double)f)
            {
                if (bad < 10) printf("mismatch value e=%u d=%u host=%.12g mine=%.12g\n", e, d, (double)f, mine);
                bad++;
            }
            for (u32 v = 0; v <= 128; v++)
            {
                u32 host = (u32)((double)v * (double)f);
                if (host != zunFadeApply(m, v))
                {
                    if (bad < 10) printf("mismatch floor e=%u d=%u v=%u host=%u mine=%u\n", e, d, v, host,
                                         zunFadeApply(m, v));
                    bad++;
                }
            }
            checked++;
        }
    }
    printf("fadecheck checked=%llu mismatches=%llu\n", (unsigned long long)checked, (unsigned long long)bad);
    return bad ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage();
        return 64;
    }
    std::string cmd = argv[1];
    if (cmd == "render") return cmdRender(argc, argv);
    if (cmd == "fadecheck") return cmdFadeCheck();
    usage();
    return 64;
}
