// th06_ps3 SDL shim implementation (Phase 1)
// Functional: RWops(stdio) / timer / surface alloc / error string
// Stub: video/renderer/audio/input/image/ttf — replaced by PS3 backends in later phases
#include <SDL2/SDL_shim.h>
#include <algorithm>
#if defined(TH06_PSP) || defined(__PSL1GHT__)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif
#if defined(TH06_PS3) && defined(__PSL1GHT__)
extern "C" char th06_basedir[512]; // ps3/sentry_guest.cpp
#endif

#ifdef TH06_PSP
#include <pspkernel.h>
#include "fileio.hpp"
extern "C" int th06_psp_video_init(void);
extern "C" void th06_psp_video_present(const unsigned int *src, int w, int h, int srcPitchBytes);
extern "C" void th06_psp_input_poll(unsigned char *keystate, unsigned int count);
extern "C" int th06_psp_audio_init(void);
extern "C" void th06_psp_audio_queue(const short *pcm, unsigned int bytes);
extern "C" unsigned int th06_psp_audio_queued_bytes(void);
extern "C" void th06_psp_audio_shutdown(void);
extern "C" void th06_psp_boot_note(const char *message);
extern "C" int th06_psp_boot_phase(void);
static const void *g_LastTexPixels;
static int g_LastTexPitch;
// GameWindow's own backend breadcrumbs are inside PS3 guards, so the shim is
// the only place the PSP can see the window/renderer stages from.  Boot scoped:
// silent from the first presented frame onwards.
static void ShimBootNote(const char *message)
{
    if (th06_psp_boot_phase())
        th06_psp_boot_note(message);
}
#elif defined(__PSL1GHT__)
#include <sys/systime.h>
#if defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED
extern "C" void th06_arcade_present(const unsigned int *src, int w, int h, int srcPitchBytes);
#else
extern "C" int th06_video_init(void);
extern "C" void th06_video_present(const unsigned int *src, int w, int h, int srcPitchBytes);
#endif
static const void *g_LastTexPixels;
static int g_LastTexPitch;
#endif

static char g_ErrorBuf[256] = "no error";
const char *SDL_GetError(void)
{
    return g_ErrorBuf;
}
static void ShimSetError(const char *msg)
{
    snprintf(g_ErrorBuf, sizeof(g_ErrorBuf), "%s", msg);
}

void SDL_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
}

// ---------------------------------------------------------------- init
int SDL_Init(Uint32)
{
#ifdef TH06_PSP
    ShimBootNote("SDL INIT");
#endif
    return 0;
}
int SDL_InitSubSystem(Uint32)
{
    return 0;
}
void SDL_Quit(void)
{
}
SDL_bool SDL_SetHint(const char *, const char *)
{
    return SDL_TRUE;
}

// ---------------------------------------------------------------- timer
Uint64 SDL_GetTicks64(void)
{
#ifdef TH06_PSP
    return sceKernelGetSystemTimeWide() / 1000ull;
#elif defined(__PSL1GHT__)
    u64 sec, nsec;
    sysGetCurrentTime(&sec, &nsec);
    return sec * 1000ull + nsec / 1000000ull;
#else
    return 0;
#endif
}
Uint32 SDL_GetTicks(void)
{
    return (Uint32)SDL_GetTicks64();
}
void SDL_Delay(Uint32 ms)
{
#if defined(TH06_PSP)
    sceKernelDelayThread(ms * 1000);
#elif defined(__PSL1GHT__)
    sysUsleep(ms * 1000);
#else
    (void)ms;
#endif
}
SDL_TimerID SDL_AddTimer(Uint32, SDL_TimerCallback, void *)
{
    return 1; // pumped manually by PS3 main loop in later phase
}
SDL_bool SDL_RemoveTimer(SDL_TimerID)
{
    return SDL_TRUE;
}

// ---------------------------------------------------------------- rwops
SDL_RWops *SDL_RWFromFile(const char *file, const char *mode)
{
#if defined(TH06_PS3) && defined(TH06_PS3_BASEDIR)
    // PS3 lv2 has no CWD - resolve relative paths like FileSystem::FopenUTF8 does
    char resolved[768];
    if (file[0] != '/')
    {
        snprintf(resolved, sizeof(resolved), "%s/%s", th06_basedir, file);
        file = resolved;
    }
#endif
    FILE *fp = std::fopen(file, mode);
    if (!fp)
    {
        ShimSetError("RWFromFile: fopen failed");
        return nullptr;
    }
    SDL_RWops *rw = (SDL_RWops *)std::calloc(1, sizeof(SDL_RWops));
    rw->fp = fp;
    return rw;
}
SDL_RWops *SDL_RWFromConstMem(const void *mem, int size)
{
    SDL_RWops *rw = (SDL_RWops *)std::calloc(1, sizeof(SDL_RWops));
    rw->mem = (const Uint8 *)mem;
    rw->memSize = (size_t)size;
    return rw;
}
size_t SDL_RWread(SDL_RWops *ctx, void *ptr, size_t size, size_t num)
{
    if (ctx->fp)
        return std::fread(ptr, size, num, ctx->fp);
    if (size == 0)
        return 0;
    size_t avail = (ctx->memSize - ctx->memPos) / size;
    size_t n = avail < num ? avail : num;
    std::memcpy(ptr, ctx->mem + ctx->memPos, n * size);
    ctx->memPos += n * size;
    return n;
}
Sint64 SDL_RWseek(SDL_RWops *ctx, Sint64 offset, int whence)
{
    if (ctx->fp)
    {
        if (std::fseek(ctx->fp, (long)offset, whence) != 0)
            return -1;
        return std::ftell(ctx->fp);
    }
    size_t base = whence == RW_SEEK_SET ? 0 : whence == RW_SEEK_CUR ? ctx->memPos : ctx->memSize;
    ctx->memPos = base + (size_t)offset;
    if (ctx->memPos > ctx->memSize)
        ctx->memPos = ctx->memSize;
    return (Sint64)ctx->memPos;
}
Sint64 SDL_RWtell(SDL_RWops *ctx)
{
    return ctx->fp ? std::ftell(ctx->fp) : (Sint64)ctx->memPos;
}
Sint64 SDL_RWsize(SDL_RWops *ctx)
{
    if (!ctx->fp)
        return (Sint64)ctx->memSize;
    long cur = std::ftell(ctx->fp);
    std::fseek(ctx->fp, 0, SEEK_END);
    long size = std::ftell(ctx->fp);
    std::fseek(ctx->fp, cur, SEEK_SET);
    return size;
}
int SDL_RWclose(SDL_RWops *ctx)
{
    int r = 0;
    if (ctx->fp)
        r = std::fclose(ctx->fp);
    std::free(ctx);
    return r;
}
Uint16 SDL_ReadLE16(SDL_RWops *ctx)
{
    Uint8 b[2] = {0, 0};
    SDL_RWread(ctx, b, 1, 2);
    return (Uint16)(b[0] | (b[1] << 8));
}
Uint32 SDL_ReadLE32(SDL_RWops *ctx)
{
    Uint8 b[4] = {0, 0, 0, 0};
    SDL_RWread(ctx, b, 1, 4);
    return (Uint32)b[0] | ((Uint32)b[1] << 8) | ((Uint32)b[2] << 16) | ((Uint32)b[3] << 24);
}

// ---------------------------------------------------------------- pixels/surfaces
int SDL_BITSPERPIXEL(Uint32 fmt)
{
    switch (fmt)
    {
    case SDL_PIXELFORMAT_RGB24:
        return 24;
    case SDL_PIXELFORMAT_RGB565:
    case SDL_PIXELFORMAT_RGBA4444:
    case SDL_PIXELFORMAT_RGBA5551:
        return 16;
    default:
        return 32;
    }
}
int SDL_BYTESPERPIXEL(Uint32 fmt)
{
    return SDL_BITSPERPIXEL(fmt) / 8;
}

#ifdef TH06_PSP
static SDL_Surface *g_PspSurfaces[128];
static Uint32 g_PspNextSurfaceSerial;
static Uint32 g_PspLastLockedSurfaceSerial;
extern "C" unsigned int th06_psp_ge_trim_surface_cache();

static void PspRegisterSurface(SDL_Surface *surface)
{
    surface->pspSerial = ++g_PspNextSurfaceSerial;
    if (surface->pspSerial == 0)
        surface->pspSerial = ++g_PspNextSurfaceSerial;
    for (unsigned int i = 0; i < sizeof(g_PspSurfaces) / sizeof(g_PspSurfaces[0]); ++i)
    {
        if (!g_PspSurfaces[i])
        {
            g_PspSurfaces[i] = surface;
            return;
        }
    }
}

static void PspUnregisterSurface(SDL_Surface *surface)
{
    for (unsigned int i = 0; i < sizeof(g_PspSurfaces) / sizeof(g_PspSurfaces[0]); ++i)
    {
        if (g_PspSurfaces[i] == surface)
        {
            g_PspSurfaces[i] = nullptr;
            return;
        }
    }
}

extern "C" Uint32 th06_psp_last_locked_surface_serial()
{
    return g_PspLastLockedSurfaceSerial;
}

extern "C" int th06_psp_surface_serial_alive(Uint32 serial)
{
    if (serial == 0)
        return 0;
    for (SDL_Surface *surface : g_PspSurfaces)
        if (surface && surface->pspSerial == serial)
            return 1;
    return 0;
}

extern "C" void th06_psp_mark_surface_ge_cached(Uint32 serial, int cached)
{
    for (unsigned int i = 0; i < sizeof(g_PspSurfaces) / sizeof(g_PspSurfaces[0]); ++i)
    {
        SDL_Surface *surface = g_PspSurfaces[i];
        if (surface && surface->pspSerial == serial)
        {
            surface->pspGeCached = cached != 0;
            return;
        }
    }
}
#endif

SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32, int width, int height, int, Uint32 format)
{
    if (width < 0 || height < 0)
        return nullptr;
    SDL_Surface *s = (SDL_Surface *)std::calloc(1, sizeof(SDL_Surface));
    if (!s)
        return nullptr;
    s->format = (SDL_PixelFormat *)std::calloc(1, sizeof(SDL_PixelFormat));
    if (!s->format)
    {
        std::free(s);
        return nullptr;
    }
    s->format->format = format;
    s->format->BitsPerPixel = (Uint8)SDL_BITSPERPIXEL(format);
    s->format->BytesPerPixel = (Uint8)SDL_BYTESPERPIXEL(format);
    s->w = width;
    s->h = height;
    s->pitch = width * s->format->BytesPerPixel;
    const size_t pixelBytes = (size_t)s->pitch * (size_t)height;
    if (pixelBytes != 0)
    {
        s->pixels = std::calloc(1, pixelBytes);
        if (!s->pixels)
        {
            std::free(s->format);
            std::free(s);
            return nullptr;
        }
    }
    s->refcount = 1;
#ifdef TH06_PSP
    PspRegisterSurface(s);
#endif
    return s;
}
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int width, int height, int depth, int pitch, Uint32 format)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 0, 0, depth, format);
    if (!s)
        return nullptr;
    std::free(s->pixels);
    s->pixels = pixels;
    s->w = width;
    s->h = height;
    s->pitch = pitch;
    s->flags = 1; // borrowed pixels
    return s;
}
void SDL_FreeSurface(SDL_Surface *surface)
{
    if (!surface)
        return;
#ifdef TH06_PSP
    // Remove the lifetime pin before trimming a cache-only JPEG. A live GU
    // handle still protects borrowed pixels; Trim also completes queued GE
    // reads before freeing an unborrowed entry.
    PspUnregisterSurface(surface);
    if (surface->pspGeCached)
        th06_psp_ge_trim_surface_cache();
#endif
    if (!(surface->flags & 1))
        std::free(surface->pixels);
    std::free(surface->format);
    std::free(surface);
}
int SDL_LockSurface(SDL_Surface *surface)
{
#ifdef TH06_PSP
    if (surface)
    {
        g_PspLastLockedSurfaceSerial = surface->pspSerial;
        surface->pspSavedHeight = 0;
        if (surface->pspGeCached && surface->h > 0)
        {
            // AnmManager::ExtractSurfacePixels computes and loops over h while
            // the surface is locked.  A cached immutable JPEG needs no second
            // 900 KiB copy; Unlock restores h before the backend receives the
            // content rectangle.  The backend hit path never dereferences the
            // resulting zero-length temporary allocation.
            surface->pspSavedHeight = surface->h;
            surface->h = 0;
        }
    }
#endif
    return 0;
}
void SDL_UnlockSurface(SDL_Surface *surface)
{
#ifdef TH06_PSP
    if (surface && surface->pspSavedHeight > 0)
    {
        surface->h = surface->pspSavedHeight;
        surface->pspSavedHeight = 0;
    }
#endif
}
static bool SDL_ReadPixelRGBA(const SDL_Surface *s, int x, int y, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a)
{
    if (!s || !s->format || !s->pixels || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return false;

    const Uint8 *p = (const Uint8 *)s->pixels + y * s->pitch + x * s->format->BytesPerPixel;
    switch (s->format->format)
    {
    case SDL_PIXELFORMAT_RGBA32:
        *r = p[0];
        *g = p[1];
        *b = p[2];
        *a = p[3];
        return true;
    case SDL_PIXELFORMAT_ARGB8888:
        *a = p[0];
        *r = p[1];
        *g = p[2];
        *b = p[3];
        return true;
    case SDL_PIXELFORMAT_RGB24:
        *r = p[0];
        *g = p[1];
        *b = p[2];
        *a = 0xFF;
        return true;
    default:
        return false;
    }
}

static bool SDL_WritePixelRGBA(SDL_Surface *s, int x, int y, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (!s || !s->format || !s->pixels || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return false;

    Uint8 *p = (Uint8 *)s->pixels + y * s->pitch + x * s->format->BytesPerPixel;
    switch (s->format->format)
    {
    case SDL_PIXELFORMAT_RGBA32:
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = a;
        return true;
    case SDL_PIXELFORMAT_ARGB8888:
        p[0] = a;
        p[1] = r;
        p[2] = g;
        p[3] = b;
        return true;
    case SDL_PIXELFORMAT_RGB24:
        p[0] = r;
        p[1] = g;
        p[2] = b;
        return true;
    default:
        return false;
    }
}

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color)
{
    if (!dst || !dst->format || !dst->pixels)
        return -1;

    SDL_Rect r = rect ? *rect : (SDL_Rect){0, 0, dst->w, dst->h};
    if (r.x < 0)
    {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < 0)
    {
        r.h += r.y;
        r.y = 0;
    }
    if (r.x + r.w > dst->w)
        r.w = dst->w - r.x;
    if (r.y + r.h > dst->h)
        r.h = dst->h - r.y;
    if (r.w <= 0 || r.h <= 0)
        return 0;

    if (color == 0)
    {
        const int bytes = dst->format->BytesPerPixel;
        for (int y = 0; y < r.h; y++)
            std::memset((Uint8 *)dst->pixels + (r.y + y) * dst->pitch + r.x * bytes, 0, (size_t)r.w * bytes);
        return 0;
    }

    Uint8 cr = (color >> 16) & 0xFF;
    Uint8 cg = (color >> 8) & 0xFF;
    Uint8 cb = color & 0xFF;
    Uint8 ca = (color >> 24) & 0xFF;
    if (dst->format->format == SDL_PIXELFORMAT_RGBA32)
    {
        cr = (color >> 24) & 0xFF;
        cg = (color >> 16) & 0xFF;
        cb = (color >> 8) & 0xFF;
        ca = color & 0xFF;
    }

    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++)
            SDL_WritePixelRGBA(dst, r.x + x, r.y + y, cr, cg, cb, ca);
    return 0;
}
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    if (!src || !dst || !src->format || !dst->format || !src->pixels || !dst->pixels)
        return -1;

    SDL_Rect sr = srcrect ? *srcrect : (SDL_Rect){0, 0, src->w, src->h};
    SDL_Rect dr = dstrect ? *dstrect : (SDL_Rect){0, 0, sr.w, sr.h};
    int copyW = sr.w;
    int copyH = sr.h;

    if (sr.x < 0)
    {
        dr.x -= sr.x;
        copyW += sr.x;
        sr.x = 0;
    }
    if (sr.y < 0)
    {
        dr.y -= sr.y;
        copyH += sr.y;
        sr.y = 0;
    }
    if (dr.x < 0)
    {
        sr.x -= dr.x;
        copyW += dr.x;
        dr.x = 0;
    }
    if (dr.y < 0)
    {
        sr.y -= dr.y;
        copyH += dr.y;
        dr.y = 0;
    }
    copyW = std::min(copyW, src->w - sr.x);
    copyH = std::min(copyH, src->h - sr.y);
    copyW = std::min(copyW, dst->w - dr.x);
    copyH = std::min(copyH, dst->h - dr.y);
    if (copyW <= 0 || copyH <= 0)
        return 0;

    for (int y = 0; y < copyH; y++)
    {
        for (int x = 0; x < copyW; x++)
        {
            Uint8 r, g, b, a;
            if (SDL_ReadPixelRGBA(src, sr.x + x, sr.y + y, &r, &g, &b, &a))
                SDL_WritePixelRGBA(dst, dr.x + x, dr.y + y, r, g, b, a);
        }
    }
    return 0;
}
int SDL_SoftStretchLinear(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect)
{
    if (!src || !dst || !src->format || !dst->format || !src->pixels || !dst->pixels)
        return -1;

    SDL_Rect sr = srcrect ? *srcrect : (SDL_Rect){0, 0, src->w, src->h};
    SDL_Rect dr = dstrect ? *dstrect : (SDL_Rect){0, 0, dst->w, dst->h};
    if (sr.w <= 0 || sr.h <= 0 || dr.w <= 0 || dr.h <= 0)
        return 0;

    for (int y = 0; y < dr.h; y++)
    {
        const int dy = dr.y + y;
        if (dy < 0 || dy >= dst->h)
            continue;

        int sy0 = sr.y + (int)((Sint64)y * sr.h / dr.h);
        int sy1 = sr.y + (int)(((Sint64)(y + 1) * sr.h + dr.h - 1) / dr.h);
        sy0 = std::max(0, std::min(src->h - 1, sy0));
        sy1 = std::max(sy0 + 1, std::min(src->h, sy1));

        for (int x = 0; x < dr.w; x++)
        {
            const int dx = dr.x + x;
            if (dx < 0 || dx >= dst->w)
                continue;

            int sx0 = sr.x + (int)((Sint64)x * sr.w / dr.w);
            int sx1 = sr.x + (int)(((Sint64)(x + 1) * sr.w + dr.w - 1) / dr.w);
            sx0 = std::max(0, std::min(src->w - 1, sx0));
            sx1 = std::max(sx0 + 1, std::min(src->w, sx1));

            unsigned int sumR = 0, sumG = 0, sumB = 0, sumA = 0, count = 0;
            for (int sy = sy0; sy < sy1; sy++)
            {
                for (int sx = sx0; sx < sx1; sx++)
                {
                    Uint8 r, g, b, a;
                    if (SDL_ReadPixelRGBA(src, sx, sy, &r, &g, &b, &a))
                    {
                        sumR += r;
                        sumG += g;
                        sumB += b;
                        sumA += a;
                        count++;
                    }
                }
            }
            if (count != 0)
            {
                SDL_WritePixelRGBA(dst, dx, dy, (Uint8)(sumR / count), (Uint8)(sumG / count),
                                   (Uint8)(sumB / count), (Uint8)(sumA / count));
            }
        }
    }
    return 0;
}
int SDL_SetSurfaceBlendMode(SDL_Surface *, SDL_BlendMode)
{
    return 0;
}

// ---------------------------------------------------------------- video/renderer stubs
SDL_Window *SDL_CreateWindow(const char *, int, int, int, int, Uint32)
{
#if defined(TH06_PSP)
    ShimBootNote("SDL CREATEWINDOW");
    th06_psp_video_init();
#elif defined(__PSL1GHT__) && !(defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED)
    th06_video_init();
#endif
    static int dummy;
    return (SDL_Window *)&dummy;
}
void SDL_DestroyWindow(SDL_Window *)
{
}
void SDL_GetWindowSize(SDL_Window *, int *w, int *h)
{
    if (w)
#ifdef TH06_PSP
        *w = 480;
#else
        *w = 640;
#endif
    if (h)
#ifdef TH06_PSP
        *h = 272;
#else
        *h = 480;
#endif
}
SDL_Renderer *SDL_CreateRenderer(SDL_Window *, int, Uint32)
{
#ifdef TH06_PSP
    ShimBootNote("SDL CREATERENDERER");
#endif
    static int dummy;
    return (SDL_Renderer *)&dummy;
}
void SDL_DestroyRenderer(SDL_Renderer *)
{
}
SDL_Texture *SDL_CreateTexture(SDL_Renderer *, Uint32, int, int, int)
{
#ifdef TH06_PSP
    ShimBootNote("SDL CREATETEXTURE");
#endif
    static int dummy;
    return (SDL_Texture *)&dummy;
}
SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *, SDL_Surface *)
{
    static int dummy;
    return (SDL_Texture *)&dummy;
}
void SDL_DestroyTexture(SDL_Texture *)
{
}
int SDL_UpdateTexture(SDL_Texture *, const SDL_Rect *, const void *pixels, int pitch)
{
#if defined(TH06_PSP) || defined(__PSL1GHT__)
    g_LastTexPixels = pixels;
    g_LastTexPitch = pitch;
#endif
    return 0;
}
int SDL_RenderClear(SDL_Renderer *)
{
    return 0;
}
int SDL_RenderCopy(SDL_Renderer *, SDL_Texture *, const SDL_Rect *, const SDL_Rect *)
{
    return 0;
}
int SDL_RenderFillRect(SDL_Renderer *, const SDL_Rect *)
{
    return 0;
}
int SDL_RenderDrawRect(SDL_Renderer *, const SDL_Rect *)
{
    return 0;
}
int SDL_RenderDrawLine(SDL_Renderer *, int, int, int, int)
{
    return 0;
}
int SDL_RenderDrawPoint(SDL_Renderer *, int, int)
{
    return 0;
}
int SDL_SetRenderDrawColor(SDL_Renderer *, Uint8, Uint8, Uint8, Uint8)
{
    return 0;
}
void SDL_RenderPresent(SDL_Renderer *)
{
#if defined(TH06_PSP)
    if (g_LastTexPixels)
        th06_psp_video_present((const unsigned int *)g_LastTexPixels, 480, 272, g_LastTexPitch);
#elif defined(__PSL1GHT__)
    if (g_LastTexPixels)
#if defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED
        th06_arcade_present((const unsigned int *)g_LastTexPixels,
                            GAME_WINDOW_WIDTH_REAL, GAME_WINDOW_HEIGHT_REAL,
                            g_LastTexPitch);
#else
        th06_video_present((const unsigned int *)g_LastTexPixels, 640, 480, g_LastTexPitch);
#endif
#endif
}

int SDL_GL_SetAttribute(SDL_GLattr, int)
{
    return -1;
}
SDL_GLContext SDL_GL_CreateContext(SDL_Window *)
{
    ShimSetError("no GL on PS3 shim");
    return nullptr;
}
void SDL_GL_DeleteContext(SDL_GLContext)
{
}
int SDL_GL_MakeCurrent(SDL_Window *, SDL_GLContext)
{
    return -1;
}
SDL_bool SDL_GL_ExtensionSupported(const char *)
{
    return SDL_FALSE;
}
void *SDL_GL_GetProcAddress(const char *)
{
    return nullptr;
}
int SDL_GL_SetSwapInterval(int)
{
    return -1;
}
void SDL_GL_SwapWindow(SDL_Window *)
{
}

// ---------------------------------------------------------------- input stubs
int SDL_ShowCursor(int)
{
    return 0;
}
#if defined(TH06_PSP) || defined(__PSL1GHT__)
extern "C" void th06_pad_poll(unsigned char *keystate);
extern "C" void th06_kb_poll(unsigned char *keystate);
extern "C" int th06_pad_game_input_blocked(void);
extern "C" int th06_video_xmb_blocks_game(void);
#endif
const Uint8 *SDL_GetKeyboardState(int *numkeys)
{
    static Uint8 state[SDL_NUM_SCANCODES];
    if (numkeys)
        *numkeys = SDL_NUM_SCANCODES;
#if defined(TH06_PSP)
    th06_psp_input_poll(state, SDL_NUM_SCANCODES);
#elif defined(__PSL1GHT__)
    // Pad keeps its own last-known state in padstate (pad-001: len=0 keeps
    // previous sample). Compose pad|keyboard fresh each poll so a keyboard
    // release is never masked by a stale merged array.
    static Uint8 padstate[SDL_NUM_SCANCODES];
    th06_pad_poll(padstate);
    std::memcpy(state, padstate, sizeof(state));
#if !defined(TH06_NO_KB)
    th06_kb_poll(state);
#endif
    // The same pad drives the in-game XMB. Keep the game alive behind the
    // translucent overlay without allowing XMB navigation to move, shoot or
    // confirm inside TH06. The close quarantine is intentionally blocked too.
    if (th06_video_xmb_blocks_game() || th06_pad_game_input_blocked())
        std::memset(state, 0, sizeof(state));
#endif
    return state;
}
void SDL_StartTextInput(void)
{
}
void SDL_StopTextInput(void)
{
}
int SDL_NumJoysticks(void)
{
    return 0; // scePad wired here in Phase 1-4
}
SDL_bool SDL_IsGameController(int)
{
    return SDL_FALSE;
}
SDL_GameController *SDL_GameControllerOpen(int)
{
    return nullptr;
}
void SDL_GameControllerClose(SDL_GameController *)
{
}
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *)
{
    return nullptr;
}
Uint8 SDL_GameControllerGetButton(SDL_GameController *, SDL_GameControllerButton)
{
    return 0;
}
Sint16 SDL_GameControllerGetAxis(SDL_GameController *, SDL_GameControllerAxis)
{
    return 0;
}
SDL_bool SDL_GameControllerHasAxis(SDL_GameController *, SDL_GameControllerAxis)
{
    return SDL_FALSE;
}

int SDL_PollEvent(SDL_Event *)
{
    return 0;
}
int SDL_ShowSimpleMessageBox(Uint32, const char *title, const char *message, SDL_Window *)
{
    std::printf("[MSGBOX] %s: %s\n", title, message);
    return 0;
}

// ---------------------------------------------------------------- audio stubs (cellAudio in later phase)
#ifdef __PSL1GHT__
extern "C" int th06_audio_init(void);
extern "C" void th06_audio_queue(const short *pcm, unsigned int bytes);
extern "C" void th06_audio_pump(void);
extern "C" unsigned int th06_audio_backlog_bytes(void);
#endif
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *, int, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int)
{
    if (obtained && desired)
        *obtained = *desired;
#ifdef TH06_PSP
    if (th06_psp_audio_init() != 0)
        return 0;
#elif defined(__PSL1GHT__)
    if (th06_audio_init() != 0)
        return 0;
#endif
    return 1;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID)
{
#ifdef TH06_PSP
    th06_psp_audio_shutdown();
#endif
}
void SDL_PauseAudioDevice(SDL_AudioDeviceID, int)
{
}
int SDL_QueueAudio(SDL_AudioDeviceID, const void *data, Uint32 len)
{
#ifdef TH06_PSP
    th06_psp_audio_queue((const short *)data, len);
#elif defined(__PSL1GHT__)
    th06_audio_queue((const short *)data, len);
    th06_audio_pump();
#endif
    return 0;
}
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID)
{
#ifdef TH06_PSP
    return th06_psp_audio_queued_bytes();
#elif defined(__PSL1GHT__)
    th06_audio_pump();
    return th06_audio_backlog_bytes();
#else
    return 0;
#endif
}
// Real WAV parser: RIFF/fmt/data, PCM only. Output is ALWAYS native s16 (u8 expanded,
// LE16 swapped), spec.format = AUDIO_S16SYS, channels/freq preserved.
SDL_AudioSpec *SDL_LoadWAV_RW(SDL_RWops *src, int freesrc, SDL_AudioSpec *spec, Uint8 **audio_buf, Uint32 *audio_len)
{
    SDL_AudioSpec *result = nullptr;
    Uint8 *out = nullptr;
    Uint16 channels = 0, bits = 0;
    Uint32 freq = 0;
    bool haveFmt = false;

    if (audio_buf)
        *audio_buf = nullptr;
    if (audio_len)
        *audio_len = 0;

    do
    {
        Uint8 hdr[12];
        if (SDL_RWread(src, hdr, 1, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        {
            ShimSetError("LoadWAV: not a RIFF/WAVE");
            break;
        }

        for (;;)
        {
            Uint8 chunkHdr[8];
            if (SDL_RWread(src, chunkHdr, 1, 8) != 8)
                break;
            Uint32 chunkLen = (Uint32)chunkHdr[4] | ((Uint32)chunkHdr[5] << 8) | ((Uint32)chunkHdr[6] << 16) |
                              ((Uint32)chunkHdr[7] << 24);

            if (memcmp(chunkHdr, "fmt ", 4) == 0)
            {
                Uint8 fmt[16];
                Uint32 toRead = chunkLen < 16 ? chunkLen : 16;
                if (SDL_RWread(src, fmt, 1, toRead) != toRead)
                    break;
                Uint16 audioFormat = (Uint16)(fmt[0] | (fmt[1] << 8));
                channels = (Uint16)(fmt[2] | (fmt[3] << 8));
                freq = (Uint32)fmt[4] | ((Uint32)fmt[5] << 8) | ((Uint32)fmt[6] << 16) | ((Uint32)fmt[7] << 24);
                bits = (Uint16)(fmt[14] | (fmt[15] << 8));
                if (audioFormat != 1)
                {
                    ShimSetError("LoadWAV: not PCM");
                    break;
                }
                haveFmt = true;
                if (chunkLen > toRead)
                    SDL_RWseek(src, chunkLen - toRead, RW_SEEK_CUR);
            }
            else if (memcmp(chunkHdr, "data", 4) == 0 && haveFmt)
            {
                Uint32 sampleCount = (bits == 8) ? chunkLen : chunkLen / 2;
                out = (Uint8 *)malloc(sampleCount * 2);
                Sint16 *o16 = (Sint16 *)out;
                if (bits == 8)
                {
                    Uint8 *tmp = (Uint8 *)malloc(chunkLen);
                    SDL_RWread(src, tmp, 1, chunkLen);
                    for (Uint32 i = 0; i < chunkLen; i++)
                        o16[i] = (Sint16)(((int)tmp[i] - 128) << 8);
                    free(tmp);
                }
                else
                {
                    SDL_RWread(src, out, 1, chunkLen);
                    for (Uint32 i = 0; i < sampleCount; i++)
                    {
                        Uint16 v = (Uint16)(out[i * 2] | (out[i * 2 + 1] << 8)); // LE on disk
                        o16[i] = (Sint16)v;
                    }
                }
                if (spec)
                {
                    memset(spec, 0, sizeof(*spec));
                    spec->freq = (int)freq;
                    spec->format = AUDIO_S16SYS;
                    spec->channels = (Uint8)channels;
                }
                if (audio_buf)
                    *audio_buf = out;
                if (audio_len)
                    *audio_len = sampleCount * 2;
                result = spec;
                break;
            }
            else
            {
                SDL_RWseek(src, (chunkLen + 1) & ~1u, RW_SEEK_CUR);
            }
        }
    } while (0);

    if (!result && out)
        free(out);
    if (freesrc && src)
        SDL_RWclose(src);
    return result;
}
void SDL_FreeWAV(Uint8 *audio_buf)
{
    std::free(audio_buf);
}
int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat src_format, Uint8 src_channels, int src_rate,
                      SDL_AudioFormat dst_format, Uint8 dst_channels, int dst_rate)
{
    if (!cvt || src_channels == 0 || dst_channels == 0 || src_rate <= 0 || dst_rate <= 0)
        return -1;

    std::memset(cvt, 0, sizeof(*cvt));
    cvt->src_format = src_format;
    cvt->dst_format = dst_format;
    cvt->src_channels = src_channels;
    cvt->dst_channels = dst_channels;
    cvt->src_rate = src_rate;
    cvt->dst_rate = dst_rate;
    cvt->needed = (src_format != dst_format) || (src_channels != dst_channels) || (src_rate != dst_rate);

    // The PS3 shim currently normalizes SDL_LoadWAV_RW output to native signed 16-bit.
    // Keep SDL_AudioCVT limited to the format EoSD actually asks for: s16 -> s16.
    if (src_format != AUDIO_S16SYS || dst_format != AUDIO_S16SYS)
    {
        ShimSetError("BuildAudioCVT: unsupported audio format");
        return -1;
    }

    cvt->rate_incr = (double)src_rate / (double)dst_rate;
    cvt->len_ratio = ((double)dst_rate / (double)src_rate) * ((double)dst_channels / (double)src_channels);
    cvt->len_mult = std::max(1, (int)cvt->len_ratio + 2);
    return cvt->needed ? 1 : 0;
}
static Sint16 SDL_CVTReadS16FrameChannel(const Sint16 *src, int frames, int channels, int frame, int channel)
{
    frame = std::max(0, std::min(frames - 1, frame));
    if (channels == 1)
        return src[frame];
    if (channel >= channels)
        channel = channels - 1;
    return src[frame * channels + channel];
}

int SDL_ConvertAudio(SDL_AudioCVT *cvt)
{
    if (!cvt || !cvt->buf || cvt->len < 0)
        return -1;

    if (!cvt->needed)
    {
        cvt->len_cvt = cvt->len;
        return 0;
    }

    if (cvt->src_format != AUDIO_S16SYS || cvt->dst_format != AUDIO_S16SYS || cvt->src_channels == 0 ||
        cvt->dst_channels == 0 || cvt->src_rate <= 0 || cvt->dst_rate <= 0)
    {
        ShimSetError("ConvertAudio: unsupported conversion");
        return -1;
    }

    const int srcFrameBytes = (int)cvt->src_channels * 2;
    const int srcFrames = cvt->len / srcFrameBytes;
    if (srcFrames <= 0)
    {
        cvt->len_cvt = 0;
        return 0;
    }

    const int dstFrames = (int)(((long long)srcFrames * cvt->dst_rate + cvt->src_rate - 1) / cvt->src_rate);
    const int dstSamples = dstFrames * (int)cvt->dst_channels;
    const int dstBytes = dstSamples * 2;
    Sint16 *converted = (Sint16 *)std::malloc((size_t)dstBytes);
    if (!converted)
    {
        ShimSetError("ConvertAudio: out of memory");
        return -1;
    }

    const Sint16 *src = (const Sint16 *)cvt->buf;
    for (int df = 0; df < dstFrames; df++)
    {
        const double srcPos = (double)df * (double)cvt->src_rate / (double)cvt->dst_rate;
        const int sf0 = (int)srcPos;
        const int sf1 = std::min(srcFrames - 1, sf0 + 1);
        const double frac = srcPos - (double)sf0;

        for (int dc = 0; dc < cvt->dst_channels; dc++)
        {
            double s0;
            double s1;
            if (cvt->dst_channels == 1 && cvt->src_channels >= 2)
            {
                s0 = ((double)SDL_CVTReadS16FrameChannel(src, srcFrames, cvt->src_channels, sf0, 0) +
                      (double)SDL_CVTReadS16FrameChannel(src, srcFrames, cvt->src_channels, sf0, 1)) *
                     0.5;
                s1 = ((double)SDL_CVTReadS16FrameChannel(src, srcFrames, cvt->src_channels, sf1, 0) +
                      (double)SDL_CVTReadS16FrameChannel(src, srcFrames, cvt->src_channels, sf1, 1)) *
                     0.5;
            }
            else
            {
                int sc = (cvt->src_channels == 1) ? 0 : std::min((int)cvt->src_channels - 1, dc);
                s0 = SDL_CVTReadS16FrameChannel(src, srcFrames, cvt->src_channels, sf0, sc);
                s1 = SDL_CVTReadS16FrameChannel(src, srcFrames, cvt->src_channels, sf1, sc);
            }

            int sample = (int)(s0 + (s1 - s0) * frac);
            sample = std::max(-32768, std::min(32767, sample));
            converted[df * cvt->dst_channels + dc] = (Sint16)sample;
        }
    }

    std::memcpy(cvt->buf, converted, (size_t)dstBytes);
    std::free(converted);
    cvt->len_cvt = dstBytes;
    return 0;
}

// ---------------------------------------------------------------- image/ttf
int IMG_Init(int flags)
{
    return flags;
}
void IMG_Quit(void)
{
}

#if defined(TH06_PSP) || defined(__PSL1GHT__)
struct TTF_GlyphCacheEntry
{
    Uint32 codepoint;
    int ptsize;
    int bitmapLeft;
    int bitmapTop;
    int advance;
    unsigned int width;
    unsigned int rows;
    Uint8 *pixels;
    bool valid;
};

struct TTF_Font
{
    FT_Face face;
    int ptsize;
    TTF_GlyphCacheEntry glyphCache[512];
    unsigned int glyphCacheNext;
};

static FT_Library g_FreetypeLibrary;
static bool g_FreetypeReady;

static bool TTF_ResolvePath(const char *file, char *out, size_t outSize)
{
    if (!file || !file[0])
        return false;

    if (file[0] == '/')
    {
        snprintf(out, outSize, "%s", file);
        return true;
    }

#ifdef TH06_PSP
    // Distributed free font lives beside EBOOT.  Also accept a user-provided
    // font inside the copied th6 directory without modifying that directory.
    snprintf(out, outSize, "%s/%s", th06_psp_game_dir(), file);
    FILE *fontFile = std::fopen(out, "rb");
    if (fontFile)
    {
        std::fclose(fontFile);
        return true;
    }
    snprintf(out, outSize, "%s/%s", th06_psp_data_dir(), file);
    return true;
#endif

#if defined(TH06_PS3) && defined(TH06_PS3_BASEDIR)
    snprintf(out, outSize, "%s/%s", th06_basedir, file);
    FILE *fp = std::fopen(out, "rb");
    if (fp)
    {
        std::fclose(fp);
        return true;
    }
#endif

    snprintf(out, outSize, "%s", file);
    return true;
}

static const char *TTF_NextUtf8(const char *p, Uint32 *out)
{
    const unsigned char *s = (const unsigned char *)p;
    if (s[0] == 0)
    {
        *out = 0;
        return p;
    }
    if (s[0] < 0x80)
    {
        *out = s[0];
        return p + 1;
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80)
    {
        *out = ((Uint32)(s[0] & 0x1F) << 6) | (Uint32)(s[1] & 0x3F);
        return p + 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
    {
        *out = ((Uint32)(s[0] & 0x0F) << 12) | ((Uint32)(s[1] & 0x3F) << 6) | (Uint32)(s[2] & 0x3F);
        return p + 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80)
    {
        *out = ((Uint32)(s[0] & 0x07) << 18) | ((Uint32)(s[1] & 0x3F) << 12) |
               ((Uint32)(s[2] & 0x3F) << 6) | (Uint32)(s[3] & 0x3F);
        return p + 4;
    }

    *out = s[0];
    return p + 1;
}

static int TTF_Ceil26Dot6(FT_Pos v)
{
    return (int)((v + 63) >> 6);
}

static TTF_GlyphCacheEntry *TTF_LoadCachedGlyph(TTF_Font *font, Uint32 codepoint)
{
    for (unsigned int i = 0; i < sizeof(font->glyphCache) / sizeof(font->glyphCache[0]); i++)
    {
        TTF_GlyphCacheEntry *entry = &font->glyphCache[i];
        if (entry->valid && entry->codepoint == codepoint && entry->ptsize == font->ptsize)
            return entry;
    }

    if (FT_Load_Char(font->face, codepoint, FT_LOAD_RENDER) != 0)
        return nullptr;

    TTF_GlyphCacheEntry *entry = &font->glyphCache[font->glyphCacheNext];
    font->glyphCacheNext = (font->glyphCacheNext + 1) % (sizeof(font->glyphCache) / sizeof(font->glyphCache[0]));
    std::free(entry->pixels);
    std::memset(entry, 0, sizeof(*entry));

    FT_GlyphSlot glyph = font->face->glyph;
    FT_Bitmap *bitmap = &glyph->bitmap;
    entry->codepoint = codepoint;
    entry->ptsize = font->ptsize;
    entry->bitmapLeft = glyph->bitmap_left;
    entry->bitmapTop = glyph->bitmap_top;
    entry->advance = (int)(glyph->advance.x >> 6);
    entry->width = bitmap->width;
    entry->rows = bitmap->rows;

    if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY && entry->width != 0 && entry->rows != 0)
    {
        entry->pixels = (Uint8 *)std::malloc((size_t)entry->width * entry->rows);
        if (!entry->pixels)
            return nullptr;
        for (unsigned int y = 0; y < entry->rows; y++)
        {
            const Uint8 *srcRow =
                bitmap->pitch >= 0 ? bitmap->buffer + y * bitmap->pitch
                                   : bitmap->buffer + (entry->rows - 1 - y) * (unsigned int)(-bitmap->pitch);
            std::memcpy(entry->pixels + (size_t)y * entry->width, srcRow, entry->width);
        }
    }
    entry->valid = true;
    return entry;
}

static void TTF_Measure(TTF_Font *font, const char *text, int *outW, int *outH, int *outBaseline)
{
    int baseline = TTF_Ceil26Dot6(font->face->size->metrics.ascender);
    int lineHeight = TTF_Ceil26Dot6(font->face->size->metrics.height);
    if (baseline <= 0)
        baseline = font->ptsize;
    if (lineHeight <= 0)
        lineHeight = font->ptsize + 4;

    int penX = 0;
    int maxX = 1;
    int maxY = lineHeight;

    for (const char *p = text ? text : ""; *p;)
    {
        Uint32 cp;
        p = TTF_NextUtf8(p, &cp);
        if (cp == '\r' || cp == '\n')
            cp = ' ';

        TTF_GlyphCacheEntry *glyph = TTF_LoadCachedGlyph(font, cp);
        if (!glyph)
        {
            penX += std::max(1, font->ptsize / 2);
            maxX = std::max(maxX, penX);
            continue;
        }

        maxX = std::max(maxX, penX + glyph->bitmapLeft + (int)glyph->width);
        maxY = std::max(maxY, baseline - glyph->bitmapTop + (int)glyph->rows);
        penX += glyph->advance;
        maxX = std::max(maxX, penX);
    }

    if (outW)
        *outW = std::max(1, maxX + 2);
    if (outH)
        *outH = std::max(1, maxY + 2);
    if (outBaseline)
        *outBaseline = baseline;
}

int TTF_Init(void)
{
    if (g_FreetypeReady)
        return 0;
    if (FT_Init_FreeType(&g_FreetypeLibrary) != 0)
    {
        ShimSetError("TTF_Init: FT_Init_FreeType failed");
        return -1;
    }
    g_FreetypeReady = true;
    return 0;
}
void TTF_Quit(void)
{
    if (g_FreetypeReady)
    {
        FT_Done_FreeType(g_FreetypeLibrary);
        g_FreetypeLibrary = nullptr;
        g_FreetypeReady = false;
    }
}
TTF_Font *TTF_OpenFont(const char *file, int ptsize)
{
    if (TTF_Init() != 0)
        return nullptr;

    char resolved[768];
    if (!TTF_ResolvePath(file, resolved, sizeof(resolved)))
    {
        ShimSetError("TTF_OpenFont: invalid font path");
        return nullptr;
    }

    TTF_Font *font = (TTF_Font *)std::calloc(1, sizeof(TTF_Font));
    if (!font)
    {
        ShimSetError("TTF_OpenFont: out of memory");
        return nullptr;
    }

    if (FT_New_Face(g_FreetypeLibrary, resolved, 0, &font->face) != 0)
    {
        std::free(font);
        ShimSetError("TTF_OpenFont: FT_New_Face failed");
#ifdef TH06_PSP
        if (th06_psp_boot_phase())
            th06_psp_boot_notef("TTF FONT NG %s", file);
#endif
        return nullptr;
    }

    FT_Select_Charmap(font->face, FT_ENCODING_UNICODE);
    font->ptsize = ptsize > 0 ? ptsize : 10;
    if (FT_Set_Pixel_Sizes(font->face, 0, font->ptsize) != 0)
    {
        FT_Done_Face(font->face);
        std::free(font);
        ShimSetError("TTF_OpenFont: FT_Set_Pixel_Sizes failed");
        return nullptr;
    }

#ifdef TH06_PSP
    if (th06_psp_boot_phase())
        th06_psp_boot_notef("TTF FONT OK %s", file);
#endif

    return font;
}
void TTF_CloseFont(TTF_Font *font)
{
    if (!font)
        return;
    for (unsigned int i = 0; i < sizeof(font->glyphCache) / sizeof(font->glyphCache[0]); i++)
        std::free(font->glyphCache[i].pixels);
    if (font->face)
        FT_Done_Face(font->face);
    std::free(font);
}
int TTF_SetFontSize(TTF_Font *font, int ptsize)
{
    if (!font || !font->face)
        return -1;
    const int requestedSize = ptsize > 0 ? ptsize : 1;
    if (font->ptsize == requestedSize)
        return 0;
    font->ptsize = requestedSize;
    if (FT_Set_Pixel_Sizes(font->face, 0, font->ptsize) != 0)
    {
        ShimSetError("TTF_SetFontSize: FT_Set_Pixel_Sizes failed");
        return -1;
    }
    return 0;
}
SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font, const char *text, SDL_Color fg)
{
    if (!font || !font->face)
    {
        ShimSetError("TTF_RenderUTF8_Blended: invalid font");
        return nullptr;
    }

    int width, height, baseline;
    TTF_Measure(font, text, &width, &height, &baseline);
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface)
        return nullptr;

    int penX = 0;
    for (const char *p = text ? text : ""; *p;)
    {
        Uint32 cp;
        p = TTF_NextUtf8(p, &cp);
        if (cp == '\r' || cp == '\n')
            cp = ' ';

        TTF_GlyphCacheEntry *glyph = TTF_LoadCachedGlyph(font, cp);
        if (!glyph)
        {
            penX += std::max(1, font->ptsize / 2);
            continue;
        }

        const int dstX0 = penX + glyph->bitmapLeft;
        const int dstY0 = baseline - glyph->bitmapTop;

        if (glyph->pixels != nullptr)
        {
            for (unsigned int gy = 0; gy < glyph->rows; gy++)
            {
                const Uint8 *srcRow = glyph->pixels + (size_t)gy * glyph->width;
                const int dy = dstY0 + (int)gy;
                if (dy < 0 || dy >= surface->h)
                    continue;
                for (unsigned int gx = 0; gx < glyph->width; gx++)
                {
                    const int dx = dstX0 + (int)gx;
                    if (dx < 0 || dx >= surface->w)
                        continue;

                    Uint8 alpha = (Uint8)((unsigned int)srcRow[gx] * fg.a / 255u);
                    Uint8 *dst = (Uint8 *)surface->pixels + dy * surface->pitch + dx * 4;
                    if (alpha > dst[0])
                    {
                        dst[0] = alpha; // ARGB8888 byte order on PS3/BE: A,R,G,B
                        dst[1] = fg.r;
                        dst[2] = fg.g;
                        dst[3] = fg.b;
                    }
                }
            }
        }

        penX += glyph->advance;
    }

    return surface;
}
int TTF_SizeUTF8(TTF_Font *font, const char *text, int *w, int *h)
{
    if (!font || !font->face)
        return -1;
    TTF_Measure(font, text, w, h, nullptr);
    return 0;
}
const char *TTF_GetError(void)
{
    return SDL_GetError();
}
#else
int TTF_Init(void)
{
    return 0;
}
void TTF_Quit(void)
{
}
TTF_Font *TTF_OpenFont(const char *, int)
{
    static int dummy;
    return (TTF_Font *)&dummy;
}
void TTF_CloseFont(TTF_Font *)
{
}
int TTF_SetFontSize(TTF_Font *, int)
{
    return 0;
}
SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *, const char *, SDL_Color)
{
    return SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_RGBA32);
}
int TTF_SizeUTF8(TTF_Font *, const char *text, int *w, int *h)
{
    if (w)
        *w = (int)std::strlen(text);
    if (h)
        *h = 16;
    return 0;
}
const char *TTF_GetError(void)
{
    return SDL_GetError();
}
#endif
