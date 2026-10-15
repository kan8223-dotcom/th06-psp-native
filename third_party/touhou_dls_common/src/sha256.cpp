#include "tdls/sha256.hpp"

namespace tdls {

namespace {

const u32 kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32u - n)); }

void compress(u32 h[8], const u8 *p)
{
    u32 w[64];
    for (u32 i = 0; i < 16; i++)
    {
        w[i] = rd32be(p + i * 4);
    }
    for (u32 i = 16; i < 64; i++)
    {
        u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (u32 i = 0; i < 64; i++)
    {
        u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + S1 + ch + kRound[i] + w[i];
        u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

} // namespace

void Sha256::init()
{
    h[0] = 0x6a09e667u;
    h[1] = 0xbb67ae85u;
    h[2] = 0x3c6ef372u;
    h[3] = 0xa54ff53au;
    h[4] = 0x510e527fu;
    h[5] = 0x9b05688cu;
    h[6] = 0x1f83d9abu;
    h[7] = 0x5be0cd19u;
    totalBytes = 0;
    blockLen = 0;
}

void Sha256::update(const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    totalBytes += len;
    while (len > 0)
    {
        if (blockLen == 0 && len >= 64)
        {
            compress(h, p);
            p += 64;
            len -= 64;
            continue;
        }
        u32 take = 64u - blockLen;
        if (take > len)
        {
            take = len;
        }
        for (u32 i = 0; i < take; i++)
        {
            block[blockLen + i] = p[i];
        }
        blockLen += take;
        p += take;
        len -= take;
        if (blockLen == 64)
        {
            compress(h, block);
            blockLen = 0;
        }
    }
}

void Sha256::finish(u8 digest[32])
{
    u64 bits = totalBytes * 8u;
    u8 pad = 0x80;
    update(&pad, 1);
    u8 zero = 0;
    while (blockLen != 56)
    {
        update(&zero, 1);
    }
    u8 lenBytes[8];
    for (u32 i = 0; i < 8; i++)
    {
        lenBytes[i] = (u8)((bits >> (56u - 8u * i)) & 0xffu);
    }
    update(lenBytes, 8);
    for (u32 i = 0; i < 8; i++)
    {
        wr32be(digest + i * 4, h[i]);
    }
}

void sha256Hex(const u8 digest[32], char out[65])
{
    static const char kHex[] = "0123456789abcdef";
    for (u32 i = 0; i < 32; i++)
    {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 15u];
    }
    out[64] = 0;
}

void sha256Buffer(const void *data, u32 len, u8 digest[32])
{
    Sha256 s;
    s.init();
    s.update(data, len);
    s.finish(digest);
}

} // namespace tdls
