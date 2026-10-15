#include "tdls/dls.hpp"

namespace tdls {

namespace {

const u32 ID_RIFF = 0x46464952u; // 'RIFF'
const u32 ID_LIST = 0x5453494cu; // 'LIST'

inline u32 ID(const char *s) { return fourcc(s[0], s[1], s[2], s[3]); }

struct Chunk
{
    u32 id;
    u32 size;
    u32 header;   // チャンク見出しの位置
    u32 data;     // 中身の位置（LIST/RIFF は型の位置）
    u32 listType; // LIST/RIFF の型。それ以外は 0
    u32 body;     // LIST/RIFF の子の先頭（型の直後）。それ以外は data
    u32 end;      // 中身の終わり（パディング前）
};

struct Ctx
{
    ByteSource *src;
    DlsFile *f;
    bool fill;
    u32 nIns, nRgn, nWave, nConn, nCue;
    DlsStatus st;
};

class ChunkIter
{
  public:
    ChunkIter(Ctx &c, u32 begin, u32 end) : c_(c), pos_(begin), end_(end) {}

    bool next(Chunk &ch)
    {
        if (c_.st != DLS_OK || pos_ >= end_ || end_ - pos_ < 8)
        {
            return false; // 末尾の 8 バイト未満は詰め物として無視
        }
        u8 h[12];
        if (!c_.src->read(pos_, h, 8))
        {
            fail(DLS_ERR_READ, pos_);
            return false;
        }
        ch.id = rd32le(h);
        ch.size = rd32le(h + 4);
        ch.header = pos_;
        ch.data = pos_ + 8;
        if (ch.size > end_ - ch.data)
        {
            fail(DLS_ERR_TRUNCATED, pos_);
            return false;
        }
        ch.end = ch.data + ch.size;
        ch.listType = 0;
        ch.body = ch.data;
        if (ch.id == ID_LIST || ch.id == ID_RIFF)
        {
            if (ch.size < 4 || !c_.src->read(ch.data, h + 8, 4))
            {
                fail(DLS_ERR_BAD_CHUNK, pos_);
                return false;
            }
            ch.listType = rd32le(h + 8);
            ch.body = ch.data + 4;
        }
        u32 next = ch.end + (ch.size & 1u);
        pos_ = next > end_ ? end_ : next; // 最後のパディング欠けは許す
        return true;
    }

  private:
    void fail(DlsStatus s, u32 off)
    {
        c_.st = s;
        c_.f->errorOffset = off;
    }
    Ctx &c_;
    u32 pos_, end_;
};

bool readBytes(Ctx &c, u32 off, void *dst, u32 len)
{
    if (!c.src->read(off, dst, len))
    {
        c.st = DLS_ERR_READ;
        c.f->errorOffset = off;
        return false;
    }
    return true;
}

void bad(Ctx &c, u32 off)
{
    if (c.st == DLS_OK)
    {
        c.st = DLS_ERR_BAD_CHUNK;
        c.f->errorOffset = off;
    }
}

void readString(Ctx &c, const Chunk &ch, char *dst, u32 cap)
{
    u32 n = ch.size < cap - 1 ? ch.size : cap - 1;
    if (!c.fill)
    {
        return;
    }
    if (!readBytes(c, ch.data, dst, n))
    {
        return;
    }
    dst[n] = 0; // 途中に NUL があればそこで終わる（C 文字列として扱う）
}

void parseInfo(Ctx &c, const Chunk &list, char *name, u32 nameCap, char *copyright, u32 copyrightCap)
{
    ChunkIter it(c, list.body, list.end);
    Chunk ch;
    while (it.next(ch))
    {
        if (ch.id == ID("INAM") && name)
        {
            readString(c, ch, name, nameCap);
        }
        else if (ch.id == ID("ICOP") && copyright)
        {
            readString(c, ch, copyright, copyrightCap);
        }
    }
}

void noteUnknown(Ctx &c, u32 id)
{
    c.f->unknownChunks++;
    if (!c.fill)
    {
        return;
    }
    for (u32 i = 0; i < c.f->unknownTopCount; i++)
    {
        if (c.f->unknownTopIds[i] == id)
        {
            return;
        }
    }
    if (c.f->unknownTopCount < 8)
    {
        c.f->unknownTopIds[c.f->unknownTopCount++] = id;
    }
}

void parseWsmp(Ctx &c, const Chunk &ch, DlsWsmp &w)
{
    u8 b[20];
    if (ch.size < 20)
    {
        bad(c, ch.header);
        return;
    }
    if (!readBytes(c, ch.data, b, 20))
    {
        return;
    }
    u32 cb = rd32le(b);
    w.unityNote = rd16le(b + 4);
    w.fineTune = rds16le(b + 6);
    w.attenuation = rds32le(b + 8);
    w.options = rd32le(b + 12);
    w.loopCount = rd32le(b + 16);
    w.loop.type = w.loop.start = w.loop.length = 0;
    if (cb < 20 || cb > ch.size)
    {
        bad(c, ch.header);
        return;
    }
    if (w.loopCount > 0)
    {
        u8 l[16];
        if (cb + 16u > ch.size)
        {
            bad(c, ch.header);
            return;
        }
        if (!readBytes(c, ch.data + cb, l, 16))
        {
            return;
        }
        w.loop.type = rd32le(l + 4);
        w.loop.start = rd32le(l + 8);
        w.loop.length = rd32le(l + 12);
        if (w.loopCount > 1 && c.fill)
        {
            c.f->multiLoopWsmp++;
        }
    }
}

// LIST 'lart' / 'lar2' の中の art1 / art2（接続ブロックの形は同じ）
void parseArtList(Ctx &c, const Chunk &list, DlsArt &art)
{
    art.firstConn = c.nConn;
    art.connCount = 0;
    art.chunkCount = 0;
    ChunkIter it(c, list.body, list.end);
    Chunk ch;
    while (it.next(ch))
    {
        bool a1 = ch.id == ID("art1");
        bool a2 = ch.id == ID("art2");
        if (!a1 && !a2)
        {
            noteUnknown(c, ch.id);
            continue;
        }
        if (a2)
        {
            c.f->level2Chunks += c.fill ? 1u : 0u;
        }
        u8 b[8];
        if (ch.size < 8 || !readBytes(c, ch.data, b, 8))
        {
            bad(c, ch.header);
            return;
        }
        u32 cb = rd32le(b);
        u32 n = rd32le(b + 4);
        if (cb < 8 || cb > ch.size || n > (ch.size - cb) / 12u)
        {
            bad(c, ch.header);
            return;
        }
        for (u32 k = 0; k < n; k++)
        {
            if (c.fill)
            {
                u8 e[12];
                if (!readBytes(c, ch.data + cb + k * 12u, e, 12))
                {
                    return;
                }
                DlsConnection &cn = c.f->conns[c.nConn];
                cn.source = rd16le(e);
                cn.control = rd16le(e + 2);
                cn.destination = rd16le(e + 4);
                cn.transform = rd16le(e + 6);
                cn.scale = rds32le(e + 8);
            }
            c.nConn++;
        }
        art.connCount = (u16)(art.connCount + n);
        art.chunkCount++;
    }
}

void parseRegion(Ctx &c, const Chunk &list, DlsRegion *r)
{
    DlsRegion tmp;
    DlsRegion &rg = r ? *r : tmp;
    rg.keyLo = rg.keyHi = rg.velLo = rg.velHi = 0;
    rg.options = rg.keyGroup = rg.layer = rg.wlnkOptions = rg.phaseGroup = 0;
    rg.channel = rg.tableIndex = 0;
    rg.waveIndex = 0xffffffffu;
    rg.hasWsmp = rg.hasWlnk = rg.hasArt = 0;
    rg.pad_ = 0;
    rg.art.firstConn = 0;
    rg.art.connCount = rg.art.chunkCount = 0;
    bool haveRgnh = false;
    ChunkIter it(c, list.body, list.end);
    Chunk ch;
    while (it.next(ch))
    {
        if (ch.id == ID("rgnh"))
        {
            u8 b[14];
            if (ch.size < 12 || !readBytes(c, ch.data, b, ch.size >= 14 ? 14u : 12u))
            {
                bad(c, ch.header);
                return;
            }
            rg.keyLo = rd16le(b);
            rg.keyHi = rd16le(b + 2);
            rg.velLo = rd16le(b + 4);
            rg.velHi = rd16le(b + 6);
            rg.options = rd16le(b + 8);
            rg.keyGroup = rd16le(b + 10);
            rg.layer = ch.size >= 14 ? rd16le(b + 12) : 0;
            haveRgnh = true;
        }
        else if (ch.id == ID("wsmp"))
        {
            parseWsmp(c, ch, rg.wsmp);
            rg.hasWsmp = 1;
        }
        else if (ch.id == ID("wlnk"))
        {
            u8 b[12];
            if (ch.size < 12 || !readBytes(c, ch.data, b, 12))
            {
                bad(c, ch.header);
                return;
            }
            rg.wlnkOptions = rd16le(b);
            rg.phaseGroup = rd16le(b + 2);
            rg.channel = rd32le(b + 4);
            rg.tableIndex = rd32le(b + 8);
            rg.hasWlnk = 1;
        }
        else if (ch.id == ID_LIST && (ch.listType == ID("lart") || ch.listType == ID("lar2")))
        {
            if (ch.listType == ID("lar2"))
            {
                c.f->level2Chunks += c.fill ? 1u : 0u;
            }
            parseArtList(c, ch, rg.art);
            rg.hasArt = 1;
        }
        else
        {
            noteUnknown(c, ch.id == ID_LIST ? ch.listType : ch.id);
        }
    }
    if (!haveRgnh)
    {
        bad(c, list.header);
    }
    if (c.fill)
    {
        if (!rg.hasWsmp)
        {
            c.f->regionsWithoutWsmp++;
        }
        if (!rg.hasWlnk)
        {
            c.f->regionsWithoutWlnk++;
        }
    }
}

void parseInstrument(Ctx &c, const Chunk &list)
{
    DlsInstrument tmp;
    DlsInstrument &ins = c.fill ? c.f->instruments[c.nIns] : tmp;
    ins.bank = ins.program = ins.declaredRegions = 0;
    ins.firstRegion = c.nRgn;
    ins.regionCount = 0;
    ins.listOffset = list.header;
    ins.hasArt = 0;
    ins.pad_[0] = ins.pad_[1] = ins.pad_[2] = 0;
    ins.art.firstConn = 0;
    ins.art.connCount = ins.art.chunkCount = 0;
    ins.name[0] = 0;
    ChunkIter it(c, list.body, list.end);
    Chunk ch;
    while (it.next(ch))
    {
        if (ch.id == ID("insh"))
        {
            u8 b[12];
            if (ch.size < 12 || !readBytes(c, ch.data, b, 12))
            {
                bad(c, ch.header);
                return;
            }
            ins.declaredRegions = rd32le(b);
            ins.bank = rd32le(b + 4);
            ins.program = rd32le(b + 8);
        }
        else if (ch.id == ID_LIST && ch.listType == ID("lrgn"))
        {
            ChunkIter rit(c, ch.body, ch.end);
            Chunk rc;
            while (rit.next(rc))
            {
                if (rc.id == ID_LIST && (rc.listType == ID("rgn ") || rc.listType == ID("rgn2")))
                {
                    if (rc.listType == ID("rgn2"))
                    {
                        c.f->level2Chunks += c.fill ? 1u : 0u;
                    }
                    parseRegion(c, rc, c.fill ? &c.f->regions[c.nRgn] : 0);
                    c.nRgn++;
                    ins.regionCount++;
                }
                else
                {
                    noteUnknown(c, rc.id == ID_LIST ? rc.listType : rc.id);
                }
            }
        }
        else if (ch.id == ID_LIST && (ch.listType == ID("lart") || ch.listType == ID("lar2")))
        {
            if (ch.listType == ID("lar2"))
            {
                c.f->level2Chunks += c.fill ? 1u : 0u;
            }
            parseArtList(c, ch, ins.art);
            ins.hasArt = 1;
        }
        else if (ch.id == ID_LIST && ch.listType == ID("INFO"))
        {
            parseInfo(c, ch, ins.name, sizeof(ins.name), 0, 0);
        }
        else if (ch.id == ID("dlid"))
        {
            c.f->level2Chunks += c.fill ? 1u : 0u;
        }
        else
        {
            noteUnknown(c, ch.id == ID_LIST ? ch.listType : ch.id);
        }
    }
    c.nIns++;
}

void parseWave(Ctx &c, const Chunk &list)
{
    DlsWave tmp;
    DlsWave &w = c.fill ? c.f->waves[c.nWave] : tmp;
    w.listOffset = list.header;
    w.dataOffset = w.dataBytes = 0;
    w.formatTag = w.channels = w.blockAlign = w.bitsPerSample = 0;
    w.sampleRate = 0;
    w.hasWsmp = 0;
    w.pad_[0] = w.pad_[1] = w.pad_[2] = 0;
    w.name[0] = 0;
    bool haveFmt = false, haveData = false;
    ChunkIter it(c, list.body, list.end);
    Chunk ch;
    while (it.next(ch))
    {
        if (ch.id == ID("fmt "))
        {
            u8 b[16];
            if (ch.size < 16 || !readBytes(c, ch.data, b, 16))
            {
                bad(c, ch.header);
                return;
            }
            w.formatTag = rd16le(b);
            w.channels = rd16le(b + 2);
            w.sampleRate = rd32le(b + 4);
            w.blockAlign = rd16le(b + 12);
            w.bitsPerSample = rd16le(b + 14);
            haveFmt = true;
        }
        else if (ch.id == ID("data"))
        {
            w.dataOffset = ch.data;
            w.dataBytes = ch.size;
            haveData = true;
        }
        else if (ch.id == ID("wsmp"))
        {
            parseWsmp(c, ch, w.wsmp);
            w.hasWsmp = 1;
        }
        else if (ch.id == ID_LIST && ch.listType == ID("INFO"))
        {
            parseInfo(c, ch, w.name, sizeof(w.name), 0, 0);
        }
        else if (ch.id == ID("dlid"))
        {
            c.f->level2Chunks += c.fill ? 1u : 0u;
        }
        else
        {
            noteUnknown(c, ch.id == ID_LIST ? ch.listType : ch.id);
        }
    }
    if (!haveFmt || !haveData)
    {
        bad(c, list.header);
    }
    c.nWave++;
}

void parseTop(Ctx &c, u32 begin, u32 end)
{
    DlsFile &f = *c.f;
    ChunkIter it(c, begin, end);
    Chunk ch;
    while (it.next(ch))
    {
        if (ch.id == ID("colh"))
        {
            u8 b[4];
            if (ch.size < 4 || !readBytes(c, ch.data, b, 4))
            {
                bad(c, ch.header);
                return;
            }
            f.colhCount = rd32le(b);
        }
        else if (ch.id == ID("vers"))
        {
            u8 b[8];
            if (ch.size < 8 || !readBytes(c, ch.data, b, 8))
            {
                bad(c, ch.header);
                return;
            }
            f.hasVers = 1;
            f.versMs = rd32le(b);
            f.versLs = rd32le(b + 4);
        }
        else if (ch.id == ID("ptbl"))
        {
            u8 b[8];
            if (ch.size < 8 || !readBytes(c, ch.data, b, 8))
            {
                bad(c, ch.header);
                return;
            }
            u32 cb = rd32le(b);
            u32 n = rd32le(b + 4);
            if (cb < 8 || cb > ch.size || n > (ch.size - cb) / 4u)
            {
                bad(c, ch.header);
                return;
            }
            c.nCue = n;
            if (c.fill)
            {
                for (u32 i = 0; i < n; i++)
                {
                    u8 o[4];
                    if (!readBytes(c, ch.data + cb + i * 4u, o, 4))
                    {
                        return;
                    }
                    f.cueOffsets[i] = rd32le(o);
                }
            }
        }
        else if (ch.id == ID_LIST && ch.listType == ID("lins"))
        {
            ChunkIter iit(c, ch.body, ch.end);
            Chunk ic;
            while (iit.next(ic))
            {
                if (ic.id == ID_LIST && ic.listType == ID("ins "))
                {
                    parseInstrument(c, ic);
                }
                else
                {
                    noteUnknown(c, ic.id == ID_LIST ? ic.listType : ic.id);
                }
            }
        }
        else if (ch.id == ID_LIST && ch.listType == ID("wvpl"))
        {
            f.wvplListOffset = ch.header;
            ChunkIter wit(c, ch.body, ch.end);
            Chunk wc;
            while (wit.next(wc))
            {
                if (wc.id == ID_LIST && wc.listType == ID("wave"))
                {
                    parseWave(c, wc);
                }
                else
                {
                    noteUnknown(c, wc.id == ID_LIST ? wc.listType : wc.id);
                }
            }
        }
        else if (ch.id == ID_LIST && ch.listType == ID("INFO"))
        {
            parseInfo(c, ch, f.infoName, sizeof(f.infoName), f.infoCopyright, sizeof(f.infoCopyright));
        }
        else if (ch.id == ID("dlid") || ch.id == ID("cdl "))
        {
            f.level2Chunks += c.fill ? 1u : 0u;
        }
        else
        {
            noteUnknown(c, ch.id == ID_LIST ? ch.listType : ch.id);
        }
    }
}

s32 findWaveAt(const DlsFile &f, u32 off)
{
    u32 lo = 0, hi = f.waveCount;
    while (lo < hi)
    {
        u32 mid = lo + (hi - lo) / 2;
        if (f.waves[mid].listOffset < off)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return (lo < f.waveCount && f.waves[lo].listOffset == off) ? (s32)lo : -1;
}

} // namespace

const char *dlsStatusName(DlsStatus s)
{
    switch (s)
    {
    case DLS_OK: return "OK";
    case DLS_ERR_READ: return "READ";
    case DLS_ERR_NOT_DLS: return "NOT_DLS";
    case DLS_ERR_TRUNCATED: return "TRUNCATED";
    case DLS_ERR_BAD_CHUNK: return "BAD_CHUNK";
    case DLS_ERR_NO_MEMORY: return "NO_MEMORY";
    case DLS_ERR_PTBL: return "PTBL";
    }
    return "?";
}

DlsStatus dlsParse(ByteSource &src, Arena &arena, DlsFile &out)
{
    memset(&out, 0, sizeof(out));
    out.fileSize = src.size();
    u8 h[12];
    if (out.fileSize < 12 || !src.read(0, h, 12))
    {
        return DLS_ERR_NOT_DLS;
    }
    if (rd32le(h) != ID_RIFF || rd32le(h + 8) != ID("DLS "))
    {
        return DLS_ERR_NOT_DLS;
    }
    out.riffSize = rd32le(h + 4);
    if (out.riffSize < 4 || out.riffSize > out.fileSize - 8)
    {
        return DLS_ERR_TRUNCATED;
    }
    u32 end = 8 + out.riffSize;

    // 1 回目: 数を数える
    Ctx c;
    c.src = &src;
    c.f = &out;
    c.fill = false;
    c.nIns = c.nRgn = c.nWave = c.nConn = c.nCue = 0;
    c.st = DLS_OK;
    parseTop(c, 12, end);
    if (c.st != DLS_OK)
    {
        return c.st;
    }
    u32 nIns = c.nIns, nRgn = c.nRgn, nWave = c.nWave, nConn = c.nConn, nCue = c.nCue;

    // 1 回目で触った集計を戻す（2 回目で数え直す）
    out.unknownChunks = 0;
    out.level2Chunks = 0;

    out.instruments = (DlsInstrument *)arena.alloc(nIns * (u32)sizeof(DlsInstrument), 4);
    out.regions = (DlsRegion *)arena.alloc(nRgn * (u32)sizeof(DlsRegion), 4);
    out.waves = (DlsWave *)arena.alloc(nWave * (u32)sizeof(DlsWave), 4);
    out.conns = (DlsConnection *)arena.alloc(nConn * (u32)sizeof(DlsConnection), 4);
    out.cueOffsets = (u32 *)arena.alloc(nCue * 4u, 4);
    out.cueToWave = (u32 *)arena.alloc(nCue * 4u, 4);
    if ((nIns && !out.instruments) || (nRgn && !out.regions) || (nWave && !out.waves) ||
        (nConn && !out.conns) || (nCue && (!out.cueOffsets || !out.cueToWave)))
    {
        return DLS_ERR_NO_MEMORY;
    }

    // 2 回目: 中身を詰める
    c.fill = true;
    c.nIns = c.nRgn = c.nWave = c.nConn = c.nCue = 0;
    parseTop(c, 12, end);
    if (c.st != DLS_OK)
    {
        return c.st;
    }
    out.instrumentCount = c.nIns;
    out.regionCount = c.nRgn;
    out.waveCount = c.nWave;
    out.connCount = c.nConn;
    out.ptblCueCount = c.nCue;

    // ptbl の番地 -> wave。基準の候補を順に試し、全件が LIST 'wave' に当たるものを採る。
    // 候補: (0) 'wvpl' 型の直後, (1) LIST の中身の先頭（型の位置）, (2) LIST 見出しの位置
    const u32 bases[3] = {out.wvplListOffset + 12u, out.wvplListOffset + 8u, out.wvplListOffset};
    bool ok = false;
    for (u32 b = 0; b < 3 && !ok; b++)
    {
        ok = true;
        for (u32 i = 0; i < out.ptblCueCount; i++)
        {
            s32 w = findWaveAt(out, bases[b] + out.cueOffsets[i]);
            if (w < 0)
            {
                ok = false;
                break;
            }
            out.cueToWave[i] = (u32)w;
        }
        if (ok)
        {
            out.cueBase = bases[b];
        }
    }
    if (!ok)
    {
        return DLS_ERR_PTBL;
    }
    for (u32 r = 0; r < out.regionCount; r++)
    {
        DlsRegion &rg = out.regions[r];
        rg.waveIndex = rg.tableIndex < out.ptblCueCount ? out.cueToWave[rg.tableIndex] : 0xffffffffu;
    }
    return DLS_OK;
}

s32 dlsFindInstrument(const DlsFile &f, bool drum, u8 msb, u8 lsb, u8 program)
{
    for (u32 i = 0; i < f.instrumentCount; i++)
    {
        const DlsInstrument &ins = f.instruments[i];
        if (ins.isDrum() == drum && ins.bankMsb() == msb && ins.bankLsb() == lsb && ins.prog() == program)
        {
            return (s32)i;
        }
    }
    return -1;
}

const DlsWsmp *dlsEffectiveWsmp(const DlsFile &f, const DlsRegion &r)
{
    if (r.hasWsmp)
    {
        return &r.wsmp;
    }
    if (r.waveIndex < f.waveCount && f.waves[r.waveIndex].hasWsmp)
    {
        return &f.waves[r.waveIndex].wsmp;
    }
    return 0;
}

} // namespace tdls
