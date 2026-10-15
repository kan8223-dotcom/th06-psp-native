// PC 用ツール（STL 可）。共通部品 tdls を使って
//   catalog : gm.dls の楽器・リージョン・wave の一覧
//   work    : 作品ごとの使用音色一覧・調査表・規則ごとのサブセット一覧とブロブのハッシュ
// を出す。出力は決まった順序・整数のみ・LF。gm.dls と派生物は private/ にだけ置くこと。
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "tdls/byte_source.hpp"
#include "tdls/dls.hpp"
#include "tdls/manifest.hpp"
#include "tdls/sha256.hpp"
#include "tdls/smf_scan.hpp"
#include "tdls/subset.hpp"

using namespace tdls;

namespace {

bool loadFile(const std::string &path, std::vector<u8> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
    {
        return false;
    }
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

std::string hexDigest(const void *p, size_t n)
{
    u8 d[32];
    char hex[65];
    sha256Buffer(p, (u32)n, d);
    sha256Hex(d, hex);
    return hex;
}

std::string trimName(const char *s)
{
    std::string r(s);
    while (!r.empty() && (r.back() == ' ' || r.back() == '\0'))
    {
        r.pop_back();
    }
    for (char &ch : r)
    {
        if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7f || ch == '"')
        {
            ch = '_';
        }
    }
    return r;
}

std::string fourccStr(u32 id)
{
    std::string s;
    for (int i = 0; i < 4; i++)
    {
        char ch = (char)((id >> (8 * i)) & 0xff);
        s += (ch >= 0x20 && ch < 0x7f) ? ch : '?';
    }
    return s;
}

struct Out
{
    std::string text;
    void line(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        text += buf;
        text += '\n';
    }
};

struct LoadedDls
{
    std::vector<u8> bytes;
    std::vector<u8> arenaMem;
    Arena arena;
    DlsFile f;
    std::string sha;
};

bool loadDls(const std::string &path, LoadedDls &d)
{
    if (!loadFile(path, d.bytes))
    {
        fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    d.sha = hexDigest(d.bytes.data(), d.bytes.size());
    d.arenaMem.resize(4u << 20);
    d.arena.init(d.arenaMem.data(), (u32)d.arenaMem.size());
    MemorySource src(d.bytes.data(), (u32)d.bytes.size());
    DlsStatus st = dlsParse(src, d.arena, d.f);
    if (st != DLS_OK)
    {
        fprintf(stderr, "dlsParse: %s at 0x%x\n", dlsStatusName(st), d.f.errorOffset);
        return false;
    }
    return true;
}

void wsmpText(char *buf, size_t cap, const DlsWsmp &w)
{
    snprintf(buf, cap, "unity=%u fine=%d atten=%d opt=0x%x loops=%u loop=%u:%u+%u", w.unityNote, w.fineTune,
             w.attenuation, w.options, w.loopCount, w.loop.type, w.loop.start, w.loop.length);
}

int cmdCatalog(const std::string &dlsPath, const std::string &outPath)
{
    LoadedDls d;
    if (!loadDls(dlsPath, d))
    {
        return 1;
    }
    const DlsFile &f = d.f;
    Out o;
    o.line("# tdls catalog v1 (gm.dls の派生メタデータ。private/ の外に出さない)");
    o.line("dls.size %u", f.fileSize);
    o.line("dls.sha256 %s", d.sha.c_str());
    if (f.hasVers)
    {
        o.line("dls.vers %u.%u.%u.%u", f.versMs >> 16, f.versMs & 0xffff, f.versLs >> 16, f.versLs & 0xffff);
    }
    o.line("dls.colh %u", f.colhCount);
    o.line("dls.instruments %u regions %u waves %u connections %u ptbl_cues %u", f.instrumentCount,
           f.regionCount, f.waveCount, f.connCount, f.ptblCueCount);
    o.line("dls.ptbl_base wvpl_list+%u", f.cueBase - f.wvplListOffset);
    o.line("dls.level2_chunks %u (rgn2/lar2/art2/cdl /dlid)", f.level2Chunks);
    std::string unk;
    for (u32 i = 0; i < f.unknownTopCount; i++)
    {
        unk += " " + fourccStr(f.unknownTopIds[i]);
    }
    o.line("dls.unknown_chunks %u ids%s", f.unknownChunks, unk.c_str());
    o.line("dls.multi_loop_wsmp %u regions_without_wsmp %u regions_without_wlnk %u", f.multiLoopWsmp,
           f.regionsWithoutWsmp, f.regionsWithoutWlnk);
    o.line("dls.info.name \"%s\"", trimName(f.infoName).c_str());

    // wave の形式ごとの集計
    std::map<std::string, std::pair<u32, u64>> fmts;
    u64 waveBytes = 0;
    for (u32 i = 0; i < f.waveCount; i++)
    {
        const DlsWave &w = f.waves[i];
        char key[96];
        snprintf(key, sizeof(key), "tag%u ch%u %uHz %ubit", w.formatTag, w.channels, w.sampleRate,
                 w.bitsPerSample);
        fmts[key].first++;
        fmts[key].second += w.dataBytes;
        waveBytes += w.dataBytes;
    }
    for (auto &kv : fmts)
    {
        o.line("waves.format %s count %u bytes %llu", kv.first.c_str(), kv.second.first,
               (unsigned long long)kv.second.second);
    }
    o.line("waves.total_bytes %llu", (unsigned long long)waveBytes);

    // 参照されない wave
    std::vector<u32> refs(f.waveCount, 0);
    u32 badIndex = 0;
    for (u32 r = 0; r < f.regionCount; r++)
    {
        if (f.regions[r].waveIndex < f.waveCount)
        {
            refs[f.regions[r].waveIndex]++;
        }
        else
        {
            badIndex++;
        }
    }
    u32 unref = 0;
    u64 unrefBytes = 0;
    for (u32 i = 0; i < f.waveCount; i++)
    {
        if (!refs[i])
        {
            unref++;
            unrefBytes += f.waves[i].dataBytes;
        }
    }
    o.line("waves.unreferenced %u bytes %llu ; regions_with_bad_table_index %u", unref,
           (unsigned long long)unrefBytes, badIndex);

    // art1 の接続の種類（source/control/destination/transform の組）ごとの件数と scale の範囲
    struct ConnStat
    {
        u32 count, inIns, inRgn;
        s32 lo, hi;
    };
    std::map<u64, ConnStat> kinds;
    auto addArt = [&](const DlsArt &a, bool fromIns) {
        for (u32 k = 0; k < a.connCount; k++)
        {
            const DlsConnection &c = f.conns[a.firstConn + k];
            u64 key = ((u64)c.source << 48) | ((u64)c.control << 32) | ((u64)c.destination << 16) | c.transform;
            auto it = kinds.find(key);
            if (it == kinds.end())
            {
                kinds[key] = ConnStat{0, 0, 0, c.scale, c.scale};
                it = kinds.find(key);
            }
            it->second.count++;
            (fromIns ? it->second.inIns : it->second.inRgn)++;
            if (c.scale < it->second.lo) it->second.lo = c.scale;
            if (c.scale > it->second.hi) it->second.hi = c.scale;
        }
    };
    for (u32 i = 0; i < f.instrumentCount; i++)
    {
        if (f.instruments[i].hasArt) addArt(f.instruments[i].art, true);
    }
    for (u32 r = 0; r < f.regionCount; r++)
    {
        if (f.regions[r].hasArt) addArt(f.regions[r].art, false);
    }
    for (auto &kv : kinds)
    {
        o.line("conn.kind src=0x%04x ctl=0x%04x dst=0x%04x trn=0x%04x count=%u (ins %u rgn %u) scale=%d..%d",
               (unsigned)(kv.first >> 48), (unsigned)((kv.first >> 32) & 0xffff),
               (unsigned)((kv.first >> 16) & 0xffff), (unsigned)(kv.first & 0xffff), kv.second.count,
               kv.second.inIns, kv.second.inRgn, kv.second.lo, kv.second.hi);
    }

    // 楽器（ファイル順）
    char wbuf[256];
    for (u32 i = 0; i < f.instrumentCount; i++)
    {
        const DlsInstrument &ins = f.instruments[i];
        o.line("ins %u drum=%u msb=%u lsb=%u prog=%u name=\"%s\" regions=%u/%u art=%u bank_raw=0x%08x prog_raw=0x%x",
               i, ins.isDrum() ? 1u : 0u, ins.bankMsb(), ins.bankLsb(), ins.prog(), trimName(ins.name).c_str(),
               ins.regionCount, ins.declaredRegions, ins.hasArt ? ins.art.connCount : 0u, ins.bank,
               ins.program);
        for (u32 k = 0; k < ins.regionCount; k++)
        {
            const DlsRegion &r = f.regions[ins.firstRegion + k];
            const DlsWsmp *ws = dlsEffectiveWsmp(f, r);
            if (ws)
            {
                wsmpText(wbuf, sizeof(wbuf), *ws);
            }
            else
            {
                snprintf(wbuf, sizeof(wbuf), "no-wsmp");
            }
            o.line("  rgn key=%u-%u vel=%u-%u opt=0x%x kgrp=%u wave=%u ch=%u wlnkopt=0x%x own_wsmp=%u %s art=%u",
                   r.keyLo, r.keyHi, r.velLo, r.velHi, r.options, r.keyGroup, r.waveIndex, r.channel,
                   r.wlnkOptions, r.hasWsmp, wbuf, r.hasArt ? r.art.connCount : 0u);
        }
    }
    for (u32 i = 0; i < f.waveCount; i++)
    {
        const DlsWave &w = f.waves[i];
        if (w.hasWsmp)
        {
            wsmpText(wbuf, sizeof(wbuf), w.wsmp);
        }
        else
        {
            snprintf(wbuf, sizeof(wbuf), "no-wsmp");
        }
        o.line("wave %u bytes=%u rate=%u bits=%u ch=%u refs=%u name=\"%s\" %s", i, w.dataBytes, w.sampleRate,
               w.bitsPerSample, w.channels, refs[i], trimName(w.name).c_str(), wbuf);
    }
    std::string h = hexDigest(o.text.data(), o.text.size());
    o.line("catalog.sha256 %s", h.c_str());
    FILE *fo = fopen(outPath.c_str(), "wb");
    if (!fo)
    {
        fprintf(stderr, "cannot write %s\n", outPath.c_str());
        return 1;
    }
    fwrite(o.text.data(), 1, o.text.size(), fo);
    fclose(fo);
    printf("catalog: %s (%u ins, %u rgn, %u waves) sha256=%s arena_peak=%u\n", outPath.c_str(), f.instrumentCount,
           f.regionCount, f.waveCount, h.c_str(), d.arena.peak);
    return 0;
}

// ---------------------------------------------------------------- work コマンド

struct MidiInput
{
    std::string path, name;
    std::vector<u8> bytes;
    InputFile info;
};

bool loadMidi(const std::string &path, MidiInput &m)
{
    if (!loadFile(path, m.bytes))
    {
        fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    m.path = path;
    size_t slash = path.find_last_of('/');
    m.name = slash == std::string::npos ? path : path.substr(slash + 1);
    m.info.name = 0; // 後で name.c_str() を入れる
    m.info.size = (u32)m.bytes.size();
    sha256Buffer(m.bytes.data(), (u32)m.bytes.size(), m.info.sha256);
    return true;
}

class FileSink : public TextSink
{
  public:
    std::string text;
    void put(const char *s, u32 n) { text.append(s, n); }
};

bool writeText(const std::string &path, const std::string &text)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
    {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return true;
}

// 内容の集計（LINEAR 走査で使う＝ファイルに書いてあるもの）
struct ContentStats : public SmfObserver
{
    std::map<std::string, u32> sysexKinds;   // 種類 -> 件数
    std::map<std::string, u32> sysexExact;   // 生のバイト列（先頭 16 バイト）-> 件数
    std::map<u32, std::vector<u32>> cc;       // CC 番号 -> {件数, 最小, 最大, ch マスク}
    std::map<std::string, std::vector<u32>> params; // "NRPN 1 8" -> {件数, 最小, 最大}
    u32 programChanges = 0, pitchBends = 0, notesOn = 0;
    bool sawNote = false, resetBeforeFirstNote = false;
    u32 firstNoteTick = 0;
    bool logEvents = false;
    std::vector<std::string> log;

    static std::string hexBytes(const u8 *p, u32 n)
    {
        std::string s;
        char b[4];
        for (u32 i = 0; i < n; i++)
        {
            snprintf(b, sizeof(b), "%s%02X", i ? " " : "", p[i]);
            s += b;
        }
        return s;
    }
    void onSysEx(u32 tick, const u8 *d, u32 len, bool escape)
    {
        std::string kind;
        u32 addr;
        const u8 *db;
        u32 dn;
        bool ok;
        if (escape)
        {
            kind = "F7 escape";
        }
        else if (len >= 4 && d[0] == 0x7e && d[2] == 0x09 && d[3] == 0x01)
        {
            kind = "GM System On";
            if (!sawNote) resetBeforeFirstNote = true;
        }
        else if (smfParseGsDt1(d, len, &addr, &db, &dn, &ok))
        {
            char k[96];
            u32 a1 = addr >> 16, a2 = (addr >> 8) & 0x7f, a3 = addr & 0x7f;
            if (addr == 0x40007f)
            {
                snprintf(k, sizeof(k), "GS DT1 40 00 7F (GS Reset)");
                if (!sawNote) resetBeforeFirstNote = true;
            }
            else if (addr == 0x00007f)
            {
                snprintf(k, sizeof(k), "GS DT1 00 00 7F (SC-88 System Mode Set) data%u", dn);
                if (!sawNote) resetBeforeFirstNote = true;
            }
            else if (a1 == 0x40 && (a2 & 0xf0) == 0x10)
                snprintf(k, sizeof(k), "GS DT1 40 1x %02X (part param) data%u", a3, dn);
            else if (a1 == 0x40 && (a2 & 0xf0) == 0x20)
                snprintf(k, sizeof(k), "GS DT1 40 2x %02X (part param 2) data%u", a3, dn);
            else if (a1 == 0x40 && (a2 & 0xf0) == 0x40)
                snprintf(k, sizeof(k), "GS DT1 40 4x %02X (part param 3) data%u", a3, dn);
            else
                snprintf(k, sizeof(k), "GS DT1 %02X %02X %02X data%u", a1, a2, a3, dn);
            kind = k;
            if (!ok) kind += " [CHECKSUM NG]";
        }
        else if (len >= 1 && d[0] == 0x7f)
        {
            kind = "Universal Realtime " + hexBytes(d, len < 4 ? len : 4);
        }
        else if (len >= 1 && d[0] == 0x7e)
        {
            kind = "Universal Non-RT " + hexBytes(d, len < 4 ? len : 4);
        }
        else if (len >= 1 && d[0] == 0x43)
        {
            kind = "Yamaha " + hexBytes(d, len < 4 ? len : 4);
        }
        else
        {
            kind = "other " + hexBytes(d, len < 4 ? len : 4);
        }
        sysexKinds[kind]++;
        sysexExact["F0 " + hexBytes(d, len)]++;
        if (logEvents)
        {
            log.push_back("t=" + std::to_string(tick) + " SysEx F0 " + hexBytes(d, len));
        }
    }
    void onControl(u32 tick, u8 ch, u8 c, u8 v)
    {
        auto &e = cc[c];
        if (e.empty()) e = {0, 127, 0, 0};
        e[0]++;
        e[1] = std::min<u32>(e[1], v);
        e[2] = std::max<u32>(e[2], v);
        e[3] |= 1u << ch;
        if (logEvents)
        {
            log.push_back("t=" + std::to_string(tick) + " ch" + std::to_string(ch + 1) + " CC" + std::to_string(c) +
                          "=" + std::to_string(v));
        }
    }
    void onProgram(u32 tick, u8 ch, u8 p)
    {
        programChanges++;
        if (logEvents)
        {
            log.push_back("t=" + std::to_string(tick) + " ch" + std::to_string(ch + 1) + " PC " + std::to_string(p));
        }
    }
    void onParam(u32, u8, bool nrpn, u8 msb, u8 lsb, u8 v)
    {
        char k[48];
        snprintf(k, sizeof(k), "%s %3u %3u", nrpn ? "NRPN" : "RPN ", msb, lsb);
        auto &e = params[k];
        if (e.empty()) e = {0, 127, 0};
        e[0]++;
        e[1] = std::min<u32>(e[1], v);
        e[2] = std::max<u32>(e[2], v);
    }
    void onNoteOn(u32 tick, u8, u8, u8, const ToneKey &)
    {
        if (!sawNote)
        {
            sawNote = true;
            firstNoteTick = tick;
        }
        notesOn++;
    }
    void onMeta(u32 tick, u8 type, const u8 *d, u32 len)
    {
        if (logEvents)
        {
            log.push_back("t=" + std::to_string(tick) + " meta " + std::to_string(type) + " len " +
                          std::to_string(len) + (len ? " " + hexBytes(d, len < 8 ? len : 8) : ""));
        }
    }
};

struct WorkSpec
{
    std::string name;
    std::string initPath;
    std::vector<std::string> files;
};

struct ScanResult
{
    std::vector<ToneUse> storage;
    UsageSet usage;
    std::string usageText, usageSha;
};

struct PerFile
{
    std::string name;
    SmfInfo info;
    UsageStats gameStats;
    u32 linearNotes;
    u32 linearPoly;
    u32 bytes;
    ContentStats content;
};

const u32 kToneCap = 1024;

// 1 作品を GAME/LINEAR で走査する。perFile は GAME と LINEAR の 1 ファイル単位の集計。
bool scanWork(const WorkSpec &ws, SmfMode mode, ScanResult &res, std::vector<PerFile> *perFile,
              ContentStats *initContent, const std::string &usageName)
{
    std::vector<MidiInput> inputs;
    std::vector<std::string> sorted = ws.files;
    std::sort(sorted.begin(), sorted.end(), [](const std::string &a, const std::string &b) {
        std::string ba = a.substr(a.find_last_of('/') + 1), bb = b.substr(b.find_last_of('/') + 1);
        return ba < bb;
    });
    for (auto &p : sorted)
    {
        inputs.emplace_back();
        if (!loadMidi(p, inputs.back())) return false;
    }
    MidiInput init;
    bool haveInit = !ws.initPath.empty();
    if (haveInit && !loadMidi(ws.initPath, init)) return false;

    res.storage.assign(kToneCap, ToneUse());
    res.usage.init(res.storage.data(), kToneCap);
    for (size_t i = 0; i < inputs.size(); i++)
    {
        SmfChannelState st[16];
        smfResetChannels(st);
        if (haveInit)
        {
            // init.mid は音色を鳴らさない前提だが、鳴らしても数えない（使い捨ての集合へ）
            std::vector<ToneUse> tmp(64);
            UsageSet scratch;
            scratch.init(tmp.data(), 64);
            SmfScanOptions o = {SMF_MODE_LINEAR, 32};
            ContentStats dummy;
            SmfStatus s = smfScan(init.bytes.data(), (u32)init.bytes.size(), st, scratch, o,
                                  initContent && i == 0 ? initContent : &dummy, 0);
            if (s != SMF_OK || scratch.stats.notes != 0)
            {
                fprintf(stderr, "init.mid: %s notes=%u\n", smfStatusName(s), scratch.stats.notes);
                if (s != SMF_OK) return false;
            }
        }
        SmfScanOptions opt = {mode, (u32)(i < 32 ? i : 32)};
        SmfInfo info;
        // 1 本ずつの集計（作品全体とは別の集合）
        std::vector<ToneUse> one(kToneCap);
        UsageSet oneSet;
        oneSet.init(one.data(), kToneCap);
        SmfChannelState st1[16];
        memcpy(st1, st, sizeof(st));
        SmfStatus s1 = smfScan(inputs[i].bytes.data(), (u32)inputs[i].bytes.size(), st1, oneSet, opt, 0, &info);
        SmfStatus s = smfScan(inputs[i].bytes.data(), (u32)inputs[i].bytes.size(), st, res.usage, opt, 0, 0);
        if (s != SMF_OK || s1 != SMF_OK)
        {
            fprintf(stderr, "%s: %s at 0x%x\n", inputs[i].name.c_str(), smfStatusName(s), info.errorOffset);
            return false;
        }
        if (perFile)
        {
            PerFile pf;
            pf.name = inputs[i].name;
            pf.info = info;
            pf.gameStats = oneSet.stats;
            // LINEAR の内容集計
            std::vector<ToneUse> lin(kToneCap);
            UsageSet linSet;
            linSet.init(lin.data(), kToneCap);
            SmfChannelState st2[16];
            smfResetChannels(st2);
            SmfScanOptions o2 = {SMF_MODE_LINEAR, 0};
            smfScan(inputs[i].bytes.data(), (u32)inputs[i].bytes.size(), st2, linSet, o2, &pf.content, 0);
            pf.linearNotes = linSet.stats.notes;
            pf.linearPoly = linSet.stats.maxPolyphony;
            pf.bytes = (u32)inputs[i].bytes.size();
            perFile->push_back(pf);
        }
    }
    // 最大同時発音は 1 本ずつの最大（全体の集合ではファイルをまたいで数えない）
    if (perFile)
    {
        u32 mx = 0;
        for (auto &pf : *perFile) mx = std::max(mx, pf.gameStats.maxPolyphony);
        res.usage.stats.maxPolyphony = mx;
    }
    std::vector<InputFile> infos;
    for (auto &in : inputs)
    {
        in.info.name = in.name.c_str();
        infos.push_back(in.info);
    }
    if (haveInit) init.info.name = init.name.c_str();
    FileSink sink;
    ManifestWriter w;
    w.begin(&sink);
    writeUsageList(w, usageName.c_str(), mode, haveInit ? &init.info : 0, infos.data(), (u32)infos.size(),
                   res.usage);
    char hex[65];
    w.finish(hex);
    res.usageText = sink.text;
    res.usageSha = hex;
    return !w.overflowed() && !res.usage.overflow;
}

std::string noteList(const ToneUse &t)
{
    std::string s;
    for (u32 n = 0; n < 128; n++)
    {
        if (t.hasNote(n))
        {
            if (!s.empty()) s += ",";
            s += std::to_string(n);
        }
    }
    return s;
}

std::string insLabel(const DlsFile &f, s32 i)
{
    if (i < 0) return "(none)";
    const DlsInstrument &ins = f.instruments[i];
    char b[96];
    snprintf(b, sizeof(b), "%s%u/%u/%u %s", ins.isDrum() ? "kit " : "", ins.bankMsb(), ins.bankLsb(), ins.prog(),
             trimName(ins.name).c_str());
    return b;
}

struct RuleResult
{
    std::string rule;
    u32 ins, rgn, waves, uncovered, unmapped;
    u32 pcm16, pcm8, pcm16Trim, pcm8Trim;
    std::vector<BlobInfo> blobs;
    std::string manifestSha;
};

bool g_writeBlobs = false; // --write-blobs: ブロブを outDir に書く（検証用。private/ の外に出さない）

bool doSubsets(LoadedDls &d, const std::string &work, const ScanResult &sr, const std::string &outDir,
               std::vector<RuleResult> &results)
{
    u8 dlsSha[32];
    sha256Buffer(d.bytes.data(), (u32)d.bytes.size(), dlsSha);
    for (u32 r = 0; r < MAP_RULE_COUNT; r++)
    {
        std::vector<u8> mem(1u << 20);
        Arena a;
        a.init(mem.data(), (u32)mem.size());
        SubsetPlan p;
        if (!subsetPlan(d.f, sr.usage, (MapRule)r, a, p))
        {
            fprintf(stderr, "subsetPlan failed\n");
            return false;
        }
        RuleResult rr;
        rr.rule = mapRuleName((MapRule)r);
        rr.ins = p.instruments;
        rr.rgn = p.regions;
        rr.waves = p.waves;
        rr.uncovered = p.uncoveredNotes;
        rr.unmapped = p.unmappedTones;
        subsetPcmTotals(d.f, p, &rr.pcm16, &rr.pcm8, &rr.pcm16Trim, &rr.pcm8Trim);
        const u32 variants[5] = {0, BLOB_PCM8, BLOB_TRIM_LOOP, BLOB_PCM8 | BLOB_TRIM_LOOP, BLOB_BIG_ENDIAN};
        MemorySource src(d.bytes.data(), (u32)d.bytes.size());
        for (u32 v : variants)
        {
            u32 sz = subsetBlobSize(d.f, sr.usage, p, v);
            std::vector<u8> blob(sz);
            u32 written = 0;
            if (!subsetWriteBlob(src, d.f, sr.usage, p, v, dlsSha, blob.data(), sz, &written) || written != sz)
            {
                fprintf(stderr, "subsetWriteBlob failed flags=%u\n", v);
                return false;
            }
            BlobInfo bi;
            bi.flags = v;
            bi.size = sz;
            sha256Buffer(blob.data(), sz, bi.sha256);
            rr.blobs.push_back(bi);
            if (g_writeBlobs && work.find("_linear") == std::string::npos)
            {
                std::string bp = outDir + "/blob_" + work + "_" + rr.rule + "_f" + std::to_string(v) + ".tdsb";
                if (!writeText(bp, std::string((const char *)blob.data(), sz))) return false;
            }
        }
        FileSink sink;
        ManifestWriter w;
        w.begin(&sink);
        writeSubsetManifest(w, d.f, dlsSha, sr.usageSha.c_str(), sr.usage, p, rr.blobs.data(),
                            (u32)rr.blobs.size());
        char hex[65];
        w.finish(hex);
        rr.manifestSha = hex;
        if (!writeText(outDir + "/subset_" + work + "_" + rr.rule + ".txt", sink.text)) return false;
        results.push_back(rr);
    }
    return true;
}

bool writeInventory(const LoadedDls &d, const WorkSpec &ws, const ScanResult &game, const ScanResult &linear,
                    const std::vector<PerFile> &perFile, const ContentStats &initContent, const std::string &outPath)
{
    Out o;
    const DlsFile &f = d.f;
    o.line("# %s 使用音色の調査表（tdls work、GAME = ゲームの CC2/CC4 ループどおり 2 周、LINEAR = ファイル全体）", ws.name.c_str());
    o.line("usage.game.sha256 %s", game.usageSha.c_str());
    o.line("usage.linear.sha256 %s", linear.usageSha.c_str());
    o.line("%s", "");
    o.line("## ファイル");
    o.line("%-14s %7s fmt trk  div  last_tick notes_game notes_linear poly_game poly_linear CC2 CC4 reset_before_1st_note first_note_tick",
           "name", "bytes");
    for (auto &pf : perFile)
    {
        auto ccCount = [&](u32 n) -> u32 {
            auto it = pf.content.cc.find(n);
            return it == pf.content.cc.end() ? 0u : it->second[0];
        };
        o.line("%-14s %7u %3u %3u %4u %10u %10u %12u %9u %11u %3u %3u %s %u", pf.name.c_str(), pf.bytes,
               pf.info.format, pf.info.tracks, pf.info.division, pf.info.lastTick, pf.gameStats.notes,
               pf.linearNotes, pf.gameStats.maxPolyphony, pf.linearPoly, ccCount(2), ccCount(4),
               pf.content.resetBeforeFirstNote ? "yes" : "NO", pf.content.firstNoteTick);
    }
    o.line("（last_tick と notes_game は GAME 走査＝ループ 2 周目まで。CC2/CC4 はファイル中の件数）");
    const UsageStats &s = game.usage.stats;
    o.line("%s", "");
    o.line("## 集計（GAME）");
    o.line("notes=%u no_program=%u pending_bank=%u gs_reset=%u gm_on=%u sys_mode=%u rhythm_set=%u tone_number=%u "
           "rx_channel=%u rx_nonidentity=%u gs_checksum_err=%u loop_rs_mismatch=%u f7=%u max_poly(1曲)=%u",
           s.notes, s.notesNoProgram, s.notesPendingBank, s.gsReset, s.gmOn, s.sysModeSet, s.rhythmPartSet,
           s.toneNumberSet, s.rxChannelSet, s.rxChannelNonIdentity, s.gsChecksumErrors,
           s.loopRunningStatusMismatch, s.f7Events, s.maxPolyphony);

    // 音色表
    u32 mel = 0, drum = 0;
    for (u32 t = 0; t < game.usage.count; t++) (game.usage.tones[t].key.drum ? drum : mel)++;
    o.line("%s", "");
    o.line("## 音色（GAME）: 旋律 %u 種、リズム %u キット", mel, drum);
    o.line("d msb lsb prog notes   key     vel     ch(1始まり)      files  A規則→楽器 / C規則→楽器");
    for (u32 t = 0; t < game.usage.count; t++)
    {
        const ToneUse &tu = game.usage.tones[t];
        MapHow ha, hc;
        s32 ia = mapTone(f, tu.key, MAP_A_CAPITAL, &ha);
        s32 ic = mapTone(f, tu.key, MAP_C_STRICT_LSB, &hc);
        std::string chs;
        for (u32 c = 0; c < 16; c++)
            if (tu.channelMask & (1u << c)) chs += (chs.empty() ? "" : ",") + std::to_string(c + 1);
        u32 nf = 0;
        for (u32 b = 0; b < 32; b++) nf += (tu.fileMask >> b) & 1u;
        o.line("%u %3u %3u %4u %6u %3u-%-3u %3u-%-3u %-16s %2u  %s(%s) / %s(%s)", tu.key.drum, tu.key.msb,
               tu.key.lsb, tu.key.program, tu.noteCount, tu.noteLo, tu.noteHi, tu.velLo, tu.velHi, chs.c_str(), nf,
               insLabel(f, ia).c_str(), mapHowName(ha), insLabel(f, ic).c_str(), mapHowName(hc));
    }
    o.line("%s", "");
    o.line("## リズムのノート（キットごと）");
    for (u32 t = 0; t < game.usage.count; t++)
    {
        const ToneUse &tu = game.usage.tones[t];
        if (!tu.key.drum) continue;
        o.line("kit prog=%u (msb=%u lsb=%u) notes=%u : %s", tu.key.program, tu.key.msb, tu.key.lsb, tu.noteCount,
               noteList(tu).c_str());
    }
    // GAME と LINEAR の差
    o.line("%s", "");
    o.line("## LINEAR にだけある音色（CC4 の後ろ = ゲームでは鳴らない）");
    u32 onlyLin = 0;
    for (u32 t = 0; t < linear.usage.count; t++)
    {
        const ToneUse &tu = linear.usage.tones[t];
        const ToneUse *g = game.usage.find(tu.key);
        bool extraNotes = false;
        if (g)
            for (u32 q = 0; q < 4; q++)
                if (tu.noteBits[q] & ~g->noteBits[q]) extraNotes = true;
        if (!g || extraNotes)
        {
            o.line("d=%u msb=%u lsb=%u prog=%u %s", tu.key.drum, tu.key.msb, tu.key.lsb, tu.key.program,
                   g ? "（音色はあるがノートが増える）" : "（音色ごと LINEAR だけ）");
            onlyLin++;
        }
    }
    if (!onlyLin) o.line("なし");
    o.line("## GAME にだけある音色（ループ 2 周目で状態を引き継いで初めて鳴る組）");
    u32 onlyGame = 0;
    for (u32 t = 0; t < game.usage.count; t++)
    {
        const ToneUse &tu = game.usage.tones[t];
        if (!linear.usage.find(tu.key))
        {
            o.line("d=%u msb=%u lsb=%u prog=%u", tu.key.drum, tu.key.msb, tu.key.lsb, tu.key.program);
            onlyGame++;
        }
    }
    if (!onlyGame) o.line("なし");

    // 内容（LINEAR）の集計を作品で合算
    std::map<std::string, u32> kinds;
    std::map<std::string, u32> exact;
    std::map<u32, std::vector<u32>> cc;
    std::map<std::string, std::vector<u32>> params;
    for (auto &pf : perFile)
    {
        for (auto &kv : pf.content.sysexKinds) kinds[kv.first] += kv.second;
        for (auto &kv : pf.content.sysexExact) exact[kv.first] += kv.second;
        for (auto &kv : pf.content.cc)
        {
            auto &e = cc[kv.first];
            if (e.empty()) e = {0, 127, 0, 0};
            e[0] += kv.second[0];
            e[1] = std::min(e[1], kv.second[1]);
            e[2] = std::max(e[2], kv.second[2]);
            e[3] |= kv.second[3];
        }
        for (auto &kv : pf.content.params)
        {
            auto &e = params[kv.first];
            if (e.empty()) e = {0, 127, 0};
            e[0] += kv.second[0];
            e[1] = std::min(e[1], kv.second[1]);
            e[2] = std::max(e[2], kv.second[2]);
        }
    }
    o.line("%s", "");
    o.line("## SysEx の種類（LINEAR = ファイルの中身、全曲合計）");
    for (auto &kv : kinds) o.line("%6u  %s", kv.second, kv.first.c_str());
    o.line("## SysEx の生バイト列（種類 %zu）", exact.size());
    for (auto &kv : exact) o.line("%6u  %s", kv.second, kv.first.c_str());
    o.line("%s", "");
    o.line("## CC（LINEAR）: 番号 件数 値の範囲 ch");
    for (auto &kv : cc)
    {
        std::string chs;
        for (u32 c = 0; c < 16; c++)
            if (kv.second[3] & (1u << c)) chs += (chs.empty() ? "" : ",") + std::to_string(c + 1);
        o.line("CC%-3u %7u %3u-%-3u ch %s", kv.first, kv.second[0], kv.second[1], kv.second[2], chs.c_str());
    }
    o.line("%s", "");
    o.line("## RPN/NRPN のデータエントリ（CC6）: 種別 MSB LSB 件数 値の範囲");
    for (auto &kv : params) o.line("%s %6u %3u-%u", kv.first.c_str(), kv.second[0], kv.second[1], kv.second[2]);
    if (!ws.initPath.empty())
    {
        o.line("%s", "");
        o.line("## init.mid の中身（全事象）");
        for (auto &l : initContent.log) o.line("%s", l.c_str());
    }
    return writeText(outPath, o.text);
}

int cmdWork(int argc, char **argv)
{
    std::string dlsPath, outDir;
    std::vector<WorkSpec> works;
    for (int i = 2; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--dls" && i + 1 < argc) dlsPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--write-blobs") g_writeBlobs = true;
        else if (a == "--work" && i + 1 < argc)
        {
            works.emplace_back();
            works.back().name = argv[++i];
        }
        else if (a == "--init" && i + 1 < argc && !works.empty()) works.back().initPath = argv[++i];
        else if (!works.empty()) works.back().files.push_back(a);
        else
        {
            fprintf(stderr, "bad arg %s\n", a.c_str());
            return 64;
        }
    }
    if (dlsPath.empty() || outDir.empty() || works.empty())
    {
        fprintf(stderr, "need --dls --out --work\n");
        return 64;
    }
    LoadedDls d;
    if (!loadDls(dlsPath, d)) return 1;
    printf("dls.sha256 %s size %u\n", d.sha.c_str(), d.f.fileSize);
    printf("%-6s %-13s %4s %4s %4s %5s %5s %9s %9s %9s %9s %9s %s\n", "work", "rule", "ins", "rgn", "wave", "uncov",
           "unmap", "pcm16", "pcm8", "pcm16trim", "pcm8trim", "blob16", "manifest_sha256");
    // 全作品合算（union）用
    std::vector<ToneUse> uniStore(kToneCap);
    UsageSet uni;
    uni.init(uniStore.data(), kToneCap);
    for (auto &ws : works)
    {
        ScanResult game, linear;
        std::vector<PerFile> perFile;
        ContentStats initContent;
        initContent.logEvents = true;
        if (!scanWork(ws, SMF_MODE_GAME, game, &perFile, &initContent, ws.name)) return 1;
        if (!scanWork(ws, SMF_MODE_LINEAR, linear, 0, 0, ws.name)) return 1;
        if (!writeText(outDir + "/usage_" + ws.name + ".txt", game.usageText)) return 1;
        if (!writeText(outDir + "/usage_" + ws.name + "_linear.txt", linear.usageText)) return 1;
        if (!writeInventory(d, ws, game, linear, perFile, initContent, outDir + "/inventory_" + ws.name + ".txt"))
            return 1;
        printf("usage %s game=%s linear=%s tones=%u\n", ws.name.c_str(), game.usageSha.c_str(),
               linear.usageSha.c_str(), game.usage.count);
        std::vector<RuleResult> rr;
        if (!doSubsets(d, ws.name, game, outDir, rr)) return 1;
        for (auto &r : rr)
        {
            printf("%-6s %-13s %4u %4u %4u %5u %5u %9u %9u %9u %9u %9u %s\n", ws.name.c_str(), r.rule.c_str(), r.ins,
                   r.rgn, r.waves, r.uncovered, r.unmapped, r.pcm16, r.pcm8, r.pcm16Trim, r.pcm8Trim, r.blobs[0].size,
                   r.manifestSha.c_str());
            for (auto &b : r.blobs)
            {
                char h[65];
                sha256Hex(b.sha256, h);
                printf("    blob flags=%u size=%u sha256=%s\n", b.flags, b.size, h);
            }
        }
        // 比較用: CC4 の後ろまで数えた（LINEAR）場合の大きさ
        std::vector<RuleResult> rl;
        if (!doSubsets(d, ws.name + "_linear", linear, outDir, rl)) return 1;
        for (auto &r : rl)
        {
            printf("%-6s %-13s %4u %4u %4u %5u %5u %9u %9u %9u %9u %9u %s (LINEAR)\n", ws.name.c_str(), r.rule.c_str(),
                   r.ins, r.rgn, r.waves, r.uncovered, r.unmapped, r.pcm16, r.pcm8, r.pcm16Trim, r.pcm8Trim,
                   r.blobs[0].size, r.manifestSha.c_str());
        }
        // union へ合算（ノート・ベロシティ・回数）
        for (u32 t = 0; t < game.usage.count; t++)
        {
            const ToneUse &src = game.usage.tones[t];
            ToneUse *dst = uni.findOrAdd(src.key);
            if (!dst) return 1;
            for (u32 n = 0; n < 128; n++)
            {
                if (!src.hasNote(n)) continue;
                if (!dst->hasNote(n))
                {
                    dst->noteBits[n >> 5] |= 1u << (n & 31u);
                    dst->velMin[n] = src.velMin[n];
                    dst->velMax[n] = src.velMax[n];
                }
                else
                {
                    dst->velMin[n] = std::min(dst->velMin[n], src.velMin[n]);
                    dst->velMax[n] = std::max(dst->velMax[n], src.velMax[n]);
                }
            }
            dst->noteLo = dst->noteCount ? std::min(dst->noteLo, src.noteLo) : src.noteLo;
            dst->noteHi = dst->noteCount ? std::max(dst->noteHi, src.noteHi) : src.noteHi;
            dst->velLo = dst->noteCount ? std::min(dst->velLo, src.velLo) : src.velLo;
            dst->velHi = dst->noteCount ? std::max(dst->velHi, src.velHi) : src.velHi;
            dst->noteCount += src.noteCount;
            dst->channelMask = (u16)(dst->channelMask | src.channelMask);
        }
    }
    if (works.size() > 1)
    {
        ScanResult u;
        u.usage = uni;
        FileSink sink;
        ManifestWriter w;
        w.begin(&sink);
        writeUsageList(w, "union", SMF_MODE_GAME, 0, 0, 0, uni);
        char hex[65];
        w.finish(hex);
        u.usageText = sink.text;
        u.usageSha = hex;
        if (!writeText(outDir + "/usage_union.txt", u.usageText)) return 1;
        std::vector<RuleResult> rr;
        if (!doSubsets(d, "union", u, outDir, rr)) return 1;
        for (auto &r : rr)
        {
            printf("%-6s %-13s %4u %4u %4u %5u %5u %9u %9u %9u %9u %9u %s\n", "union", r.rule.c_str(), r.ins, r.rgn,
                   r.waves, r.uncovered, r.unmapped, r.pcm16, r.pcm8, r.pcm16Trim, r.pcm8Trim, r.blobs[0].size,
                   r.manifestSha.c_str());
            for (auto &b : r.blobs)
            {
                char h[65];
                sha256Hex(b.sha256, h);
                printf("    blob flags=%u size=%u sha256=%s\n", b.flags, b.size, h);
            }
        }
    }
    return 0;
}

void usage()
{
    fprintf(stderr, "usage:\n  dls_subset_tool catalog <gm.dls> <out.txt>\n"
                    "  dls_subset_tool work --dls <gm.dls> --out <dir> --work <name> [--init <init.mid>] <mid...>"
                    " [--work <name> ...]\n");
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
    if (cmd == "catalog" && argc == 4)
    {
        return cmdCatalog(argv[2], argv[3]);
    }
    if (cmd == "work")
    {
        return cmdWork(argc, argv);
    }
    usage();
    return 64;
}
