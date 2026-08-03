#include "TextHelper.hpp"
#include "GameErrorContext.hpp"
#include "GameWindow.hpp"
#include "Supervisor.hpp"
#include "i18n.hpp"

#include "thirdparty/sjis_converter.h"

#include <SDL2/SDL_ttf.h>
#include <algorithm>
#include <cstring>

#ifdef TH06_PS3
#include <sys/systime.h>
#endif

static TTF_Font *g_Font;
static bool g_DeferTextureUploads;
static TextureData *g_DeferredTextures[64];
static i32 g_DeferredTextureCount;

bool isUTF8Encoded(const char *string);

#ifdef TH06_PS3
static u32 g_TextPerfSerial;
static u32 g_TextPerfTotalUs;
static u32 g_TextPerfRasterUs;
static u32 g_TextPerfComposeUs;
static u32 g_TextPerfUploadUs;

static u64 TextPerfNowUs()
{
    u64 sec, nsec;
    sysGetCurrentTime(&sec, &nsec);
    return sec * 1000000ull + nsec / 1000ull;
}

extern "C" void th06_text_perf_snapshot(u32 *serial, u32 *totalUs, u32 *rasterUs, u32 *composeUs, u32 *uploadUs)
{
    *serial = g_TextPerfSerial;
    *totalUs = g_TextPerfTotalUs;
    *rasterUs = g_TextPerfRasterUs;
    *composeUs = g_TextPerfComposeUs;
    *uploadUs = g_TextPerfUploadUs;
}
#endif

static void UploadTextTexture(TextureData *texture)
{
    if (texture == NULL || texture->textureData == NULL)
        return;
    g_AnmManager->SetCurrentTexture(texture->handle);
    g_GfxBackend->SetTextureImage(texture->width, texture->height, PIXEL_RGBA, PIXEL_UNSIGNED_BYTE,
                                  texture->textureData);
}

void TextHelper::BeginTextureUploadBatch()
{
    g_DeferTextureUploads = true;
    g_DeferredTextureCount = 0;
}

void TextHelper::EndTextureUploadBatch()
{
    for (i32 i = 0; i < g_DeferredTextureCount; i++)
        UploadTextTexture(g_DeferredTextures[i]);
    g_DeferredTextureCount = 0;
    g_DeferTextureUploads = false;
}

static void QueueTextTextureUpload(TextureData *texture)
{
    if (!g_DeferTextureUploads)
    {
        UploadTextTexture(texture);
        return;
    }

    for (i32 i = 0; i < g_DeferredTextureCount; i++)
    {
        if (g_DeferredTextures[i] == texture)
            return;
    }
    if (g_DeferredTextureCount < (i32)(sizeof(g_DeferredTextures) / sizeof(g_DeferredTextures[0])))
    {
        g_DeferredTextures[g_DeferredTextureCount++] = texture;
        return;
    }

    // The music room uses only a handful of atlases; this is a defensive
    // overflow path which preserves correctness for future callers.
    UploadTextTexture(texture);
}

static void PrewarmBombNameGlyphs()
{
    // Noto CJK's first rasterization of a Japanese bomb name costs about
    // 105 ms on the real PS3 PPU.  Keep that one-time font work out of the
    // gameplay frame.  All four names use the same 15px ANM font (rendered at
    // 2x for downsampling), and SDL_ttf/FreeType retains the resulting glyphs
    // for later calls.
    static const char *const names[] = {
        TH_REIMU_A_BOMB_NAME,
        TH_REIMU_B_BOMB_NAME,
        TH_MARISA_A_BOMB_NAME,
        TH_MARISA_B_BOMB_NAME,
    };
    for (u32 i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        TextHelper::PrewarmGlyphs(names[i], 15);
}

TextHelper::TextHelper()
{
    //    this->format = (D3DFORMAT)-1;
    //    this->width = 0;
    //    this->height = 0;
    //    this->hdc = 0;
    //    this->gdiObj2 = 0;
    //    this->gdiObj = 0;
    //    this->buffer = NULL;
}

TextHelper::~TextHelper()
{
    TTF_Quit();
}

#define TEXT_BUFFER_HEIGHT 64

// Extended to initialize all globals for text helper
ZunResult TextHelper::CreateTextBuffer()
{
    TTF_Init();

    // Primary font is MSゴシック, which is nonfree and has to be taken from a Windows install
    // Fallback is Noto Sans Regular (JP) which is redistributable
    if ((g_Font = TTF_OpenFont(TH_PRIMARY_FONT_FILENAME, 10), g_Font == NULL) &&
        (std::printf("%s\n", TTF_GetError()), g_Font = TTF_OpenFont(TH_FALLBACK_FONT_FILENAME, 10), g_Font == NULL))
    {
        std::printf("%s\n", TTF_GetError());

        g_GameErrorContext.Fatal(TH_ERR_FONTS_NOT_FOUND);
        return ZUN_ERROR;
    }

    PrewarmBombNameGlyphs();

    g_TextBufferSurface =
        SDL_CreateRGBSurfaceWithFormat(0, GAME_WINDOW_WIDTH, TEXT_BUFFER_HEIGHT, 32, SDL_PIXELFORMAT_RGBA32);

    SDL_SetSurfaceBlendMode(g_TextBufferSurface, SDL_BLENDMODE_NONE);

    return ZUN_SUCCESS;
}

bool TextHelper::InvertAlpha(i32 x, i32 y, i32 spriteWidth, i32 fontHeight)
{
    u8 *bufferCursor;
    i32 gradientArea;
    i32 i = 0;

    gradientArea = spriteWidth * fontHeight;

    SDL_LockSurface(g_TextBufferSurface);

    // In D3D EoSD this function mostly inverts the alpha, but on A1R5G5B5 surfaces specifically it also
    //   creates a gradient. D3D EoSD will always attempt to create an A1R5G5B5 surface for the text buffer,
    //   will only attempt use other formats as a fallback, and in those cases the text will be bugged anyway.
    //   As part of the port from GDI to SDL_ttf, we've converted the text buffer surface to always be RGBA32
    //   and no longer need the alpha inversion, but we still want that gradient to be applied

    for (bufferCursor = (u8 *)g_TextBufferSurface->pixels; i < gradientArea; i++, bufferCursor += 4)
    {
        if (bufferCursor[3]) // A
        {
            bufferCursor[0] = bufferCursor[0] - bufferCursor[0] * i / gradientArea / 2; // R
            bufferCursor[1] = bufferCursor[1] - bufferCursor[1] * i / gradientArea / 2; // G
            bufferCursor[2] = bufferCursor[2] - bufferCursor[2] * i / gradientArea / 4; // B
        }
    }

    SDL_UnlockSurface(g_TextBufferSurface);

    return true;
}

// Text strings in asset files are encoded using Shift_JIS. This allows RenderTextToTexture to handle both UTF-8 and
// Shift_JIS. This also does not check for overlong encoding, but that shouldn't matter
bool isUTF8Encoded(const char *string)
{
#define UTF8_1BYTE_MASK 0x80
#define UTF8_2BYTE_MASK 0xE0
#define UTF8_3BYTE_MASK 0xF0
#define UTF8_4BYTE_MASK 0xF8

#define UTF8_2NDBYTE_MASK 0xC0

// 0xxx xxxx
#define UTF8_1BYTE_PREFIX 0x00
// 110x xxxx
#define UTF8_2BYTE_PREFIX 0xC0
// 1110 xxxx
#define UTF8_3BYTE_PREFIX 0xE0
// 1111 0xxx
#define UTF8_4BYTE_PREFIX 0xF0

// 10xx xxxx
#define UTF8_2NDBYTE_PREFIX 0x80

    bool isMultiByteParse = false;
    int codepointLen = 0;

    while (*string != '\0')
    {
        unsigned char c = *(unsigned char *)string;

        if (!isMultiByteParse)
        {
            if ((c & UTF8_1BYTE_MASK) != UTF8_1BYTE_PREFIX)
            {
                isMultiByteParse = true;

                if ((c & UTF8_2BYTE_MASK) == UTF8_2BYTE_PREFIX)
                    codepointLen = 1;
                else if ((c & UTF8_3BYTE_MASK) == UTF8_3BYTE_PREFIX)
                    codepointLen = 2;
                else if ((c & UTF8_4BYTE_MASK) == UTF8_4BYTE_PREFIX)
                    codepointLen = 3;
                else
                    return false;
            }
        }
        else
        {
            if ((c & UTF8_2NDBYTE_MASK) != UTF8_2NDBYTE_PREFIX)
                return false;

            if (--codepointLen == 0)
                isMultiByteParse = false;
        }

        string++;
    }

    return true;

#undef UTF8_1BYTE_MASK
#undef UTF8_2BYTE_MASK
#undef UTF8_3BYTE_MASK
#undef UTF8_4BYTE_MASK

#undef UTF8_2NDBYTE_MASK

#undef UTF8_1BYTE_PREFIX
#undef UTF8_2BYTE_PREFIX
#undef UTF8_3BYTE_PREFIX
#undef UTF8_4BYTE_PREFIX

#undef UTF8_2NDBYTE_PREFIX
}

void TextHelper::PrewarmGlyphs(const char *string, i32 fontHeight)
{
    if (g_Font == NULL || string == NULL || string[0] == '\0')
        return;

    char *convertedText = NULL;
    const char *utf8Text = string;
    if (!isUTF8Encoded(string))
    {
        convertedText = sjis2utf8(string);
        if (convertedText == NULL)
            return;
        utf8Text = convertedText;
    }

    const SDL_Color white = {0xff, 0xff, 0xff, 0xff};
    TTF_SetFontSize(g_Font, std::max(1, fontHeight * 2));
    SDL_Surface *surface = TTF_RenderUTF8_Blended(g_Font, utf8Text, white);
    if (surface != NULL)
        SDL_FreeSurface(surface);
    free(convertedText);
}

void SurfaceOverwriteBlend(SDL_Surface *srcSurface, SDL_Surface *dstSurface, u32 x)
{
    // Dest surface is RGBA32.  Read the source through its declared shim format;
    // relying on a native u32 A8R8G8B8 load is fragile on PS3/BE and made dynamic
    // text vulnerable to disappearing when the TTF surface format changed.
    // We want to overwrite dest unless source has alpha 0

    SDL_LockSurface(srcSurface);
    SDL_LockSurface(dstSurface);

    for (int i = 0; i < srcSurface->h && i < dstSurface->h; i++)
    {
        for (int j = 0; j < srcSurface->w && (int)x + j < dstSurface->w; j++)
        {
            const u8 *srcData = (const u8 *)srcSurface->pixels + i * srcSurface->pitch + j * 4;
            u8 r = 0;
            u8 g = 0;
            u8 b = 0;
            u8 a = 0;
            if (srcSurface->format->format == SDL_PIXELFORMAT_ARGB8888)
            {
                a = srcData[0];
                r = srcData[1];
                g = srcData[2];
                b = srcData[3];
            }
            else
            {
                r = srcData[0];
                g = srcData[1];
                b = srcData[2];
                a = srcData[3];
            }
            if (a != 0)
            {
                u8 *dstData = (u8 *)dstSurface->pixels + i * dstSurface->pitch + (x + j) * 4;
                dstData[0] = r;
                dstData[1] = g;
                dstData[2] = b;
                dstData[3] = a;
            }
        }
    }

    SDL_UnlockSurface(dstSurface);
    SDL_UnlockSurface(srcSurface);
}

static void CopyTextBufferToTexture(SDL_Surface *src, const SDL_Rect &srcRect, u8 *dstPixels, i32 dstTexW,
                                    i32 dstTexH, const SDL_Rect &dstRect)
{
    if (src == NULL || src->pixels == NULL || dstPixels == NULL || dstTexW <= 0 || dstTexH <= 0)
        return;

    const i32 copyW = std::max(0, std::min(dstRect.w, dstTexW - dstRect.x));
    const i32 copyH = std::max(0, std::min(dstRect.h, dstTexH - dstRect.y));
    if (copyW <= 0 || copyH <= 0 || srcRect.w <= 0 || srcRect.h <= 0)
        return;

    SDL_LockSurface(src);
    for (i32 y = 0; y < copyH; y++)
    {
        i32 sy0 = srcRect.y + (i32)((long long)y * srcRect.h / copyH);
        i32 sy1 = srcRect.y + (i32)(((long long)(y + 1) * srcRect.h + copyH - 1) / copyH);
        sy0 = std::max(0, std::min(src->h - 1, sy0));
        sy1 = std::max(sy0 + 1, std::min(src->h, sy1));

        for (i32 x = 0; x < copyW; x++)
        {
            i32 sx0 = srcRect.x + (i32)((long long)x * srcRect.w / copyW);
            i32 sx1 = srcRect.x + (i32)(((long long)(x + 1) * srcRect.w + copyW - 1) / copyW);
            sx0 = std::max(0, std::min(src->w - 1, sx0));
            sx1 = std::max(sx0 + 1, std::min(src->w, sx1));

            u32 sumR = 0;
            u32 sumG = 0;
            u32 sumB = 0;
            u32 sumA = 0;
            u32 count = 0;
            for (i32 sy = sy0; sy < sy1; sy++)
            {
                const u8 *srcRow = (const u8 *)src->pixels + sy * src->pitch;
                for (i32 sx = sx0; sx < sx1; sx++)
                {
                    const u8 *p = srcRow + sx * 4;
                    sumR += p[0];
                    sumG += p[1];
                    sumB += p[2];
                    sumA += p[3];
                    count++;
                }
            }

            u8 *d = dstPixels + ((dstRect.y + y) * dstTexW + (dstRect.x + x)) * 4;
            if (count != 0)
            {
                d[0] = (u8)(sumR / count);
                d[1] = (u8)(sumG / count);
                d[2] = (u8)(sumB / count);
                d[3] = (u8)(sumA / count);
            }
            else
            {
                d[0] = d[1] = d[2] = d[3] = 0;
            }
        }
    }
    SDL_UnlockSurface(src);
}

void TextHelper::RenderTextToTexture(i32 xPos, i32 yPos, i32 spriteWidth, i32 spriteHeight, i32 fontHeight,
                                     i32 fontWidth, ZunColor textColor, ZunColor shadowColor, const char *string,
                                     TextureData *outTexture)
{
#ifdef TH06_PS3
    const u64 perfTotalStartUs = TextPerfNowUs();
#endif
    char convertedText[1024];
    SDL_Rect finalCopyDst;
    SDL_Rect finalCopySrc;
    SDL_Rect shadowRect;

    if (!isUTF8Encoded(string))
    {
        char *utf8 = sjis2utf8(string);
        strcpy(convertedText, utf8);
        free(utf8);
    }
    else
    {
        strcpy(convertedText, string);
    }

    TTF_SetFontSize(g_Font, fontHeight * 2);

    finalCopySrc.x = 0;
    finalCopySrc.y = 0;
    // SDL_ttf/Noto CJK can render a 15px game font to a taller high-res glyph
    // than the old Windows GDI/MS Gothic path.  Keeping the historical
    // fontHeight*2-2 source rect clips the lower strokes before downsampling,
    // which shows up in-game as the bottom quarter of dialogue/spell text
    // missing.  Give the high-res source enough vertical room, then scale it
    // into the sprite's actual destination height below.
    finalCopySrc.w = std::max(1, std::min(GAME_WINDOW_WIDTH, spriteWidth * 2));
    finalCopySrc.h = std::max(1, std::min(TEXT_BUFFER_HEIGHT, fontHeight * 2 + 8));

    SDL_FillRect(g_TextBufferSurface, &finalCopySrc, 0);

#ifdef TH06_PS3
    const u64 perfRasterStartUs = TextPerfNowUs();
#endif
    if (shadowColor != COLOR_WHITE)
    {
        SDL_Surface *shadowText;

        // Render shadow.
        SDL_Color sdlShadowColor;
        sdlShadowColor.a = 0xFF;
        sdlShadowColor.r = (shadowColor >> 16) & 0xFF;
        sdlShadowColor.g = (shadowColor >> 8) & 0xFF;
        sdlShadowColor.b = shadowColor & 0xFF;

        shadowText = TTF_RenderUTF8_Blended(g_Font, convertedText, sdlShadowColor);

        if (shadowText != NULL)
        {
            shadowRect.x = xPos * 2 + 3;
            shadowRect.y = 2;
            shadowRect.w = shadowText->w;
            shadowRect.h = shadowText->h;

            SDL_SetSurfaceBlendMode(shadowText, SDL_BLENDMODE_NONE);
            SDL_BlitSurface(shadowText, NULL, g_TextBufferSurface, &shadowRect);

            SDL_FreeSurface(shadowText);
        }
    }

    SDL_Color sdlTextColor;
    sdlTextColor.a = 0xFF;
    sdlTextColor.r = (textColor >> 16) & 0xFF;
    sdlTextColor.g = (textColor >> 8) & 0xFF;
    sdlTextColor.b = textColor & 0xFF;

    SDL_Surface *regularText = TTF_RenderUTF8_Blended(g_Font, convertedText, sdlTextColor);

    if (regularText != NULL)
    {
        SurfaceOverwriteBlend(regularText, g_TextBufferSurface, xPos * 2);

        SDL_FreeSurface(regularText);
    }

#ifdef TH06_PS3
    g_TextPerfRasterUs = (u32)(TextPerfNowUs() - perfRasterStartUs);
    const u64 perfComposeStartUs = TextPerfNowUs();
#endif
    // Once we get an API abstraction layer for surface operations, this needs to change
    //   We really shouldn't be clobbering the texture format
    if (!outTexture->textureData || outTexture->format != TEX_FMT_A8R8G8B8)
    {
        free(outTexture->textureData);
        outTexture->textureData = (u8 *)malloc(outTexture->width * outTexture->height * 4);
        memset(outTexture->textureData, 0, outTexture->width * outTexture->height * 4);
    }

    outTexture->format = TEX_FMT_A8R8G8B8;
    InvertAlpha(0, 0, spriteWidth * 2, fontHeight * 2 + 6);

    finalCopyDst.x = 0;
    finalCopyDst.y = yPos;
    finalCopyDst.w = spriteWidth;
    // spriteHeight is the height of the complete ANM texture atlas here, not
    // the height of the text sprite.  Scaling into it makes the glyph span the
    // rest of the atlas, leaving the 16px text sprite sampling an empty strip.
    finalCopyDst.h = std::max(1, std::min(16, (i32)outTexture->height - yPos));

    CopyTextBufferToTexture(g_TextBufferSurface, finalCopySrc, outTexture->textureData, outTexture->width,
                            outTexture->height, finalCopyDst);

#ifdef TH06_PS3
    g_TextPerfComposeUs = (u32)(TextPerfNowUs() - perfComposeStartUs);
    const u64 perfUploadStartUs = TextPerfNowUs();
#endif
    QueueTextTextureUpload(outTexture);
#ifdef TH06_PS3
    g_TextPerfUploadUs = (u32)(TextPerfNowUs() - perfUploadStartUs);
    g_TextPerfTotalUs = (u32)(TextPerfNowUs() - perfTotalStartUs);
    g_TextPerfSerial++;
#endif

    return;
}

// Extended to free all globals for text helper
void TextHelper::ReleaseTextBuffer()
{
    if (g_Font != NULL)
    {
        TTF_CloseFont(g_Font);
        g_Font = NULL;
    }

    if (g_TextBufferSurface != NULL)
    {
        SDL_FreeSurface(g_TextBufferSurface);
        g_TextBufferSurface = NULL;
    }

    return;
}
