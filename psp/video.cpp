#include "video.hpp"

#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
#include <cstdint>

#include "diag.hpp"

namespace {
constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 272;
constexpr int kStride = 512;
volatile unsigned int *gDraw; // unsigned int, not uint32_t: the PSP toolchain's uint32_t is long
bool gInitialized;
unsigned int gPresentCount;
}

extern "C" int th06_psp_video_init()
{
    if (gInitialized)
        return 0;
    // Moonlight's real-hardware path: uncached GE eDRAM base, one display
    // buffer, and no GU state.  TH06's software renderer already supplies
    // final pixels, so a second graphics pipeline is unnecessary here.
    const auto edram = reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr());
    gDraw = reinterpret_cast<volatile unsigned int *>(0x40000000u | edram);
    sceDisplaySetMode(0, kScreenWidth, kScreenHeight);
    sceDisplaySetFrameBuf(const_cast<unsigned int *>(gDraw), kStride,
                          PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    gInitialized = true;
    return 0;
}

extern "C" volatile unsigned int *th06_psp_video_pixels()
{
    return gDraw;
}

extern "C" int th06_psp_video_stride()
{
    return kStride;
}

extern "C" int th06_psp_video_width()
{
    return kScreenWidth;
}

extern "C" int th06_psp_video_height()
{
    return kScreenHeight;
}

extern "C" unsigned int th06_psp_video_present_count()
{
    return gPresentCount;
}

extern "C" void th06_psp_video_note_presented()
{
    ++gPresentCount;
    th06_psp_diag_frame_presented();
}

extern "C" void th06_psp_video_fill(unsigned int color)
{
    if (!gInitialized)
        th06_psp_video_init();
    if (!gInitialized)
        return;

    for (int y = 0; y < kScreenHeight; ++y)
    {
        volatile unsigned int *draw = gDraw + y * kStride;
        for (int x = 0; x < kStride; ++x)
            draw[x] = color;
    }
    sceDisplayWaitVblankStart();
}

extern "C" void th06_psp_video_present(const unsigned int *src, int width, int height, int pitchBytes)
{
    if (!gInitialized || !src || width <= 0 || height <= 0)
        return;

    // Progress, not a timer, is what retires the boot diagnostics: the overlay
    // yields the screen on the first frame and the record closes once the game
    // is actually running, so nothing diagnostic survives into normal play.
    th06_psp_video_note_presented();

    const auto *source = reinterpret_cast<const std::uint8_t *>(src);
    if (width == kScreenWidth && height == kScreenHeight)
    {
        for (int y = 0; y < kScreenHeight; ++y)
        {
            const auto *row = reinterpret_cast<const unsigned int *>(source + y * pitchBytes);
            volatile unsigned int *dst = gDraw + y * kStride;
            for (int x = 0; x < kScreenWidth; ++x)
                dst[x] = row[x];
        }
    }
    else
    {
        // First-light fallback only. PSP builds use the direct path above.
        for (int y = 0; y < kScreenHeight; ++y)
        {
            const auto *row = reinterpret_cast<const unsigned int *>(source + (y * height / kScreenHeight) * pitchBytes);
            volatile unsigned int *dst = gDraw + y * kStride;
            for (int x = 0; x < kScreenWidth; ++x)
                dst[x] = row[x * width / kScreenWidth];
        }
    }

    sceDisplayWaitVblankStart();
}
