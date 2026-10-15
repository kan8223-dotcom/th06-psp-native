// Host byte oracle for the actual streaming shim JPEG implementation.
// The reference is the pre-streaming public PspGuGfx.cpp
// BuildSurfaceAxisSamples / BindCachedSurface bilinear loop / GU_PSM_5650
// WriteTexturePixel branch. It reads a FULL-resolution libjpeg RGB image;
// it does not call, share resampling code with, or reimplement the streaming
// decoder. Copyright/assets and every generated image remain private.
#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <jpeglib.h>
#include "psp_jpeg_gu.hpp"

// Failure injection is restricted to the actual helper's two-row allocation
// and its decoder-state calloc. Shared libjpeg keeps its ordinary allocator.
static std::size_t gFailMallocBytes, gFailCallocBytes;
static unsigned int gFailureHits;
extern "C" void *__real_malloc(std::size_t bytes);
extern "C" void *__real_calloc(std::size_t count, std::size_t bytes);
extern "C" void *__wrap_malloc(std::size_t bytes)
{
    if (gFailMallocBytes && bytes == gFailMallocBytes) { ++gFailureHits; return nullptr; }
    return __real_malloc(bytes);
}
extern "C" void *__wrap_calloc(std::size_t count, std::size_t bytes)
{
    if (gFailCallocBytes && count == 1 && bytes == gFailCallocBytes) { ++gFailureHits; return nullptr; }
    return __real_calloc(count, bytes);
}

struct FullRgb
{
    unsigned int width;
    unsigned int height;
    std::vector<unsigned char> pixels;
};

struct OracleJpegError
{
    jpeg_error_mgr pub;
    std::jmp_buf jump;
};

static void OracleJpegErrorExit(j_common_ptr info)
{
    auto *error = reinterpret_cast<OracleJpegError *>(info->err);
    std::longjmp(error->jump, 1);
}

static FullRgb DecodeLegacyFullRgb(const std::vector<unsigned char> &encoded)
{
    jpeg_decompress_struct info{};
    OracleJpegError error{};
    info.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = OracleJpegErrorExit;
    unsigned char *volatile allocation = nullptr;
    volatile bool created = false;
    if (setjmp(error.jump))
    {
        std::free(allocation);
        if (created) jpeg_destroy_decompress(&info);
        throw std::runtime_error("legacy full-resolution JPEG decode failed");
    }
    jpeg_create_decompress(&info);
    created = true;
    jpeg_mem_src(&info, const_cast<unsigned char *>(encoded.data()), encoded.size());
    jpeg_read_header(&info, TRUE);
    // Exact legacy th06_psp_img_load_jpeg_rgb24 defaults: RGB, full size,
    // default IDCT/chroma upsampling. A lower-resolution IDCT is no oracle.
    info.out_color_space = JCS_RGB;
    jpeg_start_decompress(&info);
    const unsigned int width = info.output_width, height = info.output_height;
    if (!width || !height || width > 4096 || height > 4096 || info.output_components != 3)
        std::longjmp(error.jump, 1);
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 3u;
    allocation = static_cast<unsigned char *>(std::malloc(bytes));
    if (!allocation) std::longjmp(error.jump, 1);
    while (info.output_scanline < info.output_height)
    {
        JSAMPROW row = allocation + static_cast<std::size_t>(info.output_scanline) * width * 3u;
        if (jpeg_read_scanlines(&info, &row, 1) != 1) std::longjmp(error.jump, 1);
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    FullRgb image{width, height, {allocation, allocation + bytes}};
    std::free(allocation);
    return image;
}

namespace LegacyPublicGe
{
struct SurfaceAxisSample
{
    unsigned short first;
    unsigned short second;
    unsigned short fraction;
};

// Kept verbatim from the existing public GE filter, rather than deriving a
// second sampling rule from the new streaming helper.
static void BuildSurfaceAxisSamples(SurfaceAxisSample *samples, unsigned int destinationSize,
                                    unsigned int sourceSize)
{
    for (unsigned int i = 0; i < destinationSize; ++i)
    {
        long long fixed = static_cast<long long>((static_cast<unsigned long long>(i * 2u + 1u) * sourceSize *
                                                   128u) /
                                                  destinationSize) -
                          128;
        const long long last = static_cast<long long>(sourceSize - 1u) * 256;
        fixed = std::max(0ll, std::min(last, fixed));
        samples[i].first = static_cast<unsigned short>(fixed >> 8);
        samples[i].second = static_cast<unsigned short>(std::min(sourceSize - 1u,
                                                                 samples[i].first + 1u));
        samples[i].fraction = static_cast<unsigned short>(fixed & 255);
    }
}

static void WriteTexturePixel(unsigned char *pixels, unsigned int storageWidth,
                              unsigned int x, unsigned int y, unsigned int r,
                              unsigned int g, unsigned int b)
{
    // Existing WriteTexturePixel's swizzled GU_PSM_5650 branch, including
    // PSP's red-low-bit order (SDL RGB565 uses the opposite red/blue order).
    const unsigned int bytesPerPixel = 2u;
    const unsigned int rowBytes = storageWidth * bytesPerPixel;
    const unsigned int byteX = x * bytesPerPixel;
    const unsigned int byteOffset =
        (((y >> 3) * (rowBytes >> 4) + (byteX >> 4)) << 7) +
        ((y & 7u) << 4) + (byteX & 15u);
    auto *destination = pixels + byteOffset;
    *reinterpret_cast<unsigned short *>(destination) =
        static_cast<unsigned short>((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
}

static std::vector<unsigned char> Cache(const FullRgb &image, unsigned int storageWidth,
                                      unsigned int storageHeight, unsigned int dstWidth,
                                      unsigned int dstHeight)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(storageWidth) * storageHeight * 2u, 0);
    SurfaceAxisSample xSamples[512], ySamples[512];
    BuildSurfaceAxisSamples(xSamples, dstWidth, image.width);
    BuildSurfaceAxisSamples(ySamples, dstHeight, image.height);
    const unsigned char *source = image.pixels.data();
    const unsigned int contentWidth = image.width;
    // Existing BindCachedSurface loop. Its four reads from the full decoded
    // image are independent of the streaming helper's two-row retention.
    for (unsigned int y = 0; y < dstHeight; ++y)
    {
        const SurfaceAxisSample sy = ySamples[y];
        const unsigned int wy0 = 256u - sy.fraction;
        const unsigned int wy1 = sy.fraction;
        for (unsigned int x = 0; x < dstWidth; ++x)
        {
            const SurfaceAxisSample sx = xSamples[x];
            const unsigned int wx0 = 256u - sx.fraction;
            const unsigned int wx1 = sx.fraction;
            const unsigned char *p00 = source + (sy.first * contentWidth + sx.first) * 3;
            const unsigned char *p10 = source + (sy.first * contentWidth + sx.second) * 3;
            const unsigned char *p01 = source + (sy.second * contentWidth + sx.first) * 3;
            const unsigned char *p11 = source + (sy.second * contentWidth + sx.second) * 3;
            unsigned int rgb[3];
            for (unsigned int component = 0; component < 3; ++component)
            {
                const unsigned int top = p00[component] * wx0 + p10[component] * wx1;
                const unsigned int bottom = p01[component] * wx0 + p11[component] * wx1;
                rgb[component] = (top * wy0 + bottom * wy1 + 32768u) >> 16;
            }
            WriteTexturePixel(pixels.data(), storageWidth, x, y, rgb[0], rgb[1], rgb[2]);
        }
    }
    return pixels;
}
} // namespace LegacyPublicGe

static std::uint64_t Fnv64(const unsigned char *bytes, std::size_t size)
{
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

static unsigned int CheckResourceFailures(const std::vector<unsigned char> &encoded, unsigned int sourceWidth)
{
    unsigned int failures = 0, cases = 0;
    std::vector<unsigned char> destination(512u * 512u * 2u + 32u, 0xa5);
    auto reject = [&](const char *name, const unsigned char *data, std::size_t bytes, void *output,
                      std::size_t capacity, unsigned int sw, unsigned int sh, unsigned int tw, unsigned int th)
    {
        std::fill(destination.begin(), destination.end(), 0xa5);
        const int accepted = th06_psp_img_load_jpeg_gu565(data, bytes, output, capacity, sw, sh, tw, th);
        const bool untouched = std::all_of(destination.begin(), destination.end(), [](unsigned char b) { return b == 0xa5; });
        const bool ok = !accepted && untouched;
        ++cases;
        if (!ok) ++failures;
        std::printf("JPEG_CACHE_REJECT %s %s accepted=%d caller_untouched=%d\n", name, ok ? "PASS" : "FAIL", accepted, untouched);
    };
    unsigned char *output = destination.data() + 16;
    const std::size_t capacity = 512u * 512u * 2u;
    const unsigned char badHeader[] = {0xff, 0xd8, 0xff, 0xd9};
    reject("undersized-caller-buffer", encoded.data(), encoded.size(), output, capacity-1, 512, 512, 480, 272);
    reject("storage-zero", encoded.data(), encoded.size(), output, capacity, 0, 512, 480, 272);
    reject("storage-not-power-two", encoded.data(), encoded.size(), output, capacity, 511, 512, 480, 272);
    reject("storage-too-large", encoded.data(), encoded.size(), output, capacity, 1024, 512, 480, 272);
    reject("target-zero", encoded.data(), encoded.size(), output, capacity, 512, 512, 0, 272);
    reject("target-exceeds-storage", encoded.data(), encoded.size(), output, capacity, 512, 512, 513, 272);
    reject("null-source", nullptr, encoded.size(), output, capacity, 512, 512, 480, 272);
    reject("short-source", encoded.data(), 1, output, capacity, 512, 512, 480, 272);
    reject("null-destination", encoded.data(), encoded.size(), nullptr, capacity, 512, 512, 480, 272);
    reject("fatal-invalid-JPEG-header", badHeader, sizeof(badHeader), output, capacity, 512, 512, 480, 272);
    unsigned int width = 0, height = 0;
    const bool dimensionsReject =
        !th06_psp_img_jpeg_dimensions(encoded.data(), encoded.size(), nullptr, &height) &&
        !th06_psp_img_jpeg_dimensions(encoded.data(), encoded.size(), &width, nullptr);
    ++cases;
    if (!dimensionsReject) ++failures;
    std::printf("JPEG_CACHE_REJECT null-dimension-output %s\n", dimensionsReject ? "PASS" : "FAIL");
    gFailMallocBytes = static_cast<std::size_t>(sourceWidth) * 3u * 2u;
    gFailureHits = 0;
    reject("two-row-allocation-OOM", encoded.data(), encoded.size(), output, capacity, 512, 512, 480, 272);
    if (gFailureHits != 1) ++failures;
    gFailMallocBytes = 0;
    gFailCallocBytes = sizeof(jpeg_decompress_struct);
    gFailureHits = 0;
    reject("decoder-state-allocation-OOM", encoded.data(), encoded.size(), output, capacity, 512, 512, 480, 272);
    if (gFailureHits != 1) ++failures;
    gFailCallocBytes = 0;
    std::printf("JPEG_CACHE_RESOURCE %s CASES%u FAILURES%u\n", failures ? "FAIL" : "PASS", cases, failures);
    return failures;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: jpeg_cache_test private.jpg [private2.jpg ...]\n");
        return 2;
    }
    std::printf("JPEG_CACHE_ORACLE fullRGB+prestream-public-GE vs actual-shim-stream JPEG_LIB_VERSION=%d\n", JPEG_LIB_VERSION);
    std::printf("path\tsource\ttarget\tstorage\tbytes\treference_fnv64\tstream_fnv64\tmismatch_bytes\n");
    unsigned int cases = 0, failed = 0, staffCases = 0;
    for (int index = 1; index < argc; ++index)
    {
        try
        {
            std::ifstream input(argv[index], std::ios::binary);
            if (!input) throw std::runtime_error("cannot open input");
            std::vector<unsigned char> encoded((std::istreambuf_iterator<char>(input)), {});
            const FullRgb image = DecodeLegacyFullRgb(encoded);
            unsigned int width = 0, height = 0;
            if (!th06_psp_img_jpeg_dimensions(encoded.data(), encoded.size(), &width, &height) ||
                width != image.width || height != image.height)
                throw std::runtime_error("stream header dimensions differ from legacy full decode");
            if (width != 640 || (height != 480 && height != 780))
                throw std::runtime_error("unexpected production JPEG geometry");
            if (index == 1) failed += CheckResourceFailures(encoded, image.width);
            const unsigned int dstHeight = (height * 272u + 479u) / 480u;
            for (unsigned int dstWidth : {480u, 362u, 256u})
            {
                const unsigned int storageWidth = dstWidth == 256u ? 256u : 512u;
                const auto expected = LegacyPublicGe::Cache(image, storageWidth, 512u, dstWidth, dstHeight);
                // Nonzero guard/padding seed proves the helper initializes
                // the entire texture; compare ALL swizzled bytes, not content
                // rows alone. Surrounding guards catch caller-buffer writes.
                std::vector<unsigned char> guarded(expected.size() + 32u, 0xa5);
                unsigned char *actual = guarded.data() + 16u;
                if (!th06_psp_img_load_jpeg_gu565(encoded.data(), encoded.size(), actual,
                    expected.size(), storageWidth, 512u, dstWidth, dstHeight))
                    throw std::runtime_error("stream helper rejected valid production JPEG");
                std::size_t mismatches = 0, first = 0;
                for (std::size_t i = 0; i < expected.size(); ++i)
                    if (expected[i] != actual[i]) { if (!mismatches) first = i; ++mismatches; }
                const bool guardsOk = std::all_of(guarded.begin(), guarded.begin()+16, [](unsigned char b) { return b == 0xa5; }) &&
                    std::all_of(guarded.end()-16, guarded.end(), [](unsigned char b) { return b == 0xa5; });
                std::printf("%s\t%ux%u\t%ux%u\t%ux512\t%zu\t%016llx\t%016llx\t%zu\n",
                    argv[index], width, height, dstWidth, dstHeight, storageWidth, expected.size(),
                    static_cast<unsigned long long>(Fnv64(expected.data(), expected.size())),
                    static_cast<unsigned long long>(Fnv64(actual, expected.size())), mismatches);
                ++cases;
                if (height == 780) ++staffCases;
                if (mismatches || !guardsOk)
                {
                    ++failed;
                    std::fprintf(stderr, "FAIL %s %u-width: mismatches=%zu first=%zu want=%02x got=%02x guards=%d\n",
                        argv[index], dstWidth, mismatches, first, expected[first], actual[first], guardsOk);
                }
            }
        }
        catch (const std::exception &error)
        {
            ++failed;
            std::fprintf(stderr, "FAIL %s: %s\n", argv[index], error.what());
        }
    }
    std::printf("JPEG_CACHE_ORACLE %s FILES%d CASES%u STAFF_CASES%u FAILURES%u\n",
                failed ? "FAIL" : "PASS", argc-1, cases, staffCases, failed);
    return failed ? 1 : 0;
}
