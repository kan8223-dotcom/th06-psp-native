// SHA-256（FIPS 180-4）。外部ライブラリなし。PC と機種で同じ値を出すため共通部品に持つ。
#ifndef TDLS_SHA256_HPP
#define TDLS_SHA256_HPP

#include "types.hpp"

namespace tdls {

struct Sha256
{
    u32 h[8];
    u64 totalBytes;
    u8 block[64];
    u32 blockLen;

    void init();
    void update(const void *data, u32 len);
    void finish(u8 digest[32]);
};

// digest 32 バイトを小文字 16 進 64 文字 + NUL にする。
void sha256Hex(const u8 digest[32], char out[65]);

// 一発計算の便利関数。
void sha256Buffer(const void *data, u32 len, u8 digest[32]);

} // namespace tdls

#endif
