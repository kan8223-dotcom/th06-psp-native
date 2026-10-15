#pragma once

// Diagnostic-only pixel hashing. No game state, graphics calls or allocation.
// Call after the existing GU finish/sync and before changing the draw buffer.
#include "tdls/sha256.hpp"

namespace th06renderhash
{
struct Rect
{
    unsigned int left, top, right, bottom; // top-origin, exclusive right/bottom
};

struct Result
{
    tdls::u8 raw[32], noFps[32];
    unsigned int nonBlack, varied, pixels;
    bool valid;
};

inline Result Pixels(const unsigned char *base, unsigned int width, unsigned int height,
                     unsigned int stride, Rect fps)
{
    Result out{};
    if (!base || !width || !height || stride < width || width > 512 || height > 272 ||
        fps.left > fps.right || fps.top > fps.bottom || fps.right > width || fps.bottom > height)
        return out;
    tdls::Sha256 raw, noFps;
    raw.init();
    noFps.init();
    unsigned int firstRgb = 0;
    bool haveFirst = false;
    for (unsigned int y = 0; y < height; ++y)
    {
        const unsigned char *row = base + y * stride * 4;
        raw.update(row, width * 4); // visible RGBA bytes; never the 512-wide padding
        if (y >= fps.top && y < fps.bottom)
        {
            noFps.update(row, fps.left * 4);
            noFps.update(row + fps.right * 4, (width - fps.right) * 4);
        }
        else
            noFps.update(row, width * 4);
        for (unsigned int x = 0; x < width; ++x)
        {
            if (y >= fps.top && y < fps.bottom && x >= fps.left && x < fps.right)
                continue;
            const unsigned char *p = row + x * 4;
            const unsigned int rgb = p[0] | (p[1] << 8) | (p[2] << 16);
            if (!haveFirst)
            {
                firstRgb = rgb;
                haveFirst = true;
            }
            ++out.pixels;
            out.nonBlack += rgb != 0;
            out.varied += rgb != firstRgb;
        }
    }
    raw.finish(out.raw);
    noFps.finish(out.noFps);
    // A blank/constant framebuffer is never evidence of rendering equality.
    // Check outside the FPS rectangle, so a clock label alone cannot pass.
    out.valid = haveFirst && out.nonBlack > 64 && out.varied > 64;
    return out;
}
} // namespace th06renderhash

#if defined(TH06_PSP_RENDER_HASH_TEST)
extern "C" int th06_psp_render_hash_frame(unsigned int *stage, unsigned int *frame,
                                         th06renderhash::Rect *fps, int *fpsValid);
#endif
