// th06_ps3: real IMG_Load_RW using portlibs libpng 1.4 + libjpeg v8 (memory sources).
// Output surface is SDL_PIXELFORMAT_RGBA32 = byte order R,G,B,A (endian-free convention).
// Also implements SDL_ConvertSurfaceFormat and SDL_BlitScaled used by AnmManager.
#include <SDL2/SDL_shim.h>

#include <csetjmp>
#include <cstdint>
#include <jpeglib.h>
#include <png.h>

// ---------------------------------------------------------------- helpers
static SDL_Surface *AllocSurfaceRGBA32(int w, int h)
{
    return SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
}

// ---------------------------------------------------------------- PNG (libpng 1.4: custom memory read)
struct PngMemSrc
{
    const Uint8 *data;
    size_t size;
    size_t pos;
};

static void PngReadFn(png_structp png, png_bytep out, png_size_t len)
{
    PngMemSrc *src = (PngMemSrc *)png_get_io_ptr(png);
    if (src->pos + len > src->size)
    {
        png_error(png, "th06_ps3: png read past end");
        return;
    }
    memcpy(out, src->data + src->pos, len);
    src->pos += len;
}

// Packed 16-bit texels use the same native-u16 convention as the renderer.
static Uint16 PackPixel16(Uint32 fmt, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    switch (fmt)
    {
    case SDL_PIXELFORMAT_RGB565:
        return (Uint16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    case SDL_PIXELFORMAT_RGBA4444:
        return (Uint16)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4));
    case SDL_PIXELFORMAT_RGBA5551:
        return (Uint16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7));
    default:
        return 0;
    }
}

static bool StoreRgbaPixel(Uint8 *dst, Uint32 fmt, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    switch (fmt)
    {
    case SDL_PIXELFORMAT_RGBA32:
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = a;
        return true;
    case SDL_PIXELFORMAT_ARGB8888:
        dst[0] = a;
        dst[1] = r;
        dst[2] = g;
        dst[3] = b;
        return true;
    case SDL_PIXELFORMAT_RGB24:
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        return true;
    case SDL_PIXELFORMAT_RGB565:
    case SDL_PIXELFORMAT_RGBA4444:
    case SDL_PIXELFORMAT_RGBA5551:
        *reinterpret_cast<Uint16 *>(dst) = PackPixel16(fmt, r, g, b, a);
        return true;
    default:
        return false;
    }
}

// PSP texture PNGs can be larger than the ANM texture they feed. The generic
// path decodes a full RGBA image, converts another full source-size image, and
// only then downsizes it. Decode one RGBA row at a time and write the sampled
// pixels directly in the final size/format instead. This produces the same
// nearest-neighbour pixels with only one output surface plus a scanline.
extern "C" SDL_Surface *th06_psp_img_load_png_scaled_format(const Uint8 *data, size_t size, Uint32 format,
                                                             int targetWidth, int targetHeight)
{
    if (!data || size < 8 || png_sig_cmp((png_bytep)data, 0, 8) != 0 || targetWidth <= 0 || targetHeight <= 0)
        return nullptr;
    if (format != SDL_PIXELFORMAT_RGBA32 && format != SDL_PIXELFORMAT_ARGB8888 &&
        format != SDL_PIXELFORMAT_RGB24 && format != SDL_PIXELFORMAT_RGB565 &&
        format != SDL_PIXELFORMAT_RGBA4444 && format != SDL_PIXELFORMAT_RGBA5551)
        return nullptr;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png)
        return nullptr;
    png_infop info = png_create_info_struct(png);
    if (!info)
    {
        png_destroy_read_struct(&png, NULL, NULL);
        return nullptr;
    }

    SDL_Surface *surface = nullptr;
    Uint8 *row = nullptr;
    // libpng reports decode errors with longjmp. Keep volatile cleanup copies:
    // ordinary automatic variables modified after setjmp have indeterminate
    // values when control returns through that jump.
    SDL_Surface *volatile surfaceToFree = nullptr;
    Uint8 *volatile rowToFree = nullptr;
    if (setjmp(png_jmpbuf(png)))
    {
        png_destroy_read_struct(&png, &info, NULL);
        free(rowToFree);
        SDL_FreeSurface(surfaceToFree);
        return nullptr;
    }

    PngMemSrc src = {data, size, 0};
    png_set_read_fn(png, &src, PngReadFn);
    png_read_info(png, info);

    const png_uint_32 sourceWidth = png_get_image_width(png, info);
    const png_uint_32 sourceHeight = png_get_image_height(png, info);
    const int bitDepth = png_get_bit_depth(png, info);
    const int colorType = png_get_color_type(png, info);
    if (sourceWidth == 0 || sourceHeight == 0 || png_get_interlace_type(png, info) != PNG_INTERLACE_NONE)
    {
        png_destroy_read_struct(&png, &info, NULL);
        return nullptr;
    }

    if (colorType == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (bitDepth == 16)
        png_set_strip_16(png);
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    const png_size_t rowBytes = png_get_rowbytes(png, info);
    if (rowBytes < static_cast<png_size_t>(sourceWidth) * 4u)
    {
        png_destroy_read_struct(&png, &info, NULL);
        return nullptr;
    }

    surface = SDL_CreateRGBSurfaceWithFormat(0, targetWidth, targetHeight, SDL_BITSPERPIXEL(format), format);
    surfaceToFree = surface;
    row = static_cast<Uint8 *>(malloc(rowBytes));
    rowToFree = row;
    if (!surface || !row)
    {
        png_destroy_read_struct(&png, &info, NULL);
        free(row);
        SDL_FreeSurface(surface);
        return nullptr;
    }

    int dstY = 0;
    const int dstBytesPerPixel = surface->format->BytesPerPixel;
    for (png_uint_32 srcY = 0; srcY < sourceHeight; ++srcY)
    {
        png_read_row(png, row, NULL);
        while (dstY < targetHeight &&
               (static_cast<uint64_t>(dstY) * sourceHeight) / static_cast<unsigned int>(targetHeight) == srcY)
        {
            Uint8 *dstRow = static_cast<Uint8 *>(surface->pixels) + dstY * surface->pitch;
            for (int dstX = 0; dstX < targetWidth; ++dstX)
            {
                const png_uint_32 srcX = static_cast<png_uint_32>(
                    (static_cast<uint64_t>(dstX) * sourceWidth) / static_cast<unsigned int>(targetWidth));
                const Uint8 *srcPixel = row + static_cast<size_t>(srcX) * 4u;
                if (!StoreRgbaPixel(dstRow + dstX * dstBytesPerPixel, format, srcPixel[0], srcPixel[1], srcPixel[2],
                                    srcPixel[3]))
                    png_error(png, "unsupported target format");
            }
            ++dstY;
        }
    }

    if (dstY != targetHeight)
        png_error(png, "scaled PNG row count mismatch");
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    free(row);
    return surface;
}

static SDL_Surface *LoadPngFromMem(const Uint8 *data, size_t size)
{
    if (size < 8 || png_sig_cmp((png_bytep)data, 0, 8) != 0)
        return nullptr;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png)
        return nullptr;
    png_infop info = png_create_info_struct(png);
    if (!info)
    {
        png_destroy_read_struct(&png, NULL, NULL);
        return nullptr;
    }

    SDL_Surface *surface = nullptr;
    if (setjmp(png_jmpbuf(png)))
    {
        // libpng error path
        png_destroy_read_struct(&png, &info, NULL);
        if (surface)
            SDL_FreeSurface(surface);
        return nullptr;
    }

    PngMemSrc src = {data, size, 0};
    png_set_read_fn(png, &src, PngReadFn);

    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    int bitDepth = png_get_bit_depth(png, info);
    int colorType = png_get_color_type(png, info);

    // Normalize everything to 8-bit RGBA
    if (colorType == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (bitDepth == 16)
        png_set_strip_16(png);
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER); // add alpha if missing

    png_read_update_info(png, info);

    surface = AllocSurfaceRGBA32((int)w, (int)h);
    if (!surface)
    {
        png_destroy_read_struct(&png, &info, NULL);
        return nullptr;
    }

    for (png_uint_32 y = 0; y < h; y++)
        png_read_row(png, (png_bytep)((Uint8 *)surface->pixels + y * surface->pitch), NULL);

    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    return surface;
}

// ---------------------------------------------------------------- JPEG (libjpeg v8 mem source)
struct JpegErrorMgr
{
    jpeg_error_mgr pub;
    jmp_buf jmp;
};

static void JpegErrorExit(j_common_ptr cinfo)
{
    JpegErrorMgr *err = (JpegErrorMgr *)cinfo->err;
    longjmp(err->jmp, 1);
}

static SDL_Surface *LoadJpegFromMem(const Uint8 *data, size_t size)
{
    if (size < 2 || data[0] != 0xFF || data[1] != 0xD8)
        return nullptr;

    jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = JpegErrorExit;

    SDL_Surface *surface = nullptr;
    if (setjmp(jerr.jmp))
    {
        jpeg_destroy_decompress(&cinfo);
        if (surface)
            SDL_FreeSurface(surface);
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)data, (unsigned long)size);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    surface = AllocSurfaceRGBA32((int)cinfo.output_width, (int)cinfo.output_height);
    if (!surface)
        longjmp(jerr.jmp, 1);

    // Decode RGB row then expand to RGBA in place
    Uint8 *rowRGB = (Uint8 *)malloc(cinfo.output_width * 3);
    while (cinfo.output_scanline < cinfo.output_height)
    {
        Uint32 y = cinfo.output_scanline;
        JSAMPROW rows[1] = {rowRGB};
        jpeg_read_scanlines(&cinfo, rows, 1);
        Uint8 *dst = (Uint8 *)surface->pixels + y * surface->pitch;
        for (Uint32 x = 0; x < cinfo.output_width; x++)
        {
            dst[x * 4 + 0] = rowRGB[x * 3 + 0];
            dst[x * 4 + 1] = rowRGB[x * 3 + 1];
            dst[x * 4 + 2] = rowRGB[x * 3 + 2];
            dst[x * 4 + 3] = 0xFF;
        }
    }
    free(rowRGB);

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return surface;
}

// Large TH06 menu/result backgrounds are consumed as RGB24 surfaces.  The
// generic path first allocates a 640x480 RGBA surface (1.2 MiB), then a second
// 900 KiB RGB surface.  PSP can decode JPEG scanlines directly into the final
// RGB24 allocation, avoiding that fragmentation-sensitive peak.
extern "C" SDL_Surface *th06_psp_img_load_jpeg_rgb24(const Uint8 *data, size_t size)
{
    if (!data || size < 2 || data[0] != 0xFF || data[1] != 0xD8)
        return nullptr;

    jpeg_decompress_struct cinfo{};
    JpegErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = JpegErrorExit;

    SDL_Surface *surface = nullptr;
    bool created = false;
    if (setjmp(jerr.jmp))
    {
        if (created)
            jpeg_destroy_decompress(&cinfo);
        if (surface)
            SDL_FreeSurface(surface);
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    created = true;
    jpeg_mem_src(&cinfo, const_cast<unsigned char *>(data), static_cast<unsigned long>(size));
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    surface = SDL_CreateRGBSurfaceWithFormat(0, static_cast<int>(cinfo.output_width),
                                              static_cast<int>(cinfo.output_height), 24,
                                              SDL_PIXELFORMAT_RGB24);
    if (!surface)
        longjmp(jerr.jmp, 1);

    while (cinfo.output_scanline < cinfo.output_height)
    {
        JSAMPROW row = static_cast<Uint8 *>(surface->pixels) +
                       static_cast<size_t>(cinfo.output_scanline) * surface->pitch;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return surface;
}

// ---------------------------------------------------------------- IMG_Load_RW (real)
SDL_Surface *IMG_Load_RW(SDL_RWops *src, int freesrc)
{
    // Slurp the RWops (PBG3 entries come via RWFromConstMem; files are small)
    Sint64 size = SDL_RWsize(src);
    SDL_Surface *result = nullptr;

    if (size > 0)
    {
        if (src->mem)
        {
            result = LoadPngFromMem(src->mem, (size_t)size);
            if (!result)
                result = LoadJpegFromMem(src->mem, (size_t)size);
        }
        else
        {
            Uint8 *buf = (Uint8 *)malloc((size_t)size);
            if (buf)
            {
                SDL_RWseek(src, 0, RW_SEEK_SET);
                SDL_RWread(src, buf, 1, (size_t)size);
                result = LoadPngFromMem(buf, (size_t)size);
                if (!result)
                    result = LoadJpegFromMem(buf, (size_t)size);
                free(buf);
            }
        }
    }

    if (freesrc && src)
        SDL_RWclose(src);
    return result;
}

// ---------------------------------------------------------------- format conversion
SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 fmt, Uint32)
{
    if (!src)
        return nullptr;

    // Source is always our RGBA32 (byte order R,G,B,A)
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, src->w, src->h, SDL_BITSPERPIXEL(fmt), fmt);
    if (!dst)
        return nullptr;

    for (int y = 0; y < src->h; y++)
    {
        const Uint8 *s = (const Uint8 *)src->pixels + y * src->pitch;
        Uint8 *d = (Uint8 *)dst->pixels + y * dst->pitch;
        for (int x = 0; x < src->w; x++)
        {
            Uint8 r = s[x * 4 + 0], g = s[x * 4 + 1], b = s[x * 4 + 2], a = s[x * 4 + 3];
            switch (fmt)
            {
            case SDL_PIXELFORMAT_RGBA32:
                d[x * 4 + 0] = r;
                d[x * 4 + 1] = g;
                d[x * 4 + 2] = b;
                d[x * 4 + 3] = a;
                break;
            case SDL_PIXELFORMAT_ARGB8888: // stored as native u32 A,R,G,B bytes on BE
                d[x * 4 + 0] = a;
                d[x * 4 + 1] = r;
                d[x * 4 + 2] = g;
                d[x * 4 + 3] = b;
                break;
            case SDL_PIXELFORMAT_RGB24:
                d[x * 3 + 0] = r;
                d[x * 3 + 1] = g;
                d[x * 3 + 2] = b;
                break;
            default: // packed 16-bit
                ((Uint16 *)d)[x] = PackPixel16(fmt, r, g, b, a);
                break;
            }
        }
    }
    return dst;
}

// Nearest-neighbor scaled blit (RGBA32/same-format surfaces; used by the ANM size hack)
int SDL_BlitScaled(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    if (!src || !dst || !src->format || !dst->format)
        return -1;
    if (src->format->format != dst->format->format)
        return -1;

    SDL_Rect sr = srcrect ? *srcrect : (SDL_Rect){0, 0, src->w, src->h};
    SDL_Rect dr = dstrect ? *dstrect : (SDL_Rect){0, 0, dst->w, dst->h};
    if (sr.w <= 0 || sr.h <= 0 || dr.w <= 0 || dr.h <= 0)
        return 0;

    int bpp = src->format->BytesPerPixel;
    for (int y = 0; y < dr.h; y++)
    {
        int sy = sr.y + (int)((Sint64)y * sr.h / dr.h);
        if (sy >= src->h)
            sy = src->h - 1;
        const Uint8 *srow = (const Uint8 *)src->pixels + sy * src->pitch;
        Uint8 *drow = (Uint8 *)dst->pixels + (dr.y + y) * dst->pitch;
        for (int x = 0; x < dr.w; x++)
        {
            int sx = sr.x + (int)((Sint64)x * sr.w / dr.w);
            if (sx >= src->w)
                sx = src->w - 1;
            memcpy(drow + (dr.x + x) * bpp, srow + sx * bpp, bpp);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- SDL_ConvertPixels (real)
// Sources: byte-order RGB24/RGBA32 or native-u16 565/5551/4444. Dest: ARGB8888 as native u32
// (matches ZunColor 0xAARRGGBB semantics used by the Software renderer texel store).
int SDL_ConvertPixels(int width, int height, Uint32 src_format, const void *src, int src_pitch, Uint32 dst_format,
                      void *dst, int dst_pitch)
{
    if (dst_format != SDL_PIXELFORMAT_ARGB8888)
        return -1;

    for (int y = 0; y < height; y++)
    {
        const Uint8 *s = (const Uint8 *)src + y * src_pitch;
        Uint32 *d = (Uint32 *)((Uint8 *)dst + y * dst_pitch);
        for (int x = 0; x < width; x++)
        {
            Uint8 r, g, b, a;
            switch (src_format)
            {
            case SDL_PIXELFORMAT_RGB24:
                r = s[x * 3 + 0];
                g = s[x * 3 + 1];
                b = s[x * 3 + 2];
                a = 0xFF;
                break;
            case SDL_PIXELFORMAT_RGBA32:
                r = s[x * 4 + 0];
                g = s[x * 4 + 1];
                b = s[x * 4 + 2];
                a = s[x * 4 + 3];
                break;
            case SDL_PIXELFORMAT_RGB565: {
                Uint16 p = ((const Uint16 *)s)[x];
                r = (Uint8)(((p >> 11) & 0x1F) << 3);
                g = (Uint8)(((p >> 5) & 0x3F) << 2);
                b = (Uint8)((p & 0x1F) << 3);
                a = 0xFF;
                break;
            }
            case SDL_PIXELFORMAT_RGBA5551: {
                Uint16 p = ((const Uint16 *)s)[x];
                r = (Uint8)(((p >> 11) & 0x1F) << 3);
                g = (Uint8)(((p >> 6) & 0x1F) << 3);
                b = (Uint8)(((p >> 1) & 0x1F) << 3);
                a = (p & 1) ? 0xFF : 0;
                break;
            }
            case SDL_PIXELFORMAT_RGBA4444: {
                Uint16 p = ((const Uint16 *)s)[x];
                r = (Uint8)(((p >> 12) & 0xF) << 4);
                g = (Uint8)(((p >> 8) & 0xF) << 4);
                b = (Uint8)(((p >> 4) & 0xF) << 4);
                a = (Uint8)((p & 0xF) << 4);
                break;
            }
            case SDL_PIXELFORMAT_ARGB8888:
                d[x] = ((const Uint32 *)s)[x];
                continue;
            default:
                return -1;
            }
            d[x] = ((Uint32)a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | b;
        }
    }
    return 0;
}
