#include "tdls/manifest.hpp"

#include <stdarg.h>
#include <string.h>

namespace tdls {

namespace {

// libc の printf に頼らない最小の書式（機種ごとの差・ロケールを入れないため）。
// 対応: %u %d %x %s %c %% と 0 埋め・桁数（例 %08x %04x %3u）。引数は unsigned/int/const char*。
// 戻り値は書いた文字数（cap-1 で切れたら -1）。
int formatInto(char *out, u32 cap, const char *fmt, va_list ap)
{
    u32 n = 0;
    bool overflow = false;
    auto put = [&](char c) {
        if (n + 1 < cap)
        {
            out[n++] = c;
        }
        else
        {
            overflow = true;
        }
    };
    for (const char *p = fmt; *p; p++)
    {
        if (*p != '%')
        {
            put(*p);
            continue;
        }
        p++;
        bool zero = false;
        u32 width = 0;
        if (*p == '0')
        {
            zero = true;
            p++;
        }
        while (*p >= '0' && *p <= '9')
        {
            width = width * 10u + (u32)(*p - '0');
            p++;
        }
        char digits[16];
        u32 nd = 0;
        bool neg = false;
        switch (*p)
        {
        case 'u':
        case 'x':
        {
            unsigned v = va_arg(ap, unsigned);
            u32 base = *p == 'x' ? 16u : 10u;
            do
            {
                u32 dgt = v % base;
                digits[nd++] = (char)(dgt < 10 ? '0' + dgt : 'a' + (dgt - 10));
                v /= base;
            } while (v);
            break;
        }
        case 'd':
        {
            int v = va_arg(ap, int);
            unsigned m = v < 0 ? 0u - (unsigned)v : (unsigned)v;
            neg = v < 0;
            do
            {
                digits[nd++] = (char)('0' + m % 10u);
                m /= 10u;
            } while (m);
            break;
        }
        case 's':
        {
            const char *s = va_arg(ap, const char *);
            u32 len = (u32)strlen(s);
            for (u32 i = len; i < width; i++) put(' ');
            for (u32 i = 0; i < len; i++) put(s[i]);
            continue;
        }
        case 'c':
            put((char)va_arg(ap, int));
            continue;
        case '%':
            put('%');
            continue;
        default:
            return -1; // 使っていない書式は受けない
        }
        u32 len = nd + (neg ? 1u : 0u);
        if (!zero)
        {
            for (u32 i = len; i < width; i++) put(' ');
            if (neg) put('-');
        }
        else
        {
            if (neg) put('-');
            for (u32 i = len; i < width; i++) put('0');
        }
        while (nd) put(digits[--nd]);
    }
    out[n] = 0;
    return overflow ? -1 : (int)n;
}

} // namespace

void ManifestWriter::begin(TextSink *sink)
{
    sink_ = sink;
    sha_.init();
    overflow_ = false;
}

void ManifestWriter::line(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = formatInto(buf, (u32)sizeof(buf) - 1u, fmt, ap); // 改行の 1 バイトを残す
    va_end(ap);
    if (n < 0)
    {
        overflow_ = true;
        n = (int)strlen(buf);
    }
    buf[n] = '\n';
    u32 len = (u32)n + 1u;
    sha_.update(buf, len);
    if (sink_)
    {
        sink_->put(buf, len);
    }
}

void ManifestWriter::finish(char hexOut[65])
{
    u8 d[32];
    sha_.finish(d);
    sha256Hex(d, hexOut);
    char buf[80];
    memcpy(buf, "sha256 ", 7);
    memcpy(buf + 7, hexOut, 64);
    buf[71] = '\n';
    if (sink_)
    {
        sink_->put(buf, 72);
    }
}

namespace {

void hex32(const u8 d[32], char out[65]) { sha256Hex(d, out); }

} // namespace

void writeUsageList(ManifestWriter &w, const char *work, SmfMode mode, const InputFile *init,
                    const InputFile *files, u32 fileCount, const UsageSet &u)
{
    char h[65];
    w.line("tdls-usage 1");
    w.line("work %s", work);
    w.line("mode %s", mode == SMF_MODE_GAME ? "game" : "linear");
    if (init)
    {
        hex32(init->sha256, h);
        w.line("init %s %u %s", init->name, (unsigned)init->size, h);
    }
    for (u32 i = 0; i < fileCount; i++)
    {
        hex32(files[i].sha256, h);
        w.line("file %s %u %s", files[i].name, (unsigned)files[i].size, h);
    }
    const UsageStats &s = u.stats;
    w.line("stats notes=%u no_program=%u pending_bank=%u gs_reset=%u gm_on=%u sys_mode=%u rhythm_set=%u "
           "tone_number=%u rx_channel=%u rx_nonidentity=%u gs_checksum_err=%u loop_start=%u loop_jump=%u "
           "loop_rs_mismatch=%u f7=%u max_poly=%u",
           (unsigned)s.notes, (unsigned)s.notesNoProgram, (unsigned)s.notesPendingBank, (unsigned)s.gsReset,
           (unsigned)s.gmOn, (unsigned)s.sysModeSet, (unsigned)s.rhythmPartSet, (unsigned)s.toneNumberSet,
           (unsigned)s.rxChannelSet, (unsigned)s.rxChannelNonIdentity, (unsigned)s.gsChecksumErrors,
           (unsigned)s.loopStarts, (unsigned)s.loopJumps, (unsigned)s.loopRunningStatusMismatch,
           (unsigned)s.f7Events, (unsigned)s.maxPolyphony);
    for (u32 t = 0; t < u.count; t++)
    {
        const ToneUse &tu = u.tones[t];
        w.line("tone d=%u msb=%u lsb=%u prog=%u notes=%u key=%u-%u vel=%u-%u ch=%04x files=%08x "
               "bits=%08x%08x%08x%08x",
               (unsigned)tu.key.drum, (unsigned)tu.key.msb, (unsigned)tu.key.lsb, (unsigned)tu.key.program,
               (unsigned)tu.noteCount, (unsigned)tu.noteLo, (unsigned)tu.noteHi, (unsigned)tu.velLo,
               (unsigned)tu.velHi, (unsigned)tu.channelMask, (unsigned)tu.fileMask, (unsigned)tu.noteBits[0],
               (unsigned)tu.noteBits[1], (unsigned)tu.noteBits[2], (unsigned)tu.noteBits[3]);
    }
    w.line("end %u", (unsigned)u.count);
}

void subsetPcmTotals(const DlsFile &f, const SubsetPlan &p, u32 *pcm16, u32 *pcm8, u32 *pcm16Trim, u32 *pcm8Trim)
{
    u32 a = 0, b = 0, c = 0, d = 0;
    for (u32 w = 0; w < f.waveCount; w++)
    {
        if (!p.waveUsed[w])
        {
            continue;
        }
        const DlsWave &wv = f.waves[w];
        u32 align = wv.blockAlign ? wv.blockAlign : 1u;
        u32 samples = wv.dataBytes / align;
        a += samples * 2u;
        b += samples;
        c += p.waveKeepSamples[w] * 2u;
        d += p.waveKeepSamples[w];
    }
    *pcm16 = a;
    *pcm8 = b;
    *pcm16Trim = c;
    *pcm8Trim = d;
}

void writeSubsetManifest(ManifestWriter &w, const DlsFile &f, const u8 dlsSha256[32], const char *usageSha256Hex,
                         const UsageSet &u, const SubsetPlan &p, const BlobInfo *blobs, u32 blobCount)
{
    char h[65];
    hex32(dlsSha256, h);
    w.line("tdls-subset 1");
    w.line("dls %u %s", (unsigned)f.fileSize, h);
    w.line("usage %s", usageSha256Hex);
    w.line("rule %s", mapRuleName(p.rule));
    for (u32 t = 0; t < u.count; t++)
    {
        const ToneKey &k = u.tones[t].key;
        w.line("map d=%u msb=%u lsb=%u prog=%u -> ins=%d how=%s", (unsigned)k.drum, (unsigned)k.msb,
               (unsigned)k.lsb, (unsigned)k.program, (int)p.toneInstrument[t], mapHowName((MapHow)p.toneHow[t]));
    }
    for (u32 t = 0; t < u.count; t++)
    {
        const u32 *b = p.toneUncovered + t * 4;
        if (b[0] | b[1] | b[2] | b[3])
        {
            const ToneKey &k = u.tones[t].key;
            w.line("uncovered d=%u msb=%u lsb=%u prog=%u bits=%08x%08x%08x%08x", (unsigned)k.drum,
                   (unsigned)k.msb, (unsigned)k.lsb, (unsigned)k.program, (unsigned)b[0], (unsigned)b[1],
                   (unsigned)b[2], (unsigned)b[3]);
        }
    }
    for (u32 i = 0; i < f.instrumentCount; i++)
    {
        if (!p.insUsed[i])
        {
            continue;
        }
        const DlsInstrument &ins = f.instruments[i];
        u32 sel = 0;
        for (u32 k = 0; k < ins.regionCount; k++) sel += p.rgnUsed[ins.firstRegion + k];
        w.line("ins %u d=%u msb=%u lsb=%u prog=%u regions=%u/%u", (unsigned)i, ins.isDrum() ? 1u : 0u,
               (unsigned)ins.bankMsb(), (unsigned)ins.bankLsb(), (unsigned)ins.prog(), (unsigned)sel,
               (unsigned)ins.regionCount);
        for (u32 k = 0; k < ins.regionCount; k++)
        {
            u32 r = ins.firstRegion + k;
            if (p.rgnUsed[r])
            {
                const DlsRegion &rg = f.regions[r];
                w.line("rgn %u key=%u-%u wave=%u", (unsigned)r, (unsigned)rg.keyLo, (unsigned)rg.keyHi,
                       (unsigned)rg.waveIndex);
            }
        }
    }
    for (u32 i = 0; i < f.waveCount; i++)
    {
        if (!p.waveUsed[i])
        {
            continue;
        }
        const DlsWave &wv = f.waves[i];
        u32 align = wv.blockAlign ? wv.blockAlign : 1u;
        w.line("wave %u bytes=%u rate=%u bits=%u samples=%u keep=%u", (unsigned)i, (unsigned)wv.dataBytes,
               (unsigned)wv.sampleRate, (unsigned)wv.bitsPerSample, (unsigned)(wv.dataBytes / align),
               (unsigned)p.waveKeepSamples[i]);
    }
    u32 a, b, c, d;
    subsetPcmTotals(f, p, &a, &b, &c, &d);
    w.line("total ins=%u rgn=%u wave=%u pcm16=%u pcm8=%u pcm16_trim=%u pcm8_trim=%u", (unsigned)p.instruments,
           (unsigned)p.regions, (unsigned)p.waves, (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)d);
    for (u32 i = 0; i < blobCount; i++)
    {
        hex32(blobs[i].sha256, h);
        w.line("blob flags=%u size=%u sha256=%s", (unsigned)blobs[i].flags, (unsigned)blobs[i].size, h);
    }
}

} // namespace tdls
