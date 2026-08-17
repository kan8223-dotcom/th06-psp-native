#include "graphics/PspGuGfx.hpp"

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <zlib.h>

#include "AnmManager.hpp"
#include "Supervisor.hpp"
#include "audio_me.h"
#include "diag.hpp"
#include "fileio.hpp"
#include "memory.hpp"
#include "video.hpp"

namespace
{

constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 272;
constexpr int kContentWidth = kScreenHeight * 4 / 3;
constexpr int kPillarWidth = (kScreenWidth - kContentWidth) / 2;
constexpr int kBufferWidth = 512;
constexpr unsigned int kFrameBytes = kBufferWidth * kScreenHeight * 4;
constexpr unsigned int kDepthOffset = kFrameBytes * 2;
constexpr unsigned int kDepthBytes = kBufferWidth * kScreenHeight * 2;
constexpr unsigned int kEdramUsedBytes = kDepthOffset + kDepthBytes;
constexpr int kMaxTextures = 512;
constexpr int kSurfaceCacheEntries = 2;
constexpr unsigned int kSurfaceFallbackWidth = 256;
constexpr unsigned int kMaxVertexPayloadBytes = 512 * 1024;
constexpr unsigned int kListBytes = (256 + 512) * 1024;
constexpr unsigned int kListSoftLimit = kListBytes - 16 * 1024;
constexpr unsigned int kPerDrawCommandReserve = 1024;
// Reuse the never-approached tail of AnmManager's 0x18000-vertex (2.25 MiB)
// engine buffer as a per-frame native GE arena.  This adds no PSP-1000 RAM.
// Real play peaks around 6k source vertices; the reserved tail starts after
// vertex 81920 and holds 16k native textured/coloured vertices.
constexpr unsigned int kDeferredVertexArenaBytes = 384 * 1024;
// Mailbox/cache setup is not worthwhile for tiny UI quads.  Large sprite
// batches dominate the measured bullet path and amortise one ME round trip.
// Real hardware rejected the ME-packed vertex stream twice with the same
// geometry corruption.  Compile its callsite out entirely; ME remains
// available to the independent PCM mixer only.
constexpr bool kMeVertexPackingEnabled = false;
constexpr int kMeVertexPackMinCount = 96;

// sceGuStart() converts this list to the PSP's 0x40000000 uncached alias.
// Allocations returned by sceGuGetMemory() therefore become GE-visible before
// the following draw command without a per-draw dcache syscall.  The old
// command and vertex arenas totalled the same 768 KiB; combining them does not
// increase the PSP-1000 RAM budget.  The explicit soft-limit check in Draw()
// keeps commands, sceGuGetMemory's jump words and vertex payloads in bounds.
alignas(16) unsigned int gCommandList[kListBytes / sizeof(unsigned int)];

struct GuVertexPosition
{
    float x;
    float y;
    float z;
};
static_assert(sizeof(GuVertexPosition) == 12, "unexpected GE position vertex layout");

struct GuVertexTexPosition
{
    float u;
    float v;
    float x;
    float y;
    float z;
};
static_assert(sizeof(GuVertexTexPosition) == 20, "unexpected GE textured vertex layout");
static_assert(sizeof(VertexTex1Xyzrhw) == 24, "unexpected TH06 sprite vertex layout");
static_assert(offsetof(VertexTex1Xyzrhw, textureUV) == 16, "unexpected TH06 sprite UV offset");

struct GuVertexColorPosition
{
    unsigned int color;
    float x;
    float y;
    float z;
};
static_assert(sizeof(GuVertexColorPosition) == 16, "unexpected GE colored vertex layout");

struct GuVertexTexColorPosition
{
    float u;
    float v;
    unsigned int color;
    float x;
    float y;
    float z;
};
static_assert(sizeof(GuVertexTexColorPosition) == 24, "unexpected GE textured-color vertex layout");

struct GuTexture
{
    void *pixels;
    unsigned int width;
    unsigned int height;
    unsigned int storageWidth;
    unsigned int storageHeight;
    unsigned int bytes;
    unsigned int allocationBytes;
    int psm;
    float sampleScaleX;
    float sampleScaleY;
    bool used;
    bool swizzled;
    bool borrowedSurfaceCache;
    bool surfaceCandidate;
};

struct FreeTextureBlock
{
    void *pixels;
    unsigned int bytes;
};

struct SurfaceCacheEntry
{
    void *pixels;
    unsigned int logicalWidth;
    unsigned int logicalHeight;
    unsigned int contentWidth;
    unsigned int contentHeight;
    unsigned int storageWidth;
    unsigned int storageHeight;
    unsigned int bytes;
    unsigned int allocationBytes;
    unsigned long crc;
    unsigned int surfaceSerial;
    unsigned int lastUse;
    int psm;
    float sampleScaleX;
    float sampleScaleY;
    bool valid;
    bool swizzled;
};

extern "C" unsigned int th06_psp_last_locked_surface_serial();
extern "C" void th06_psp_mark_surface_ge_cached(unsigned int serial, int cached);

class PspGuGfx;
PspGuGfx *gPspGuBackend;

struct GePerfFlightRecord
{
    unsigned int valid;
    unsigned int slow;
    unsigned int busy10;
    unsigned int fps10;
    unsigned int cpu10;
    unsigned int ge10;
    unsigned int vblank10;
    unsigned int calc10;
    unsigned int draw10;
    unsigned int rest10;
    unsigned int draws;
    unsigned int vertices;
    unsigned int listKb;
    unsigned int lists10;
};

GePerfFlightRecord gGePerfFlight{};
volatile unsigned int gGePerfFlightSequence;

void PublishGePerfFlight(const GePerfFlightRecord &candidate)
{
    __atomic_add_fetch(&gGePerfFlightSequence, 1u, __ATOMIC_ACQ_REL);
    gGePerfFlight = candidate;
    __atomic_add_fetch(&gGePerfFlightSequence, 1u, __ATOMIC_RELEASE);
}

unsigned int NextPowerOfTwo(unsigned int value)
{
    unsigned int result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

unsigned int ToGuColor(ZunColor argb)
{
    // ZunColor is 0xAARRGGBB. GU_COLOR_8888 is 0xAABBGGRR, whose little
    // endian byte order is the RGBA order used by PSP textures and vertices.
    return (argb & 0xff00ff00u) | ((argb & 0x00ff0000u) >> 16) | ((argb & 0x000000ffu) << 16);
}

unsigned int FloatColor(float r, float g, float b, float a)
{
    const auto byte = [](float value) -> unsigned int {
        if (value <= 0.0f)
            return 0;
        if (value >= 1.0f)
            return 255;
        return static_cast<unsigned int>(value * 255.0f);
    };
    return byte(r) | (byte(g) << 8) | (byte(b) << 16) | (byte(a) << 24);
}

unsigned int DiffuseColor(const unsigned char *src)
{
    return static_cast<unsigned int>(src[0]) | (static_cast<unsigned int>(src[1]) << 8) |
           (static_cast<unsigned int>(src[2]) << 16) | (static_cast<unsigned int>(src[3]) << 24);
}

unsigned int BytesPerPixel(int psm)
{
    return psm == GU_PSM_8888 ? 4u : 2u;
}

ScePspFMatrix4 ToGuMatrix(const ZunMatrix &matrix)
{
    // ZunMatrix::m[first][second] stores mathematical columns (the same
    // layout as ScePspFMatrix4.x/y/z/w), so this is a named copy, not a
    // transpose guessed from a screenshot.
    ScePspFMatrix4 result{};
    float *dst = &result.x.x;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            dst[column * 4 + row] = matrix.m[column][row];
    return result;
}

class PspGuGfx final : public GfxInterface
{
  public:
    PspGuGfx()
        : mBoundTexture(0), mCurrentDrawBuffer(0), mListOpen(false), mError(false),
          mUseTexCoord(false), mUseDiffuse(false), mSpriteCollapseEnabled(false),
          mTextureEnableKnown(false), mTextureEnabled(false),
          mAppliedTexture(~0u), mColorOpKnown(false), mAppliedColorOp(COLOR_OP_MODULATE),
          mPrimitiveColorKnown(false), mAppliedPrimitiveColor(0),
          mBlendMode(BLEND_INV_SRC_ALPHA), mMatricesDirty(true),
          mDepthFunc(DEPTH_FUNC_LEQUAL), mDepthWrite(true), mDepthTest(false), mTextureFactor(0xffffffffu),
          mColorOp(COLOR_OP_MODULATE), mClearDepth(1.0f), mClearColor(0xff000000u), mFogNear(1000.0f),
          mFogFar(5000.0f), mFogColor(0xffa0a0a0u), mFrames(0), mFirstFrameUs(0), mDraws(0), mVertices(0),
          mTextureBytes(0), mEdramSize(0), mMaxListBytes(0), mListsThisFrame(0), mSurfaceCacheClock(0),
          mSurfaceOomReported(false), mSurfaceOomSerial(0), mSurfaceOomCrc(0), mTracedCreate(false),
          mTracedDummyImage(false), mTracedViewport(false), mTracedClear(false), mRuntimeTextureTraces(0),
          mRuntimeUploadTraces(0), mFrameStartUs(0), mPerfWindowStartUs(0), mPerfCpuUs(0), mPerfGeUs(0),
          mPerfVblankUs(0), mPerfStartDraws(0), mPerfStartVertices(0), mPerfFrames(0),
          mFlightWindowStartUs(0), mFlightCpuUs(0), mFlightGeUs(0), mFlightVblankUs(0),
          mFlightStartDraws(0), mFlightStartVertices(0), mFlightLists(0), mFlightFrames(0),
          mDeferredVertices(nullptr), mDeferredCount(0), mDeferredArenaUsed(0),
          mDeferredTexture(0), mDeferredColorOp(COLOR_OP_MODULATE),
          mDeferredSourceDraws(0)
    {
        std::memset(mTextures, 0, sizeof(mTextures));
        std::memset(mFreeTextureBlocks, 0, sizeof(mFreeTextureBlocks));
        std::memset(mSurfaceCache, 0, sizeof(mSurfaceCache));
        std::memset(mAttributePointers, 0, sizeof(mAttributePointers));
        std::memset(mAttributeStrides, 0, sizeof(mAttributeStrides));
        std::memset(mTracedTransform, 0, sizeof(mTracedTransform));
        std::memset(mMatrixKnown, 0, sizeof(mMatrixKnown));
        // GetViewport() must report the same physical 4:3 viewport that
        // ZunViewport::Set() produces.  Returning the whole 480-wide panel
        // made the engine reverse (0 - VIEWPORT_OFF_X) through a u32.  On the
        // PSP build that becomes an out-of-range trunc.w.s immediately after
        // the dummy texture is created.  PPSSPP explicitly saturates that
        // conversion, so accepting the invalid input there hides a real-PSP
        // failure path instead of testing it.
        mViewport[0] = kPillarWidth;
        mViewport[1] = 0;
        mViewport[2] = kContentWidth;
        mViewport[3] = kScreenHeight;
        mDepthRange[0] = 0.0f;
        mDepthRange[1] = 1.0f;
    }

    ~PspGuGfx() override
    {
        if (gPspGuBackend == this)
            gPspGuBackend = nullptr;
        if (mListOpen)
        {
            FlushDeferredDraw();
            sceGuFinish();
            sceGuSync(0, 0);
            mListOpen = false;
        }
        for (int i = 1; i < kMaxTextures; ++i)
            if (!mTextures[i].borrowedSurfaceCache)
                std::free(mTextures[i].pixels);
        for (int i = 0; i < kMaxTextures; ++i)
            std::free(mFreeTextureBlocks[i].pixels);
        for (int i = 0; i < kSurfaceCacheEntries; ++i)
            std::free(mSurfaceCache[i].pixels);
        sceGuTerm();
    }

    bool Init()
    {
        th06_psp_boot_note("GE INIT");
        if (sceGuInit() < 0)
        {
            th06_psp_boot_note("GE INIT NG");
            return false;
        }

        // TH06 textures live in ordinary aligned main RAM (0x09xxxxxx) and
        // are sampled by GE from there.  eDRAM holds only the two 8888 frame
        // buffers and the 16-bit depth buffer.  Keep the two accounting pools
        // explicit in the log: the old TxxxxKB value is not eDRAM usage.
        mEdramSize = sceGeEdramGetSize();
        const unsigned int edramFree = mEdramSize > kEdramUsedBytes ? mEdramSize - kEdramUsedBytes : 0;
        th06_psp_boot_notef("EDRAM BASE %p SIZE %uKB USED %uKB FREE %uKB", sceGeEdramGetAddr(),
                            mEdramSize / 1024u, kEdramUsedBytes / 1024u, edramFree / 1024u);
        th06_psp_boot_note("EDRAM SETSIZE NONE");
        th06_psp_boot_note("GE TEXTURE SWIZZLE ON");
        th06_psp_boot_note(g_Supervisor.cfg.windowed ? "DISPLAY WINDOWED 4:3 362x272"
                                                     : "DISPLAY FULL STRETCH 480x272");
        char spriteMarker[256];
        SceIoStat spriteMarkerStat{};
        const char *gameDir = th06_psp_game_dir();
        if (gameDir && std::snprintf(spriteMarker, sizeof(spriteMarker),
                                     "%s/TH06PSP_GE_SPRITES.OFF", gameDir) > 0 &&
            sceIoGetstat(spriteMarker, &spriteMarkerStat) >= 0)
            mSpriteCollapseEnabled = false;
        th06_psp_boot_note(mSpriteCollapseEnabled ? "GE SPRITE2 ON" : "GE SPRITE2 OFF (NO GAIN)");
        if (kEdramUsedBytes > mEdramSize)
        {
            th06_psp_boot_note("EDRAM LAYOUT OOM");
            sceGuTerm();
            return false;
        }

        sceGuStart(GU_DIRECT, gCommandList);
        mListOpen = true;

        // Match the buffer order proven on real hardware by PMDVIS: draw at
        // offset 0, display at one frame.  The previous reversed order worked
        // in PPSSPP but the real unit stopped at the first GE presentation.
        sceGuDrawBuffer(GU_PSM_8888, reinterpret_cast<void *>(0), kBufferWidth);
        sceGuDispBuffer(kScreenWidth, kScreenHeight, reinterpret_cast<void *>(kFrameBytes), kBufferWidth);
        sceGuDepthBuffer(reinterpret_cast<void *>(kDepthOffset), kBufferWidth);

        ApplyViewport();
        ApplyDepthRange();
        sceGuClearDepth(0);
        sceGuShadeModel(GU_SMOOTH);
        sceGuFrontFace(GU_CW);
        sceGuDisable(GU_CULL_FACE);
        sceGuDisable(GU_LIGHTING);
        sceGuDisable(GU_DITHER);
        sceGuDisable(GU_STENCIL_TEST);
        sceGuEnable(GU_CLIP_PLANES);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GEQUAL, 4, 0xff);
        sceGuEnable(GU_BLEND);
        ApplyBlendMode();
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_FALSE);
        sceGuDepthFunc(GU_GEQUAL);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
        sceGuTexProjMapMode(GU_UV);
        sceGuDisable(GU_TEXTURE_2D);

        if ((g_Supervisor.cfg.opts & (1 << GCOS_DONT_USE_FOG)) == 0)
            sceGuEnable(GU_FOG);
        else
            sceGuDisable(GU_FOG);
        ApplyFog();

        ZunMatrix identity;
        identity.Identity();
        for (int mode = GU_PROJECTION; mode <= GU_TEXTURE; ++mode)
        {
            sceGumMatrixMode(mode);
            sceGumLoadIdentity();
        }

        const int initListBytes = sceGuFinish();
        mListOpen = false;
        sceGuSync(0, 0);
        mMaxListBytes = std::max(mMaxListBytes, static_cast<unsigned int>(std::max(initListBytes, 0)));
        // Buffer 1 has not been drawn yet.  Clear it through the uncached
        // eDRAM alias before handing display ownership to GU.
        const unsigned int edram = reinterpret_cast<unsigned int>(sceGeEdramGetAddr());
        auto *initialDisplay = reinterpret_cast<volatile unsigned int *>((0x40000000u | edram) + kFrameBytes);
        for (int i = 0; i < kBufferWidth * kScreenHeight; ++i)
            initialDisplay[i] = 0xff000000u;
        sceDisplayWaitVblankStart();
        sceGuDisplay(GU_TRUE);
        StartList();
        ClearDisplayBlack();
        th06_psp_boot_note("GE BACKEND OK");
        return true;
    }

    void SetFogRange(f32 nearPlane, f32 farPlane) override
    {
        if (mFogNear == nearPlane && mFogFar == farPlane)
            return;
        FlushDeferredDraw();
        mFogNear = nearPlane;
        mFogFar = farPlane;
        ApplyFog();
    }

    void SetFogColor(ZunColor color) override
    {
        if (mFogColor == color)
            return;
        FlushDeferredDraw();
        mFogColor = color;
        ApplyFog();
    }

    void ToggleVertexAttribute(u8 attr, bool enable) override
    {
        if (attr & VERTEX_ATTR_TEX_COORD)
            mUseTexCoord = enable;
        if (attr & VERTEX_ATTR_DIFFUSE)
            mUseDiffuse = enable;
    }

    void SetAttributePointer(VertexAttributeArrays attr, std::size_t stride, void *ptr) override
    {
        mAttributePointers[attr] = static_cast<const unsigned char *>(ptr);
        mAttributeStrides[attr] = stride;
    }

    void SetColorOp(TextureOpComponent component, ColorOp op) override
    {
        // The inherited software backend likewise uses only the RGB selector;
        // GU_TCC_RGBA supplies the matching texture-alpha multiplication.
        if (component == COMPONENT_RGB)
            mColorOp = op;
    }

    void SetTextureFactor(ZunColor factor) override
    {
        mTextureFactor = factor;
    }

    void SetTransformMatrix(TransformMatrix type, const ZunMatrix &matrix) override
    {
        if (th06_psp_transition_trace_take(1u << 0))
            th06_psp_boot_notef("TRACE GE TRANSFORM %u", static_cast<unsigned int>(type));
        if (mFrames == 0 && static_cast<unsigned int>(type) < 4 && !mTracedTransform[type])
        {
            th06_psp_boot_notef("GE MATRIX %u ENTER", static_cast<unsigned int>(type));
            mTracedTransform[type] = true;
        }
        static const int modes[] = {GU_MODEL, GU_VIEW, GU_PROJECTION, GU_TEXTURE};
        const ScePspFMatrix4 converted = ToGuMatrix(matrix);
        if (mMatrixKnown[type] && std::memcmp(&mAppliedMatrices[type], &converted, sizeof(converted)) == 0)
            return;
        FlushDeferredDraw();
        mAppliedMatrices[type] = converted;
        mMatrixKnown[type] = true;
        sceGumMatrixMode(modes[type]);
        sceGumLoadMatrix(&converted);
        mMatricesDirty = true;
    }

    void SetTextureFilter() override
    {
        FlushDeferredDraw();
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    }

    void GetViewport(u32 *viewport) override
    {
        for (int i = 0; i < 4; ++i)
            viewport[i] = static_cast<u32>(mViewport[i]);
    }

    void GetDepthRange(f32 *depthRange) override
    {
        depthRange[0] = mDepthRange[0];
        depthRange[1] = mDepthRange[1];
    }

    void SetViewport(i32 x, i32 y, i32 width, i32 height) override
    {
        if (mFrames == 0 && !mTracedViewport)
        {
            th06_psp_boot_notef("GE VIEWPORT %ld,%ld %ldx%ld", static_cast<long>(x), static_cast<long>(y),
                                static_cast<long>(width), static_cast<long>(height));
            mTracedViewport = true;
        }
        if (mViewport[0] == x && mViewport[1] == y && mViewport[2] == width && mViewport[3] == height)
            return;
        FlushDeferredDraw();
        mViewport[0] = x;
        mViewport[1] = y;
        mViewport[2] = width;
        mViewport[3] = height;
        ApplyViewport();
    }

    void SetDepthRange(f32 nearPlane, f32 farPlane) override
    {
        if (mDepthRange[0] == nearPlane && mDepthRange[1] == farPlane)
            return;
        FlushDeferredDraw();
        mDepthRange[0] = nearPlane;
        mDepthRange[1] = farPlane;
        ApplyDepthRange();
    }

    void Enable(Capabilities cap) override
    {
        FlushDeferredDraw();
        if (cap == CAPS_BLEND)
            sceGuEnable(GU_BLEND);
        else if (cap == CAPS_DEPTH_TEST)
        {
            mDepthTest = true;
            sceGuEnable(GU_DEPTH_TEST);
        }
    }

    bool HasError() override
    {
        const bool result = mError;
        mError = false;
        return result;
    }

    void SetBlendMode(BlendMode mode) override
    {
        if (mBlendMode == mode)
            return;
        FlushDeferredDraw();
        mBlendMode = mode;
        ApplyBlendMode();
    }

    void SetDepthMask(bool enable) override
    {
        if (mDepthWrite == enable)
            return;
        FlushDeferredDraw();
        mDepthWrite = enable;
        // GU_TRUE means masked (writes disabled), opposite to GfxInterface.
        sceGuDepthMask(enable ? GU_FALSE : GU_TRUE);
    }

    void SetDepthFunc(DepthFunc func) override
    {
        if (mDepthFunc == func)
            return;
        FlushDeferredDraw();
        mDepthFunc = func;
        // The PSP depth buffer is reversed (near=65535, far=0).
        sceGuDepthFunc(func == DEPTH_FUNC_LEQUAL ? GU_GEQUAL : GU_ALWAYS);
    }

    void SetClearDepth(f32 depth) override
    {
        FlushDeferredDraw();
        mClearDepth = depth;
        sceGuClearDepth(ToHardwareDepth(depth));
    }

    void SetClearColor(f32 r, f32 g, f32 b, f32 a) override
    {
        FlushDeferredDraw();
        mClearColor = FloatColor(r, g, b, a);
        sceGuClearColor(mClearColor);
    }

    void Clear(u32 clearBits) override
    {
        FlushDeferredDraw();
        if (th06_psp_transition_trace_take(1u << 1))
            th06_psp_boot_notef("TRACE GE CLEAR %lu", static_cast<unsigned long>(clearBits));
        if (mFrames == 0 && !mTracedClear)
        {
            th06_psp_boot_notef("GE CLEAR ENTER %lu", static_cast<unsigned long>(clearBits));
            mTracedClear = true;
        }
        int bits = 0;
        if (clearBits & CLEAR_COLOR_BUFFER)
        {
            sceGuClearColor(mClearColor);
            bits |= GU_COLOR_BUFFER_BIT;
        }
        if (clearBits & CLEAR_DEPTH_BUFFER)
        {
            sceGuClearDepth(ToHardwareDepth(mClearDepth));
            bits |= GU_DEPTH_BUFFER_BIT;
        }
        if (bits)
            sceGuClear(bits);
    }

    GfxTextureHandle CreateTexture() override
    {
        if (mFrames == 0 && !mTracedCreate)
        {
            th06_psp_boot_note("GE DUMMY CREATE ENTER");
            mTracedCreate = true;
        }
        for (unsigned int i = 1; i < kMaxTextures; ++i)
        {
            if (!mTextures[i].used)
            {
                mTextures[i] = GuTexture{};
                mTextures[i].used = true;
                if (mFrames == 0 && i == 1)
                    th06_psp_boot_note("GE DUMMY CREATE OK");
                return GfxTextureHandle(i);
            }
        }
        mError = true;
        return GfxTextureHandle(0);
    }

    void BindTexture(GfxTextureHandle handle) override
    {
        if (handle.id < kMaxTextures && mTextures[handle.id].used)
            mBoundTexture = handle.id;
        else
            mBoundTexture = 0;
    }

    void DeleteTexture(GfxTextureHandle handle) override
    {
        if (handle.id == 0 || handle.id >= kMaxTextures || !mTextures[handle.id].used)
            return;
        FlushDeferredDraw();
        // A queued GE draw may still refer to this RAM. Complete it before
        // returning the allocation to newlib or reusing the handle.
        if (mTextures[handle.id].pixels && !mTextures[handle.id].borrowedSurfaceCache)
            SubmitAndRestart();
        if (mTextures[handle.id].pixels && !mTextures[handle.id].borrowedSurfaceCache)
            RecycleTexturePixels(mTextures[handle.id]);
        mTextures[handle.id] = GuTexture{};
        if (mAppliedTexture == handle.id)
            mAppliedTexture = ~0u;
        if (mBoundTexture == handle.id)
            mBoundTexture = 0;
    }

    void SetTextureImage(u32 width, u32 height, PixelFormat fmt, PixelDataType type, const void *data) override
    {
        FlushDeferredDraw();
        const bool traceDummy = mFrames == 0 && !mTracedDummyImage && width == 1 && height == 1;
        if (traceDummy)
        {
            th06_psp_boot_note("GE DUMMY IMAGE ENTER");
            mTracedDummyImage = true;
        }
        if (mBoundTexture == 0 || width == 0 || height == 0)
        {
            mError = true;
            return;
        }

        GuTexture &texture = mTextures[mBoundTexture];
        // The handle may retain its numeric value while its format, storage
        // dimensions, or backing pointer changes.  Force the next draw to
        // publish the new image state in that case.
        if (mAppliedTexture == mBoundTexture)
            mAppliedTexture = ~0u;
        const bool surfaceCandidate = fmt == PIXEL_RGB && type == PIXEL_UNSIGNED_BYTE &&
                                      (width > 512 || height > 512);
        const int psm = surfaceCandidate              ? GU_PSM_5650
                        : (type == PIXEL_UNSIGNED_BYTE) ? GU_PSM_8888
                                                       : (type == PIXEL_UNSIGNED_SHORT_5_6_5) ? GU_PSM_5650
                                                       : (type == PIXEL_UNSIGNED_SHORT_5_5_5_1) ? GU_PSM_5551
                                                                                                : GU_PSM_4444;
        // GE texture dimensions stop at 512.  TH06's surface-copy path asks
        // for a 1024x512 backing texture for each 640x480 JPEG.  Represent the
        // same normalized UV space in a 512x512 physical texture; the eventual
        // 4:3 viewport is only 362x272, so retaining a 1024-wide intermediate
        // would add RAM without adding output detail.
        const unsigned int storageWidth = NextPowerOfTwo(std::min(width, 512ul));
        const unsigned int storageHeight = NextPowerOfTwo(std::min(height, 512ul));
        const unsigned int bytes = storageWidth * storageHeight * BytesPerPixel(psm);

        if (texture.pixels && texture.borrowedSurfaceCache)
        {
            texture.pixels = nullptr;
            texture.bytes = 0;
            texture.allocationBytes = 0;
            texture.borrowedSurfaceCache = false;
        }
        if (texture.pixels && texture.allocationBytes < bytes)
        {
            SubmitAndRestart();
            RecycleTexturePixels(texture);
        }
        texture.width = width;
        texture.height = height;
        texture.storageWidth = storageWidth;
        texture.storageHeight = storageHeight;
        texture.psm = psm;
        texture.swizzled = storageWidth * BytesPerPixel(psm) >= 16u && storageHeight >= 8u;
        texture.surfaceCandidate = surfaceCandidate;
        texture.sampleScaleX = std::min(1.0f, static_cast<float>(width) / static_cast<float>(storageWidth));
        texture.sampleScaleY = std::min(1.0f, static_cast<float>(height) / static_cast<float>(storageHeight));

        // ApplySurfaceToColorBuffer always follows this null allocation with
        // one RGB sub-image.  Delay storage until that call so a cache hit does
        // not allocate, clear, convert, upload, and free 512 KiB every frame.
        if (surfaceCandidate && !data)
        {
            texture.bytes = bytes;
            return;
        }

        if (!texture.pixels)
        {
            const bool runtimeAllocation = !th06_psp_boot_phase() && bytes >= 64u * 1024u;
            const bool traceRuntimeAllocation = th06_psp_verbose_transition_diag() && runtimeAllocation &&
                                                mRuntimeTextureTraces < 32;
            if (runtimeAllocation)
                th06_psp_runtime_progress();
            if (traceRuntimeAllocation)
            {
                ++mRuntimeTextureTraces;
                th06_psp_boot_notef("GE TEX BEGIN %ux%u %uKB RAM%uKB E%u/%uKB", static_cast<unsigned int>(width),
                                    static_cast<unsigned int>(height), bytes / 1024u, mTextureBytes / 1024u,
                                    kEdramUsedBytes / 1024u, mEdramSize / 1024u);
            }
            texture.pixels = AcquireTexturePixels(bytes, &texture.allocationBytes);
            if (!texture.pixels)
            {
                mError = true;
                const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
                th06_psp_boot_notef("GE TEX OOM %ux%u %uB T%uKB H%u/%u/%uKB",
                                    static_cast<unsigned int>(width), static_cast<unsigned int>(height), bytes,
                                    mTextureBytes / 1024u, heap.usedBytes / 1024u, heap.freeBytes / 1024u,
                                    heap.topFreeBytes / 1024u);
                return;
            }
            if (traceRuntimeAllocation)
            {
                th06_psp_boot_notef("GE TEX END %ux%u OK @%p S%p RAM%uKB E%u/%uKB",
                                    static_cast<unsigned int>(width), static_cast<unsigned int>(height),
                                    texture.pixels, data, mTextureBytes / 1024u, kEdramUsedBytes / 1024u,
                                    mEdramSize / 1024u);
            }
            if (runtimeAllocation)
                th06_psp_runtime_progress();
        }
        texture.bytes = bytes;
        const bool runtimeUpload = !th06_psp_boot_phase() && bytes >= 64u * 1024u;
        const bool traceRuntimeUpload = th06_psp_verbose_transition_diag() && runtimeUpload &&
                                        mRuntimeUploadTraces < 32;
        if (runtimeUpload)
            th06_psp_runtime_progress();
        if (traceRuntimeUpload)
        {
            ++mRuntimeUploadTraces;
            th06_psp_boot_notef("GE UPLOAD BEGIN %ux%u F%u Y%u", static_cast<unsigned int>(width),
                                static_cast<unsigned int>(height), static_cast<unsigned int>(fmt),
                                static_cast<unsigned int>(type));
            th06_psp_runtime_progress();
        }

        // RGBA8 already has exactly the byte layout produced by
        // WriteTexturePixel for GU_PSM_8888.  The old generic path cleared the
        // full texture and then copied it pixel-by-pixel, doubling RAM traffic
        // during the title -> demo load.  Use a bounded contiguous copy when
        // there is no power-of-two scaling or padding.
        const bool directRgba = data && fmt == PIXEL_RGBA && type == PIXEL_UNSIGNED_BYTE &&
                                psm == GU_PSM_8888 && width == storageWidth && height == storageHeight;
        if (directRgba)
        {
            if (texture.swizzled)
                SwizzleCopy(static_cast<unsigned char *>(texture.pixels), static_cast<const unsigned char *>(data),
                            texture.storageWidth * BytesPerPixel(texture.psm), texture.storageHeight);
            else
                std::memcpy(texture.pixels, data, bytes);
            if (traceRuntimeUpload)
                th06_psp_boot_note("GE UPLOAD COPY OK");
        }
        else
        {
            std::memset(texture.pixels, 0, bytes);
            if (traceRuntimeUpload)
                th06_psp_boot_note("GE UPLOAD ZERO OK");
            if (data)
            {
                ConvertTexture(texture, fmt, type, data);
                if (traceRuntimeUpload)
                    th06_psp_boot_note("GE UPLOAD CONVERT OK");
            }
        }

        if (traceRuntimeUpload)
            th06_psp_boot_note("GE UPLOAD CACHE BEGIN");
        sceKernelDcacheWritebackRange(texture.pixels, texture.bytes);
        sceGuTexFlush();
        if (traceRuntimeUpload)
        {
            th06_psp_boot_note("GE UPLOAD END");
        }
        if (runtimeUpload)
            th06_psp_runtime_progress();
        if (traceDummy)
            th06_psp_boot_note("GE DUMMY IMAGE OK");
    }

    void SetTextureSubImage(i32 xoffset, i32 yoffset, i32 width, i32 height, const void *data) override
    {
        FlushDeferredDraw();
        if (mBoundTexture == 0 || !data)
            return;
        GuTexture &texture = mTextures[mBoundTexture];
        if (mAppliedTexture == mBoundTexture)
            mAppliedTexture = ~0u;
        if ((!texture.pixels && !texture.surfaceCandidate) || xoffset < 0 || yoffset < 0 ||
            xoffset + width > static_cast<i32>(texture.width) ||
            yoffset + height > static_cast<i32>(texture.height))
        {
            mError = true;
            return;
        }

        const unsigned char *source = static_cast<const unsigned char *>(data);
        if (texture.surfaceCandidate)
        {
            BindCachedSurface(texture, width, height, source);
            return;
        }
        const bool shrinkX = texture.width > texture.storageWidth;
        const bool shrinkY = texture.height > texture.storageHeight;
        const int dstLeft = shrinkX ? static_cast<int>((static_cast<unsigned long long>(xoffset) * texture.storageWidth) /
                                                       texture.width)
                                    : xoffset;
        const int dstTop = shrinkY ? static_cast<int>((static_cast<unsigned long long>(yoffset) * texture.storageHeight) /
                                                      texture.height)
                                   : yoffset;
        const int dstRight = shrinkX
                                 ? static_cast<int>((static_cast<unsigned long long>(xoffset + width) *
                                                     texture.storageWidth + texture.width - 1) /
                                                    texture.width)
                                 : xoffset + width;
        const int dstBottom = shrinkY
                                  ? static_cast<int>((static_cast<unsigned long long>(yoffset + height) *
                                                      texture.storageHeight + texture.height - 1) /
                                                     texture.height)
                                  : yoffset + height;

        for (int y = dstTop; y < dstBottom; ++y)
        {
            const int sourceY = shrinkY
                                    ? std::min(height - 1, std::max(0, static_cast<int>(
                                                                   ((static_cast<unsigned long long>(y) * 2 + 1) *
                                                                    texture.height) /
                                                                       (texture.storageHeight * 2) -
                                                                   yoffset)))
                                    : y - yoffset;
            for (int x = dstLeft; x < dstRight; ++x)
            {
                const int sourceX = shrinkX
                                        ? std::min(width - 1, std::max(0, static_cast<int>(
                                                                       ((static_cast<unsigned long long>(x) * 2 + 1) *
                                                                        texture.width) /
                                                                           (texture.storageWidth * 2) -
                                                                       xoffset)))
                                        : x - xoffset;
                const unsigned char *pixel = source + (sourceY * width + sourceX) * 3;
                WriteTexturePixel(texture, x, y, pixel[0], pixel[1], pixel[2], 255);
            }
        }
        sceKernelDcacheWritebackRange(texture.pixels, texture.bytes);
        sceGuTexFlush();
    }

    void ReadPixels(i32 x, i32 y, i32 width, i32 height, const void *pixels) override
    {
        if (!pixels || width <= 0 || height <= 0)
            return;
        SubmitAndRestart();

        const auto edram = reinterpret_cast<unsigned int>(sceGeEdramGetAddr());
        const auto base = reinterpret_cast<const unsigned char *>(0x40000000u | edram |
                                                                  (mCurrentDrawBuffer ? kFrameBytes : 0u));
        auto *destination = static_cast<unsigned char *>(const_cast<void *>(pixels));
        for (int row = 0; row < height; ++row)
        {
            const int sourceY = kScreenHeight - 1 - (y + row);
            if (sourceY < 0 || sourceY >= kScreenHeight || x < 0 || x + width > kScreenWidth)
            {
                std::memset(destination + row * width * 4, 0, width * 4);
                continue;
            }
            std::memcpy(destination + row * width * 4, base + (sourceY * kBufferWidth + x) * 4, width * 4);
        }
    }

    unsigned int TrimSurfaceCache()
    {
        bool hasUnborrowedEntry = false;
        for (int cacheIndex = 0; cacheIndex < kSurfaceCacheEntries; ++cacheIndex)
        {
            const SurfaceCacheEntry &entry = mSurfaceCache[cacheIndex];
            if (!entry.valid || !entry.pixels)
                continue;
            bool borrowed = false;
            for (int textureIndex = 1; textureIndex < kMaxTextures; ++textureIndex)
            {
                if (mTextures[textureIndex].used && mTextures[textureIndex].borrowedSurfaceCache &&
                    mTextures[textureIndex].pixels == entry.pixels)
                {
                    borrowed = true;
                    break;
                }
            }
            if (!borrowed)
            {
                hasUnborrowedEntry = true;
                break;
            }
        }
        if (!hasUnborrowedEntry)
            return 0;

        // The registration boundary is between rendered frames, but finish
        // the currently open direct list before revoking any GE-visible RAM.
        SubmitAndRestart();

        unsigned int freed = 0;
        for (int cacheIndex = 0; cacheIndex < kSurfaceCacheEntries; ++cacheIndex)
        {
            SurfaceCacheEntry &entry = mSurfaceCache[cacheIndex];
            if (!entry.valid || !entry.pixels)
                continue;
            bool borrowed = false;
            for (int textureIndex = 1; textureIndex < kMaxTextures; ++textureIndex)
            {
                if (mTextures[textureIndex].used && mTextures[textureIndex].borrowedSurfaceCache &&
                    mTextures[textureIndex].pixels == entry.pixels)
                {
                    borrowed = true;
                    break;
                }
            }
            if (borrowed)
                continue;
            if (entry.surfaceSerial != 0)
                th06_psp_mark_surface_ge_cached(entry.surfaceSerial, 0);
            freed += entry.allocationBytes;
            mTextureBytes -= entry.allocationBytes;
            std::free(entry.pixels);
            entry = SurfaceCacheEntry{};
        }
        return freed;
    }

    void Draw(PrimitiveType type, i32 start, i32 count) override
    {
        if (th06_psp_transition_trace_take(1u << 2))
            th06_psp_boot_notef("TRACE GE DRAW %ld", static_cast<long>(count));
        if (count <= 0 || !mAttributePointers[VERTEX_ARRAY_POSITION] ||
            mAttributeStrides[VERTEX_ARRAY_POSITION] == 0)
            return;

        // Vertex color and texture coordinates are optional GE attributes.
        // The old fixed 24-byte layout wrote both for every vertex even when
        // TH06's batched sprites used a constant texture factor.  Select the
        // smallest native layout before reserving the live vertex arena.
        const bool textured = mUseTexCoord && mAttributePointers[VERTEX_ARRAY_TEX_COORD];
        const bool colored = mUseDiffuse && mAttributePointers[VERTEX_ARRAY_DIFFUSE];
        // An optional full-screen still can exhaust even the reduced cache on
        // a severely fragmented PSP-1000 heap.  Drawing its textured quad with
        // no backing pixels disables texturing and produces an opaque white
        // rectangle over otherwise usable result UI.  Drop only that missing
        // textured draw; later ANM/text draws with valid handles still run.
        if (textured && mBoundTexture != 0 && mBoundTexture < kMaxTextures &&
            mTextures[mBoundTexture].surfaceCandidate && !mTextures[mBoundTexture].pixels)
            return;

        // AnmManager's buffered path emits every axis-aligned sprite as six
        // triangle vertices in VertexTex1Xyzrhw layout.  GE's native sprite
        // primitive represents the same rectangle with only its two opposite
        // corners.  Collapse only the exact, uncoloured engine layout and
        // require every quad in the batch to be axis-aligned and unflipped;
        // rotated/mirrored/general triangles retain the proven path below.
        bool collapseSpriteBatch =
            mSpriteCollapseEnabled && type == PRIM_TRIANGLES && count >= 6 && count % 6 == 0 &&
            textured && !colored &&
            mAttributeStrides[VERTEX_ARRAY_POSITION] == sizeof(VertexTex1Xyzrhw) &&
            mAttributeStrides[VERTEX_ARRAY_TEX_COORD] == sizeof(VertexTex1Xyzrhw) &&
            mAttributePointers[VERTEX_ARRAY_TEX_COORD] ==
                mAttributePointers[VERTEX_ARRAY_POSITION] + offsetof(VertexTex1Xyzrhw, textureUV);
        const VertexTex1Xyzrhw *spriteSource = nullptr;
        if (collapseSpriteBatch)
        {
            spriteSource = reinterpret_cast<const VertexTex1Xyzrhw *>(
                mAttributePointers[VERTEX_ARRAY_POSITION] +
                mAttributeStrides[VERTEX_ARRAY_POSITION] * start);
            const int spriteCount = count / 6;
            for (int sprite = 0; sprite < spriteCount; ++sprite)
            {
                const VertexTex1Xyzrhw *quad = spriteSource + sprite * 6;
                const ZunVec4 &topLeft = quad[0].position;
                const ZunVec4 &topRight = quad[1].position;
                const ZunVec4 &bottomLeft = quad[2].position;
                const ZunVec4 &bottomRight = quad[5].position;
                const ZunVec2 &uvTopLeft = quad[0].textureUV;
                const ZunVec2 &uvTopRight = quad[1].textureUV;
                const ZunVec2 &uvBottomLeft = quad[2].textureUV;
                const ZunVec2 &uvBottomRight = quad[5].textureUV;
                if (topLeft.x != bottomLeft.x || topRight.x != bottomRight.x ||
                    topLeft.y != topRight.y || bottomLeft.y != bottomRight.y ||
                    topLeft.z != topRight.z || topLeft.z != bottomLeft.z ||
                    topLeft.z != bottomRight.z || topLeft.x > bottomRight.x ||
                    topLeft.y > bottomRight.y || uvTopLeft.x != uvBottomLeft.x ||
                    uvTopRight.x != uvBottomRight.x || uvTopLeft.y != uvTopRight.y ||
                    uvBottomLeft.y != uvBottomRight.y)
                {
                    collapseSpriteBatch = false;
                    spriteSource = nullptr;
                    break;
                }
            }
        }

        const bool nativeSpriteInput =
            type == PRIM_TRIANGLES && textured && !colored &&
            mAttributeStrides[VERTEX_ARRAY_POSITION] == sizeof(VertexTex1Xyzrhw) &&
            mAttributeStrides[VERTEX_ARRAY_TEX_COORD] == sizeof(VertexTex1Xyzrhw) &&
            mAttributePointers[VERTEX_ARRAY_TEX_COORD] ==
                mAttributePointers[VERTEX_ARRAY_POSITION] + offsetof(VertexTex1Xyzrhw, textureUV);
        const unsigned int constantColor = ToGuColor(mTextureFactor);
        if (!collapseSpriteBatch && nativeSpriteInput && QueueDeferredTriangles(start, count, constantColor))
            return;

        // A general draw or an arena boundary must follow all queued sprite
        // triangles, never overtake them in the GE command stream.
        FlushDeferredDraw();

        const int submittedCount = collapseSpriteBatch ? count / 3 : count;
        const unsigned int vertexBytes = textured
                                             ? (colored ? sizeof(GuVertexTexColorPosition)
                                                        : sizeof(GuVertexTexPosition))
                                             : (colored ? sizeof(GuVertexColorPosition)
                                                        : sizeof(GuVertexPosition));
        const unsigned int required = static_cast<unsigned int>(submittedCount) * vertexBytes;
        if (required > kMaxVertexPayloadBytes)
        {
            mError = true;
            th06_psp_boot_notef("GE VTX OVERSIZE %u", required);
            return;
        }
        void *vertices = nullptr;
        bool packedByMe = false;
        if (!collapseSpriteBatch && kMeVertexPackingEnabled && count >= kMeVertexPackMinCount)
        {
            Th06PspMeVertexPack pack{};
            pack.position = mAttributePointers[VERTEX_ARRAY_POSITION] +
                            mAttributeStrides[VERTEX_ARRAY_POSITION] * start;
            pack.positionStride = static_cast<unsigned int>(mAttributeStrides[VERTEX_ARRAY_POSITION]);
            pack.count = static_cast<unsigned int>(count);
            pack.textured = textured ? 1u : 0u;
            pack.colored = colored ? 1u : 0u;
            if (textured)
            {
                pack.texcoord = mAttributePointers[VERTEX_ARRAY_TEX_COORD] +
                                mAttributeStrides[VERTEX_ARRAY_TEX_COORD] * start;
                pack.texcoordStride = static_cast<unsigned int>(mAttributeStrides[VERTEX_ARRAY_TEX_COORD]);
            }
            if (colored)
            {
                pack.diffuse = mAttributePointers[VERTEX_ARRAY_DIFFUSE] +
                               mAttributeStrides[VERTEX_ARRAY_DIFFUSE] * start;
                pack.diffuseStride = static_cast<unsigned int>(mAttributeStrides[VERTEX_ARRAY_DIFFUSE]);
            }
            const void *meVertices = nullptr;
            if (th06_psp_me_vertex_pack(&pack, &meVertices))
            {
                vertices = const_cast<void *>(meVertices);
                packedByMe = true;
            }
        }

        if (packedByMe)
        {
            // ME-packed streams live outside the GU list and need command
            // space only.  This branch is compiled out while vertex ME is
            // disabled after its real-hardware corruption result.
            const unsigned int listUsed = static_cast<unsigned int>(std::max(sceGuCheckList(), 0));
            if (listUsed + kPerDrawCommandReserve > kListSoftLimit)
                SubmitAndRestart();
        }
        else
        {
            // sceGuGetMemory aligns to four bytes and inserts an eight-byte
            // jump over the payload.  Keep the original live-list path as the
            // bit-identical fallback for PPSSPP, disabled ME and timeouts.
            const unsigned int listAllocation = ((required + 3u) & ~3u) + 8u;
            const unsigned int listUsed = static_cast<unsigned int>(std::max(sceGuCheckList(), 0));
            if (listUsed + listAllocation + kPerDrawCommandReserve > kListSoftLimit)
                SubmitAndRestart();
            vertices = sceGuGetMemory(static_cast<int>(required));
        }
        if (!vertices)
        {
            mError = true;
            th06_psp_boot_notef("GE VTX LIST OOM %u", required);
            return;
        }
        int vertexType = GU_VERTEX_32BITF | GU_TRANSFORM_3D;
        if (!colored && (!mPrimitiveColorKnown || mAppliedPrimitiveColor != constantColor))
        {
            sceGuColor(constantColor);
            mPrimitiveColorKnown = true;
            mAppliedPrimitiveColor = constantColor;
        }

        if (packedByMe)
        {
            // ME already produced the exact native interleaved layout.
        }
        else if (collapseSpriteBatch)
        {
            auto *out = static_cast<GuVertexTexPosition *>(vertices);
            const int spriteCount = count / 6;
            for (int sprite = 0; sprite < spriteCount; ++sprite)
            {
                const VertexTex1Xyzrhw *quad = spriteSource + sprite * 6;
                const VertexTex1Xyzrhw &topLeft = quad[0];
                const VertexTex1Xyzrhw &bottomRight = quad[5];
                out[sprite * 2].u = topLeft.textureUV.x;
                out[sprite * 2].v = topLeft.textureUV.y;
                out[sprite * 2].x = topLeft.position.x;
                out[sprite * 2].y = topLeft.position.y;
                out[sprite * 2].z = topLeft.position.z;
                out[sprite * 2 + 1].u = bottomRight.textureUV.x;
                out[sprite * 2 + 1].v = bottomRight.textureUV.y;
                out[sprite * 2 + 1].x = bottomRight.position.x;
                out[sprite * 2 + 1].y = bottomRight.position.y;
                out[sprite * 2 + 1].z = bottomRight.position.z;
            }
            vertexType |= GU_TEXTURE_32BITF;
        }
        else if (textured && colored)
        {
            auto *out = static_cast<GuVertexTexColorPosition *>(vertices);
            for (int i = 0; i < count; ++i)
            {
                const int sourceIndex = start + i;
                const auto *position = reinterpret_cast<const float *>(
                    mAttributePointers[VERTEX_ARRAY_POSITION] +
                    mAttributeStrides[VERTEX_ARRAY_POSITION] * sourceIndex);
                const auto *uv = reinterpret_cast<const float *>(
                    mAttributePointers[VERTEX_ARRAY_TEX_COORD] + mAttributeStrides[VERTEX_ARRAY_TEX_COORD] * sourceIndex);
                const auto *diffuse = mAttributePointers[VERTEX_ARRAY_DIFFUSE] +
                                      mAttributeStrides[VERTEX_ARRAY_DIFFUSE] * sourceIndex;
                out[i].u = uv[0];
                out[i].v = uv[1];
                out[i].color = DiffuseColor(diffuse);
                out[i].x = position[0];
                out[i].y = position[1];
                out[i].z = position[2];
            }
            vertexType |= GU_TEXTURE_32BITF | GU_COLOR_8888;
        }
        else if (textured)
        {
            auto *out = static_cast<GuVertexTexPosition *>(vertices);
            const bool nativeSpriteLayout =
                mAttributeStrides[VERTEX_ARRAY_POSITION] == sizeof(VertexTex1Xyzrhw) &&
                mAttributeStrides[VERTEX_ARRAY_TEX_COORD] == sizeof(VertexTex1Xyzrhw) &&
                mAttributePointers[VERTEX_ARRAY_TEX_COORD] ==
                    mAttributePointers[VERTEX_ARRAY_POSITION] + offsetof(VertexTex1Xyzrhw, textureUV);
            if (nativeSpriteLayout)
            {
                // The overwhelmingly common TH06 path is one interleaved
                // VertexTex1Xyzrhw stream.  Specialise it so GCC can advance
                // two fixed pointers; the generic path below performed two
                // runtime-stride multiplies for every one of ~6k vertices.
                const auto *src = reinterpret_cast<const VertexTex1Xyzrhw *>(
                    mAttributePointers[VERTEX_ARRAY_POSITION]) + start;
                for (int i = 0; i < count; ++i, ++src)
                {
                    out[i].u = src->textureUV.x;
                    out[i].v = src->textureUV.y;
                    out[i].x = src->position.x;
                    out[i].y = src->position.y;
                    out[i].z = src->position.z;
                }
            }
            else
            {
                for (int i = 0; i < count; ++i)
                {
                    const int sourceIndex = start + i;
                    const auto *position = reinterpret_cast<const float *>(
                        mAttributePointers[VERTEX_ARRAY_POSITION] +
                        mAttributeStrides[VERTEX_ARRAY_POSITION] * sourceIndex);
                    const auto *uv = reinterpret_cast<const float *>(
                        mAttributePointers[VERTEX_ARRAY_TEX_COORD] +
                        mAttributeStrides[VERTEX_ARRAY_TEX_COORD] * sourceIndex);
                    out[i].u = uv[0];
                    out[i].v = uv[1];
                    out[i].x = position[0];
                    out[i].y = position[1];
                    out[i].z = position[2];
                }
            }
            vertexType |= GU_TEXTURE_32BITF;
        }
        else if (colored)
        {
            auto *out = static_cast<GuVertexColorPosition *>(vertices);
            for (int i = 0; i < count; ++i)
            {
                const int sourceIndex = start + i;
                const auto *position = reinterpret_cast<const float *>(
                    mAttributePointers[VERTEX_ARRAY_POSITION] +
                    mAttributeStrides[VERTEX_ARRAY_POSITION] * sourceIndex);
                const auto *diffuse = mAttributePointers[VERTEX_ARRAY_DIFFUSE] +
                                      mAttributeStrides[VERTEX_ARRAY_DIFFUSE] * sourceIndex;
                out[i].color = DiffuseColor(diffuse);
                out[i].x = position[0];
                out[i].y = position[1];
                out[i].z = position[2];
            }
            vertexType |= GU_COLOR_8888;
        }
        else
        {
            auto *out = static_cast<GuVertexPosition *>(vertices);
            for (int i = 0; i < count; ++i)
            {
                const int sourceIndex = start + i;
                const auto *position = reinterpret_cast<const float *>(
                    mAttributePointers[VERTEX_ARRAY_POSITION] +
                    mAttributeStrides[VERTEX_ARRAY_POSITION] * sourceIndex);
                out[i].x = position[0];
                out[i].y = position[1];
                out[i].z = position[2];
            }
        }

        ApplyTexture();
        const int primitive = collapseSpriteBatch
                                  ? GU_SPRITES
                                  : (type == PRIM_TRIANGLE_STRIP ? GU_TRIANGLE_STRIP : GU_TRIANGLES);
        // libGUM's draw helper scans all four matrix dirty flags on every
        // draw.  TH06 emits many batches under one unchanged orthographic
        // matrix, so update GUM only after an actual matrix change and submit
        // the remaining draws directly to GU.
        if (mMatricesDirty)
        {
            sceGumUpdateMatrix();
            mMatricesDirty = false;
        }

        // GU_DIRECT may consume the draw before sceGuFinish().  Unlike the old
        // cached arena, sceGuGetMemory returns storage inside libGU's uncached
        // live list, so the completed vertex writes are already visible when
        // sceGuDrawArray publishes the draw command.
        sceGuDrawArray(primitive, vertexType, submittedCount, nullptr, vertices);
        ++mDraws;
        mVertices += static_cast<unsigned int>(submittedCount);
    }

    void SwapBuffers() override
    {
        FlushDeferredDraw();
        const unsigned long long cpuEndUs = sceKernelGetSystemTimeWide();
        const bool traceTransitionSwap = th06_psp_transition_trace_take(1u << 3) != 0;
        if (traceTransitionSwap)
            th06_psp_boot_note("TRACE GE SWAP BEGIN");
        const bool firstPresent = mFrames == 0;
        if (firstPresent)
            th06_psp_boot_note("GE FIRST SWAP ENTER");
        ClearPillarboxesAtPresent();
        if (firstPresent)
            th06_psp_boot_note("GE FIRST SUBMIT");
        const int listBytes = sceGuFinish();
        mListOpen = false;
        mMaxListBytes = std::max(mMaxListBytes, static_cast<unsigned int>(std::max(listBytes, 0)));
        const unsigned long long geStartUs = sceKernelGetSystemTimeWide();
        sceGuSync(0, 0);
        const unsigned long long geEndUs = sceKernelGetSystemTimeWide();
        if (firstPresent)
            th06_psp_boot_note("GE FIRST SYNC OK");
#if !defined(TH06_PSP_RECORD_SOAK)
        sceDisplayWaitVblankStart();
#endif
        const unsigned long long vblankEndUs = sceKernelGetSystemTimeWide();
        sceGuSwapBuffers();
        if (traceTransitionSwap)
            th06_psp_boot_note("TRACE GE SWAP END");
        if (firstPresent)
            th06_psp_boot_note("GE FIRST SWAP OK");
        mCurrentDrawBuffer ^= 1;

        ++mFrames;
        if (mPerfWindowStartUs == 0)
        {
            mPerfWindowStartUs = mFrameStartUs ? mFrameStartUs : cpuEndUs;
            mPerfStartDraws = mDraws;
            mPerfStartVertices = mVertices;
            // Discard chain calls made before this GE timing window opened.
            th06_psp_cpu_perf_window(nullptr, nullptr, nullptr, nullptr);
        }
        if (mFrameStartUs != 0)
            mPerfCpuUs += cpuEndUs - mFrameStartUs;
        mPerfGeUs += geEndUs - geStartUs;
        mPerfVblankUs += vblankEndUs - geEndUs;
        ++mPerfFrames;

        if (mFlightWindowStartUs == 0)
        {
            mFlightWindowStartUs = mFrameStartUs ? mFrameStartUs : cpuEndUs;
            mFlightStartDraws = mDraws;
            mFlightStartVertices = mVertices;
            th06_psp_cpu_perf_flight_window(nullptr, nullptr);
        }
        if (mFrameStartUs != 0)
            mFlightCpuUs += cpuEndUs - mFrameStartUs;
        mFlightGeUs += geEndUs - geStartUs;
        mFlightVblankUs += vblankEndUs - geEndUs;
        mFlightLists += mListsThisFrame;
        ++mFlightFrames;

        if (mFlightFrames >= 60)
        {
            const unsigned long long elapsedUs = vblankEndUs - mFlightWindowStartUs;
            GePerfFlightRecord candidate{};
            candidate.valid = 1;
            candidate.fps10 = elapsedUs
                                  ? static_cast<unsigned int>(mFlightFrames * 10000000ull / elapsedUs)
                                  : 0;
            candidate.cpu10 = static_cast<unsigned int>(mFlightCpuUs / mFlightFrames / 100u);
            candidate.ge10 = static_cast<unsigned int>(mFlightGeUs / mFlightFrames / 100u);
            candidate.vblank10 = static_cast<unsigned int>(mFlightVblankUs / mFlightFrames / 100u);
            candidate.draws = (mDraws - mFlightStartDraws) / mFlightFrames;
            candidate.vertices = (mVertices - mFlightStartVertices) / mFlightFrames;
            candidate.listKb = mMaxListBytes / 1024u;
            candidate.lists10 = mFlightLists * 10u / mFlightFrames;
            unsigned long long calcTotalUs = 0;
            unsigned long long drawTotalUs = 0;
            th06_psp_cpu_perf_flight_window(&calcTotalUs, &drawTotalUs);
            const unsigned long long chainTotalUs = calcTotalUs + drawTotalUs;
            const unsigned long long restTotalUs = mFlightCpuUs > chainTotalUs ? mFlightCpuUs - chainTotalUs : 0;
            candidate.calc10 = static_cast<unsigned int>(calcTotalUs / mFlightFrames / 100u);
            candidate.draw10 = static_cast<unsigned int>(drawTotalUs / mFlightFrames / 100u);
            candidate.rest10 = static_cast<unsigned int>(restTotalUs / mFlightFrames / 100u);
            candidate.busy10 = candidate.cpu10 + candidate.ge10;
            candidate.slow = candidate.fps10 >= 200u && candidate.fps10 <= 450u;

            // Ignore boot/load windows and tiny title/menu draws.  Prefer a
            // real 20-45fps rendering window; otherwise retain the heaviest
            // busy window with at least 1000 vertices/frame.
            const GePerfFlightRecord current = gGePerfFlight;
            if (mFrames >= 600u && candidate.vertices >= 1000u &&
                (!current.valid || (candidate.slow && !current.slow) ||
                 (candidate.slow == current.slow && candidate.busy10 > current.busy10)))
                PublishGePerfFlight(candidate);

            mFlightWindowStartUs = vblankEndUs;
            mFlightCpuUs = 0;
            mFlightGeUs = 0;
            mFlightVblankUs = 0;
            mFlightStartDraws = mDraws;
            mFlightStartVertices = mVertices;
            mFlightLists = 0;
            mFlightFrames = 0;
        }

        if (mPerfFrames >= 300)
        {
            const unsigned long long elapsedUs = vblankEndUs - mPerfWindowStartUs;
            const unsigned int fps10 = elapsedUs
                                           ? static_cast<unsigned int>(mPerfFrames * 10000000ull / elapsedUs)
                                           : 0;
            const unsigned int cpu10 = static_cast<unsigned int>(mPerfCpuUs / mPerfFrames / 100u);
            const unsigned int ge10 = static_cast<unsigned int>(mPerfGeUs / mPerfFrames / 100u);
            const unsigned int vblank10 = static_cast<unsigned int>(mPerfVblankUs / mPerfFrames / 100u);
            const unsigned int drawsPerFrame = (mDraws - mPerfStartDraws) / mPerfFrames;
            const unsigned int verticesPerFrame = (mVertices - mPerfStartVertices) / mPerfFrames;
            unsigned long long calcTotalUs = 0;
            unsigned long long drawTotalUs = 0;
            unsigned int calcCalls = 0;
            unsigned int drawCalls = 0;
            th06_psp_cpu_perf_window(&calcTotalUs, &drawTotalUs, &calcCalls, &drawCalls);
            const unsigned long long chainTotalUs = calcTotalUs + drawTotalUs;
            const unsigned long long restTotalUs = mPerfCpuUs > chainTotalUs ? mPerfCpuUs - chainTotalUs : 0;
            const unsigned int calc10 = static_cast<unsigned int>(calcTotalUs / mPerfFrames / 100u);
            const unsigned int draw10 = static_cast<unsigned int>(drawTotalUs / mPerfFrames / 100u);
            const unsigned int rest10 = static_cast<unsigned int>(restTotalUs / mPerfFrames / 100u);
            if (th06_psp_periodic_diag_enabled())
            {
                th06_psp_boot_notef("PERF %u.%uFPS CPU%u.%ums GE%u.%ums VB%u.%ums D%u V%u", fps10 / 10,
                                    fps10 % 10, cpu10 / 10, cpu10 % 10, ge10 / 10, ge10 % 10, vblank10 / 10,
                                    vblank10 % 10, drawsPerFrame, verticesPerFrame);
                th06_psp_boot_notef("SC CALC%u.%u DRAW%u.%u REST%u.%u N%u/%u", calc10 / 10, calc10 % 10,
                                    draw10 / 10, draw10 % 10, rest10 / 10, rest10 % 10, calcCalls, drawCalls);
            }
            mPerfWindowStartUs = vblankEndUs;
            mPerfCpuUs = 0;
            mPerfGeUs = 0;
            mPerfVblankUs = 0;
            mPerfStartDraws = mDraws;
            mPerfStartVertices = mVertices;
            mPerfFrames = 0;
        }
        if (mFrames == 1)
            mFirstFrameUs = sceKernelGetSystemTimeWide();
        if (mFrames == 60)
        {
            const unsigned long long elapsed = sceKernelGetSystemTimeWide() - mFirstFrameUs;
            const unsigned int fps10 = elapsed ? static_cast<unsigned int>((59ull * 10000000ull) / elapsed) : 0;
            th06_psp_boot_notef("GE 60F %u.%uFPS D%u V%u T%uKB L%uKB", fps10 / 10, fps10 % 10, mDraws,
                                mVertices, mTextureBytes / 1024, mMaxListBytes / 1024);
        }
        th06_psp_video_note_presented();

        mListsThisFrame = 0;
        StartList();
        // The engine clears only its 4:3 viewport.  Clear the full LCD first
        // so pillarboxes cannot retain a previous scene's fog/clear color.
        ClearDisplayBlack();
    }

  private:
    GuTexture mTextures[kMaxTextures];
    FreeTextureBlock mFreeTextureBlocks[kMaxTextures];
    unsigned int mBoundTexture;
    const unsigned char *mAttributePointers[3];
    std::size_t mAttributeStrides[3];
    int mCurrentDrawBuffer;
    bool mListOpen;
    bool mError;
    bool mUseTexCoord;
    bool mUseDiffuse;
    bool mSpriteCollapseEnabled;
    bool mTextureEnableKnown;
    bool mTextureEnabled;
    unsigned int mAppliedTexture;
    bool mColorOpKnown;
    ColorOp mAppliedColorOp;
    bool mPrimitiveColorKnown;
    unsigned int mAppliedPrimitiveColor;
    BlendMode mBlendMode;
    ScePspFMatrix4 mAppliedMatrices[4];
    bool mMatrixKnown[4];
    bool mMatricesDirty;
    DepthFunc mDepthFunc;
    bool mDepthWrite;
    bool mDepthTest;
    ZunColor mTextureFactor;
    ColorOp mColorOp;
    float mClearDepth;
    unsigned int mClearColor;
    float mFogNear;
    float mFogFar;
    ZunColor mFogColor;
    i32 mViewport[4];
    float mDepthRange[2];
    unsigned int mFrames;
    unsigned long long mFirstFrameUs;
    unsigned int mDraws;
    unsigned int mVertices;
    unsigned int mTextureBytes;
    unsigned int mEdramSize;
    unsigned int mMaxListBytes;
    unsigned int mListsThisFrame;
    SurfaceCacheEntry mSurfaceCache[kSurfaceCacheEntries];
    unsigned int mSurfaceCacheClock;
    bool mSurfaceOomReported;
    unsigned int mSurfaceOomSerial;
    unsigned long mSurfaceOomCrc;
    bool mTracedCreate;
    bool mTracedDummyImage;
    bool mTracedViewport;
    bool mTracedClear;
    bool mTracedTransform[4];
    unsigned int mRuntimeTextureTraces;
    unsigned int mRuntimeUploadTraces;
    unsigned long long mFrameStartUs;
    unsigned long long mPerfWindowStartUs;
    unsigned long long mPerfCpuUs;
    unsigned long long mPerfGeUs;
    unsigned long long mPerfVblankUs;
    unsigned int mPerfStartDraws;
    unsigned int mPerfStartVertices;
    unsigned int mPerfFrames;
    unsigned long long mFlightWindowStartUs;
    unsigned long long mFlightCpuUs;
    unsigned long long mFlightGeUs;
    unsigned long long mFlightVblankUs;
    unsigned int mFlightStartDraws;
    unsigned int mFlightStartVertices;
    unsigned int mFlightLists;
    unsigned int mFlightFrames;
    GuVertexTexColorPosition *mDeferredVertices;
    unsigned int mDeferredCount;
    unsigned int mDeferredArenaUsed;
    unsigned int mDeferredTexture;
    ColorOp mDeferredColorOp;
    unsigned int mDeferredSourceDraws;

    unsigned char *DeferredArenaBase() const
    {
        if (!g_AnmManager)
            return nullptr;
        auto *end = reinterpret_cast<unsigned char *>(g_AnmManager->vertexBuffer + 0x18000);
        return end - kDeferredVertexArenaBytes;
    }

    void FlushDeferredDraw()
    {
        if (!mDeferredVertices || mDeferredCount == 0)
            return;

        const unsigned int bytes = mDeferredCount * sizeof(GuVertexTexColorPosition);
        sceKernelDcacheWritebackRange(mDeferredVertices, bytes);
        sceGuDrawArray(GU_TRIANGLES,
                       GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                       static_cast<int>(mDeferredCount), nullptr, mDeferredVertices);
        ++mDraws;
        mVertices += mDeferredCount;
        mDeferredVertices = nullptr;
        mDeferredCount = 0;
        mDeferredSourceDraws = 0;
    }

    bool QueueDeferredTriangles(int start, int count, unsigned int constantColor)
    {
        if (count <= 0 || !g_AnmManager)
            return false;
        unsigned char *arenaBase = DeferredArenaBase();
        if (!arenaBase)
            return false;

        // The engine owns the low end of this same large buffer.  Refuse the
        // shortcut long before the two regions could meet, even under a future
        // content mod with far more sprites than retail TH06.
        const auto *engineWrite = reinterpret_cast<const unsigned char *>(g_AnmManager->vertexBufferEndPtr);
        if (engineWrite >= arenaBase)
            return false;

        const unsigned int bytes = static_cast<unsigned int>(count) * sizeof(GuVertexTexColorPosition);
        const bool sameState = mDeferredVertices && mDeferredTexture == mBoundTexture &&
                               mDeferredColorOp == mColorOp;
        if (mDeferredVertices && !sameState)
            FlushDeferredDraw();

        if (mDeferredArenaUsed + bytes > kDeferredVertexArenaBytes)
            return false;

        if (!mDeferredVertices)
        {
            // The delayed group emits only one primitive command.  Check the
            // command list once when it starts, rather than once for every
            // inherited engine flush that is folded into the group.
            if (static_cast<unsigned int>(std::max(sceGuCheckList(), 0)) + kPerDrawCommandReserve >
                kListSoftLimit)
                SubmitAndRestart();

            // Publish all state before delaying only the primitive command.
            // Any later real state mutation flushes this pending primitive
            // first, preserving the original command ordering.
            ApplyTexture();
            if (mMatricesDirty)
            {
                sceGumUpdateMatrix();
                mMatricesDirty = false;
            }
            mDeferredVertices = reinterpret_cast<GuVertexTexColorPosition *>(arenaBase + mDeferredArenaUsed);
            mDeferredTexture = mBoundTexture;
            mDeferredColorOp = mColorOp;
        }

        auto *out = reinterpret_cast<GuVertexTexColorPosition *>(arenaBase + mDeferredArenaUsed);
        const auto *src = reinterpret_cast<const VertexTex1Xyzrhw *>(
            mAttributePointers[VERTEX_ARRAY_POSITION]) + start;
        for (int i = 0; i < count; ++i, ++src)
        {
            out[i].u = src->textureUV.x;
            out[i].v = src->textureUV.y;
            out[i].color = constantColor;
            out[i].x = src->position.x;
            out[i].y = src->position.y;
            out[i].z = src->position.z;
        }
        mDeferredArenaUsed += bytes;
        mDeferredCount += static_cast<unsigned int>(count);
        ++mDeferredSourceDraws;
        return true;
    }

    void RecycleTexturePixels(GuTexture &texture)
    {
        if (!texture.pixels || texture.borrowedSurfaceCache)
            return;
        for (int i = 0; i < kMaxTextures; ++i)
        {
            if (!mFreeTextureBlocks[i].pixels)
            {
                mFreeTextureBlocks[i].pixels = texture.pixels;
                mFreeTextureBlocks[i].bytes = texture.allocationBytes;
                texture.pixels = nullptr;
                texture.bytes = 0;
                texture.allocationBytes = 0;
                return;
            }
        }

        mTextureBytes -= texture.allocationBytes;
        std::free(texture.pixels);
        texture.pixels = nullptr;
        texture.bytes = 0;
        texture.allocationBytes = 0;
    }

    unsigned int ReleaseFreeTextureBlocks()
    {
        unsigned int freed = 0;
        for (int i = 0; i < kMaxTextures; ++i)
        {
            FreeTextureBlock &block = mFreeTextureBlocks[i];
            if (!block.pixels)
                continue;
            freed += block.bytes;
            mTextureBytes -= block.bytes;
            std::free(block.pixels);
            block = FreeTextureBlock{};
        }
        return freed;
    }

    void *AcquireTexturePixels(unsigned int bytes, unsigned int *allocationBytes)
    {
        int best = -1;
        for (int i = 0; i < kMaxTextures; ++i)
        {
            if (!mFreeTextureBlocks[i].pixels || mFreeTextureBlocks[i].bytes < bytes)
                continue;
            if (best < 0 || mFreeTextureBlocks[i].bytes < mFreeTextureBlocks[best].bytes)
                best = i;
        }
        if (best >= 0)
        {
            void *pixels = mFreeTextureBlocks[best].pixels;
            *allocationBytes = mFreeTextureBlocks[best].bytes;
            mFreeTextureBlocks[best] = FreeTextureBlock{};
            return pixels;
        }

        void *pixels = memalign(16, bytes);
        if (pixels)
        {
            *allocationBytes = bytes;
            mTextureBytes += bytes;
        }
        return pixels;
    }

    void StartList()
    {
        // StartList follows a completed sceGuSync at every ownership boundary,
        // so every pointer from the preceding frame/list is no longer in use.
        th06_psp_me_vertex_frame_begin();
        mDeferredVertices = nullptr;
        mDeferredCount = 0;
        mDeferredArenaUsed = 0;
        mDeferredSourceDraws = 0;
        sceGuStart(GU_DIRECT, gCommandList);
        mListOpen = true;
        ++mListsThisFrame;
        mFrameStartUs = sceKernelGetSystemTimeWide();
    }

    void ClearDisplayBlack()
    {
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
        sceGuClearColor(0xff000000u);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuClearColor(mClearColor);
        ApplyViewport();
    }

    void ClearPillarboxesAtPresent()
    {
        if (!g_Supervisor.cfg.windowed)
            return;
        // Some engine draws temporarily select the 640x480 fullscreen
        // viewport after the frame-start clear.  Enforce the PSP's 4:3 output
        // aperture at the final ownership boundary, immediately before GE
        // finishes the frame.  GU clear obeys scissor, so the 362x272 game
        // image is untouched.
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuClearColor(0xff000000u);
        sceGuScissor(0, 0, kPillarWidth, kScreenHeight);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuScissor(kPillarWidth + kContentWidth, 0, kScreenWidth, kScreenHeight);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuClearColor(mClearColor);
        ApplyViewport();
    }

    struct SurfaceAxisSample
    {
        unsigned short first;
        unsigned short second;
        unsigned short fraction;
    };

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

    void BindCachedSurface(GuTexture &texture, int contentWidth, int contentHeight, const unsigned char *source)
    {
        const unsigned int surfaceSerial = th06_psp_last_locked_surface_serial();
        unsigned long crc = 0;
        if (surfaceSerial == 0)
        {
            const unsigned int sourceBytes = static_cast<unsigned int>(contentWidth * contentHeight * 3);
            crc = crc32(crc32(0L, Z_NULL, 0), source, sourceBytes);
        }
        if (mSurfaceOomReported && mSurfaceOomSerial == surfaceSerial && mSurfaceOomCrc == crc)
            return;

        SurfaceCacheEntry *entry = nullptr;
        for (int i = 0; i < kSurfaceCacheEntries; ++i)
        {
            SurfaceCacheEntry &candidate = mSurfaceCache[i];
            const bool identityMatches = surfaceSerial != 0 ? candidate.surfaceSerial == surfaceSerial
                                                            : candidate.surfaceSerial == 0 && candidate.crc == crc;
            if (candidate.valid && identityMatches && candidate.logicalWidth == texture.width &&
                candidate.logicalHeight == texture.height && candidate.contentWidth == static_cast<unsigned int>(contentWidth) &&
                candidate.contentHeight == static_cast<unsigned int>(contentHeight))
            {
                entry = &candidate;
                break;
            }
        }

        if (!entry)
        {
            entry = &mSurfaceCache[0];
            for (int i = 0; i < kSurfaceCacheEntries; ++i)
            {
                if (!mSurfaceCache[i].valid)
                {
                    entry = &mSurfaceCache[i];
                    break;
                }
                if (mSurfaceCache[i].lastUse < entry->lastUse)
                    entry = &mSurfaceCache[i];
            }

            // An older queued draw may still fetch this cache slot.  Complete
            // it before replacing the pixels; hits never need this sync.
            if (entry->valid)
            {
                SubmitAndRestart();
                if (entry->surfaceSerial != 0)
                    th06_psp_mark_surface_ge_cached(entry->surfaceSerial, 0);
            }

            const unsigned int requestedBytes = texture.bytes;
            unsigned int poolFreed = 0;
            if (!entry->pixels || entry->allocationBytes < texture.bytes)
            {
                if (entry->pixels)
                {
                    mTextureBytes -= entry->allocationBytes;
                    std::free(entry->pixels);
                    entry->pixels = nullptr;
                    entry->bytes = 0;
                    entry->allocationBytes = 0;
                }

                unsigned int allocationBytes = 0;
                entry->pixels = AcquireTexturePixels(texture.bytes, &allocationBytes);
                if (!entry->pixels)
                {
                    // Stage-local textures are deliberately retained in a
                    // best-fit pool to avoid fragmentation between stages.
                    // Once a full-screen still cannot fit, those blocks are
                    // more useful returned to newlib so adjacent holes can
                    // coalesce into the one 512 KiB surface allocation.
                    poolFreed = ReleaseFreeTextureBlocks();
                    if (poolFreed != 0)
                        entry->pixels = AcquireTexturePixels(texture.bytes, &allocationBytes);
                }
                if (!entry->pixels && texture.storageWidth > kSurfaceFallbackWidth)
                {
                    // A 256x512 RGB565 cache retains all 442 possible staff-
                    // roll rows and only reduces horizontal resolution.  It
                    // is a safe last resort on a heavily fragmented PSP-1000
                    // heap and is preferable to an opaque unbacked quad.
                    texture.storageWidth = kSurfaceFallbackWidth;
                    texture.bytes = texture.storageWidth * texture.storageHeight * BytesPerPixel(texture.psm);
                    texture.swizzled = texture.storageWidth * BytesPerPixel(texture.psm) >= 16u &&
                                       texture.storageHeight >= 8u;
                    entry->pixels = AcquireTexturePixels(texture.bytes, &allocationBytes);
                }
                if (!entry->pixels)
                {
                    entry->bytes = 0;
                    entry->allocationBytes = 0;
                    entry->valid = false;
                    mSurfaceOomReported = true;
                    mSurfaceOomSerial = surfaceSerial;
                    mSurfaceOomCrc = crc;
                    const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
                    th06_psp_boot_notef("GE SURFACE OOM %uB F%uB X%uKB T%uKB H%u/%u/%uKB",
                                        requestedBytes, texture.bytes, poolFreed / 1024u,
                                        mTextureBytes / 1024u, heap.usedBytes / 1024u,
                                        heap.freeBytes / 1024u, heap.topFreeBytes / 1024u);
                    return;
                }
                entry->allocationBytes = allocationBytes;
            }
            entry->bytes = texture.bytes;
            mSurfaceOomReported = false;
            if (poolFreed != 0)
                th06_psp_boot_notef("GE SURFACE POOL X%uKB", poolFreed / 1024u);

            entry->logicalWidth = texture.width;
            entry->logicalHeight = texture.height;
            entry->contentWidth = static_cast<unsigned int>(contentWidth);
            entry->contentHeight = static_cast<unsigned int>(contentHeight);
            entry->storageWidth = texture.storageWidth;
            entry->storageHeight = texture.storageHeight;
            entry->psm = texture.psm;
            entry->swizzled = texture.swizzled;
            entry->crc = crc;
            entry->surfaceSerial = surfaceSerial;
            entry->valid = true;
            std::memset(entry->pixels, 0, entry->bytes);

            GuTexture destination = texture;
            destination.pixels = entry->pixels;
            // These surfaces are opaque 640x480 JPEG screens stored in the
            // engine's logical 1024x512 texture.  The old generic shrink made
            // only 320 horizontal texels, then GE enlarged them to 362.  Make
            // exactly one filtered image at the selected LCD output size.
            const unsigned int dstWidth = std::min(texture.storageWidth,
                                                   static_cast<unsigned int>(g_Supervisor.cfg.windowed
                                                                                 ? kContentWidth
                                                                                 : kScreenWidth));
            // A normal 640x480 still image occupies 272 LCD rows.  Ending
            // staff-roll surfaces are 640x780 and are cropped through a
            // moving 640x480 window; retaining only 272 rows would squash the
            // entire roll before the crop.  Preserve the same vertical scale
            // for the full source (780 -> 442), which still fits GE's 512-row
            // texture limit.
            const unsigned int scaledContentHeight =
                (static_cast<unsigned int>(contentHeight) * kScreenHeight + 479u) / 480u;
            const unsigned int dstHeight = std::min(texture.storageHeight, scaledContentHeight);
            SurfaceAxisSample xSamples[512];
            SurfaceAxisSample ySamples[512];
            BuildSurfaceAxisSamples(xSamples, dstWidth, static_cast<unsigned int>(contentWidth));
            BuildSurfaceAxisSamples(ySamples, dstHeight, static_cast<unsigned int>(contentHeight));

            destination.sampleScaleX = static_cast<float>(dstWidth) * static_cast<float>(texture.width) /
                                       (static_cast<float>(texture.storageWidth) * contentWidth);
            destination.sampleScaleY = static_cast<float>(dstHeight) * static_cast<float>(texture.height) /
                                       (static_cast<float>(texture.storageHeight) * contentHeight);
            entry->sampleScaleX = destination.sampleScaleX;
            entry->sampleScaleY = destination.sampleScaleY;

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
                    WriteTexturePixel(destination, x, y, rgb[0], rgb[1], rgb[2], 255);
                }
            }
            sceKernelDcacheWritebackRange(entry->pixels, entry->bytes);
            sceGuTexFlush();
            const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
            th06_psp_boot_notef("GE SURFACE %ux%u>%ux%u P%uKB T%uKB H%u/%uKB",
                                static_cast<unsigned int>(contentWidth), static_cast<unsigned int>(contentHeight),
                                dstWidth, dstHeight, entry->bytes / 1024u, mTextureBytes / 1024u,
                                heap.usedBytes / 1024u, heap.freeBytes / 1024u);
            if (surfaceSerial != 0)
                th06_psp_mark_surface_ge_cached(surfaceSerial, 1);
        }

        entry->lastUse = ++mSurfaceCacheClock;
        texture.pixels = entry->pixels;
        texture.storageWidth = entry->storageWidth;
        texture.storageHeight = entry->storageHeight;
        texture.bytes = entry->bytes;
        texture.allocationBytes = 0;
        texture.psm = entry->psm;
        texture.swizzled = entry->swizzled;
        texture.sampleScaleX = entry->sampleScaleX;
        texture.sampleScaleY = entry->sampleScaleY;
        texture.borrowedSurfaceCache = true;
    }

    void SubmitAndRestart()
    {
        if (!mListOpen)
            return;
        FlushDeferredDraw();
        const int listBytes = sceGuFinish();
        mListOpen = false;
        mMaxListBytes = std::max(mMaxListBytes, static_cast<unsigned int>(std::max(listBytes, 0)));
        sceGuSync(0, 0);
        StartList();
    }

    static unsigned int ToHardwareDepth(float value)
    {
        value = std::max(0.0f, std::min(1.0f, value));
        return static_cast<unsigned int>((1.0f - value) * 65535.0f);
    }

    void ApplyViewport()
    {
        int x = std::max(0, static_cast<int>(mViewport[0]));
        int width = std::max(1, static_cast<int>(mViewport[2]));
        int height = std::max(1, static_cast<int>(mViewport[3]));
        int top = kScreenHeight - (static_cast<int>(mViewport[1]) + height);

        if (!g_Supervisor.cfg.windowed)
        {
            // ZunViewport has already mapped the 640-wide logical image into
            // the compile-time 362-wide 4:3 aperture.  FULLSCREEN remaps that
            // aperture to the complete 480-wide LCD at the backend boundary,
            // matching PS3 FULL STRETCH without changing engine coordinates.
            const int right = x + width;
            x = ((x - kPillarWidth) * kScreenWidth) / kContentWidth;
            const int stretchedRight =
                ((right - kPillarWidth) * kScreenWidth + kContentWidth - 1) / kContentWidth;
            width = stretchedRight - x;
        }

        top = std::max(0, top);
        x = std::max(0, x);
        if (x + width > kScreenWidth)
            width = kScreenWidth - x;
        if (top + height > kScreenHeight)
            height = kScreenHeight - top;
        width = std::max(width, 1);
        height = std::max(height, 1);

        sceGuOffset(2048 - (x + width / 2), 2048 - (top + height / 2));
        sceGuViewport(2048, 2048, width, height);
        sceGuScissor(x, top, x + width, top + height);
        sceGuEnable(GU_SCISSOR_TEST);
    }

    void ApplyDepthRange()
    {
        sceGuDepthRange(ToHardwareDepth(mDepthRange[0]), ToHardwareDepth(mDepthRange[1]));
    }

    void ApplyFog()
    {
        // GE computes fog as (viewZ + fogEnd) * fogSlope.  PSP samples use a
        // right-handed view with visible geometry at negative Z, while TH06's
        // inherited matrices put visible geometry at positive Z.  Passing the
        // negated interval makes sceGuFog emit fogEnd=-far and
        // fogSlope=-1/(far-near), i.e. exactly (far-viewZ)/(far-near), the
        // equation used by Software.cpp and the proven PS3 shader.
        sceGuFog(-mFogNear, -mFogFar, ToGuColor(mFogColor));
    }

    void ApplyBlendMode()
    {
        if (mBlendMode == BLEND_INV_SRC_ALPHA)
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        else
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xffffffffu);
    }

    void ApplyTexture()
    {
        const bool enabled = mUseTexCoord && mBoundTexture != 0 && mTextures[mBoundTexture].pixels;
        if (!mTextureEnableKnown || mTextureEnabled != enabled)
        {
            if (enabled)
                sceGuEnable(GU_TEXTURE_2D);
            else
                sceGuDisable(GU_TEXTURE_2D);
            mTextureEnableKnown = true;
            mTextureEnabled = enabled;
        }
        if (!enabled)
            return;

        const GuTexture &texture = mTextures[mBoundTexture];
        if (mAppliedTexture != mBoundTexture)
        {
            sceGuTexMode(texture.psm, 0, 0, texture.swizzled ? GU_TRUE : GU_FALSE);
            sceGuTexImage(0, texture.storageWidth, texture.storageHeight, texture.storageWidth, texture.pixels);
            // Float UVs in the GU 3D path are normalized.  Only compensate
            // for power-of-two padding; multiplying by pixel dimensions here
            // repeats the atlas hundreds of times.
            sceGuTexScale(texture.sampleScaleX, texture.sampleScaleY);
            mAppliedTexture = mBoundTexture;
        }
        if (!mColorOpKnown || mAppliedColorOp != mColorOp)
        {
            const int op = mColorOp == COLOR_OP_ADD
                               ? GU_TFX_ADD
                               : (mColorOp == COLOR_OP_REPLACE ? GU_TFX_REPLACE : GU_TFX_MODULATE);
            sceGuTexFunc(op, GU_TCC_RGBA);
            mAppliedColorOp = mColorOp;
            mColorOpKnown = true;
        }
    }

    static void WriteTexturePixel(GuTexture &texture, unsigned int x, unsigned int y, unsigned int r,
                                  unsigned int g, unsigned int b, unsigned int a)
    {
        const unsigned int bytesPerPixel = BytesPerPixel(texture.psm);
        const unsigned int rowBytes = texture.storageWidth * bytesPerPixel;
        const unsigned int byteX = x * bytesPerPixel;
        const unsigned int byteOffset = texture.swizzled
                                            ? (((y >> 3) * (rowBytes >> 4) + (byteX >> 4)) << 7) +
                                                  ((y & 7u) << 4) + (byteX & 15u)
                                            : y * rowBytes + byteX;
        auto *destination = static_cast<unsigned char *>(texture.pixels) + byteOffset;
        if (texture.psm == GU_PSM_8888)
        {
            *reinterpret_cast<unsigned int *>(destination) = r | (g << 8) | (b << 16) | (a << 24);
        }
        else if (texture.psm == GU_PSM_5650)
        {
            *reinterpret_cast<unsigned short *>(destination) =
                static_cast<unsigned short>((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
        }
        else if (texture.psm == GU_PSM_5551)
        {
            *reinterpret_cast<unsigned short *>(destination) = static_cast<unsigned short>(
                (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >= 128 ? 1u : 0u) << 15));
        }
        else
        {
            *reinterpret_cast<unsigned short *>(destination) = static_cast<unsigned short>(
                (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12));
        }
    }

    // PSPSDK's GU samples store textures as consecutive 16-byte by 8-row
    // blocks.  widthBytes and height are powers of two here, and callers only
    // select this path when they are at least one complete swizzle block.
    static void SwizzleCopy(unsigned char *destination, const unsigned char *source, unsigned int widthBytes,
                            unsigned int height)
    {
        const unsigned int widthBlocks = widthBytes / 16u;
        const unsigned int heightBlocks = height / 8u;
        for (unsigned int blockY = 0; blockY < heightBlocks; ++blockY)
        {
            for (unsigned int blockX = 0; blockX < widthBlocks; ++blockX)
            {
                unsigned char *block = destination + (blockY * widthBlocks + blockX) * 128u;
                const unsigned char *row = source + blockY * 8u * widthBytes + blockX * 16u;
                for (unsigned int y = 0; y < 8u; ++y)
                    std::memcpy(block + y * 16u, row + y * widthBytes, 16u);
            }
        }
    }

    static void ReadSourcePixel(PixelFormat fmt, PixelDataType type, const void *data, unsigned int index,
                                unsigned int &r, unsigned int &g, unsigned int &b, unsigned int &a)
    {
        const auto *bytes = static_cast<const unsigned char *>(data);
        const auto *words = static_cast<const unsigned short *>(data);
        r = g = b = 0;
        a = 255;
        if (type == PIXEL_UNSIGNED_BYTE)
        {
            const unsigned int components = fmt == PIXEL_RGB ? 3u : 4u;
            r = bytes[index * components + 0];
            g = bytes[index * components + 1];
            b = bytes[index * components + 2];
            if (components == 4)
                a = bytes[index * components + 3];
        }
        else if (type == PIXEL_UNSIGNED_SHORT_5_6_5)
        {
            const unsigned int value = words[index];
            r = ((value >> 11) & 31u) * 255u / 31u;
            g = ((value >> 5) & 63u) * 255u / 63u;
            b = (value & 31u) * 255u / 31u;
        }
        else if (type == PIXEL_UNSIGNED_SHORT_5_5_5_1)
        {
            const unsigned int value = words[index];
            r = ((value >> 11) & 31u) * 255u / 31u;
            g = ((value >> 6) & 31u) * 255u / 31u;
            b = ((value >> 1) & 31u) * 255u / 31u;
            a = (value & 1u) ? 255u : 0u;
        }
        else
        {
            const unsigned int value = words[index];
            r = ((value >> 12) & 15u) * 17u;
            g = ((value >> 8) & 15u) * 17u;
            b = ((value >> 4) & 15u) * 17u;
            a = (value & 15u) * 17u;
        }
    }

    static void ConvertTexture(GuTexture &texture, PixelFormat fmt, PixelDataType type, const void *data)
    {
        const unsigned int contentWidth = std::min(texture.width, texture.storageWidth);
        const unsigned int contentHeight = std::min(texture.height, texture.storageHeight);
        for (unsigned int y = 0; y < contentHeight; ++y)
        {
            const unsigned int sourceY = static_cast<unsigned int>(
                (static_cast<unsigned long long>(y) * texture.height) / contentHeight);
            for (unsigned int x = 0; x < contentWidth; ++x)
            {
                const unsigned int sourceX = static_cast<unsigned int>(
                    (static_cast<unsigned long long>(x) * texture.width) / contentWidth);
                unsigned int r, g, b, a;
                ReadSourcePixel(fmt, type, data, sourceY * texture.width + sourceX, r, g, b, a);
                WriteTexturePixel(texture, x, y, r, g, b, a);
            }
        }
    }
};

} // namespace

extern "C" void th06_psp_ge_diag_checkpoint(const char *tag)
{
    GePerfFlightRecord record{};
    for (;;)
    {
        const unsigned int sequenceBefore = __atomic_load_n(&gGePerfFlightSequence, __ATOMIC_ACQUIRE);
        if (sequenceBefore & 1u)
            continue;
        record = gGePerfFlight;
        const unsigned int sequenceAfter = __atomic_load_n(&gGePerfFlightSequence, __ATOMIC_ACQUIRE);
        if (sequenceBefore == sequenceAfter && !(sequenceAfter & 1u))
            break;
    }

    if (!record.valid)
    {
        th06_psp_boot_notef("GE! %s NO HEAVY WINDOW", tag ? tag : "?");
        return;
    }

    th06_psp_boot_notef("GE! %s %u.%uF C%u.%u G%u.%u B%u.%u SC%u.%u/%u.%u/%u.%u D%u V%u L%uK S%u.%u",
                        tag ? tag : "?", record.fps10 / 10u, record.fps10 % 10u,
                        record.cpu10 / 10u, record.cpu10 % 10u, record.ge10 / 10u,
                        record.ge10 % 10u, record.vblank10 / 10u, record.vblank10 % 10u,
                        record.calc10 / 10u, record.calc10 % 10u, record.draw10 / 10u,
                        record.draw10 % 10u, record.rest10 / 10u, record.rest10 % 10u,
                        record.draws, record.vertices, record.listKb, record.lists10 / 10u,
                        record.lists10 % 10u);
    unsigned int fastDraws = 0;
    unsigned int fallbackDraws = 0;
    th06_psp_bullet_fast_stats(&fastDraws, &fallbackDraws);
    th06_psp_boot_notef("BULLET FAST %u FALLBACK %u", fastDraws, fallbackDraws);
}

GfxInterface *Th06CreatePspGuBackend()
{
    auto *backend = new PspGuGfx();
    if (!backend->Init())
    {
        delete backend;
        return nullptr;
    }
    gPspGuBackend = backend;
    return backend;
}

extern "C" unsigned int th06_psp_ge_trim_surface_cache()
{
    return gPspGuBackend ? gPspGuBackend->TrimSurfaceCache() : 0u;
}
