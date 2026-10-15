// Host test for the same diagnostic-only row hashing used by PspGuGfx.
#include "../../psp/graphics/RenderHashTest.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    unsigned char pixels[5 * 4 * 4]{}; // visible 4x4, stride 5
    const th06renderhash::Rect mask{2, 2, 4, 4};
    auto blank = th06renderhash::Pixels(pixels, 4, 4, 5, mask);
    assert(!blank.valid && blank.nonBlack == 0 && blank.varied == 0);
    for (unsigned int y = 0; y < 4; ++y)
        for (unsigned int x = 0; x < 4; ++x)
        {
            auto *p = pixels + (y * 5 + x) * 4;
            p[0] = static_cast<unsigned char>(1 + x + y * 4);
            p[3] = 255;
        }
    auto first = th06renderhash::Pixels(pixels, 4, 4, 5, mask);
    assert(first.pixels == 12 && first.nonBlack == 12 && first.varied == 11);
    unsigned char compact[4 * 4 * 4];
    for (unsigned int y = 0; y < 4; ++y)
        std::memcpy(compact + y * 16, pixels + y * 20, 16);
    unsigned char reference[32];
    tdls::sha256Buffer(compact, sizeof(compact), reference);
    assert(!std::memcmp(first.raw, reference, 32));
    pixels[(2 * 5 + 2) * 4] ^= 1; // an FPS-only change
    auto fpsChange = th06renderhash::Pixels(pixels, 4, 4, 5, mask);
    assert(std::memcmp(first.raw, fpsChange.raw, 32));
    assert(!std::memcmp(first.noFps, fpsChange.noFps, 32));
    pixels[(0 * 5 + 1) * 4] ^= 1; // one real non-FPS pixel
    auto gameChange = th06renderhash::Pixels(pixels, 4, 4, 5, mask);
    assert(std::memcmp(fpsChange.noFps, gameChange.noFps, 32));
    pixels[(0 * 5 + 4) * 4] ^= 1; // padding must be ignored
    auto paddingChange = th06renderhash::Pixels(pixels, 4, 4, 5, mask);
    assert(!std::memcmp(gameChange.raw, paddingChange.raw, 32));
    assert(!std::memcmp(gameChange.noFps, paddingChange.noFps, 32));
    assert(!th06renderhash::Pixels(nullptr, 4, 4, 5, mask).valid);
    assert(!th06renderhash::Pixels(pixels, 4, 4, 3, mask).valid);
    assert(!th06renderhash::Pixels(pixels, 4, 4, 5, {3, 2, 2, 4}).valid);
    static unsigned char larger[100 * 4];
    for (unsigned int x = 0; x < 100; ++x)
        larger[x * 4] = 42;
    assert(!th06renderhash::Pixels(larger, 100, 1, 100, {0, 0, 0, 0}).valid);
    for (unsigned int x = 0; x < 100; ++x)
        larger[x * 4] = static_cast<unsigned char>(x + 1);
    assert(th06renderhash::Pixels(larger, 100, 1, 100, {0, 0, 0, 0}).valid);
    // Colour confined to the excluded clock rectangle is not a real image.
    static unsigned char fpsOnly[200 * 4];
    for (unsigned int x = 0; x < 100; ++x)
        fpsOnly[(100 + x) * 4] = static_cast<unsigned char>(x + 1);
    assert(!th06renderhash::Pixels(fpsOnly, 100, 2, 100, {0, 1, 100, 2}).valid);
    std::puts("render pixel hash: real bytes/padding/FPS-only/one-pixel/null/constant checks passed");
}
