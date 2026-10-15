// 読み口の抽象。PC は丸ごとメモリ、機種側は sceIo 等で実装する。
//
// PSP Go の内蔵ストレージ（ef0）は seek と極小 read の連打で 30 秒止まる
// （memory: psp-go-ef0-tiny-read-stall、TH08 r141/r150）。DLS の解析は
// チャンク見出し（8〜20 バイト）を大量に読むので、機種側では
//   (1) ファイルを丸ごと RAM に読んで MemorySource を使う（TH08 r150 と同じ判断）、
//   (2) 大きなブロックで先読みする読み口を自前で書く、
// のどちらかにすること。極小 read をそのまま sceIoRead に流さない。
#ifndef TDLS_BYTE_SOURCE_HPP
#define TDLS_BYTE_SOURCE_HPP

#include <string.h>

#include "types.hpp"

namespace tdls {

class ByteSource
{
  public:
    virtual ~ByteSource() {}
    virtual u32 size() const = 0;
    // [offset, offset+len) を dst へ。範囲外や読み失敗は false。
    virtual bool read(u32 offset, void *dst, u32 len) = 0;
};

class MemorySource : public ByteSource
{
  public:
    MemorySource(const void *data, u32 size) : data_((const u8 *)data), size_(size) {}
    u32 size() const { return size_; }
    bool read(u32 offset, void *dst, u32 len)
    {
        if (offset > size_ || len > size_ - offset)
        {
            return false;
        }
        memcpy(dst, data_ + offset, len);
        return true;
    }
    const u8 *data() const { return data_; }

  private:
    const u8 *data_;
    u32 size_;
};

} // namespace tdls

#endif
