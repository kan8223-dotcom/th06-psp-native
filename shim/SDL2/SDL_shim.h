// th06_ps3 SDL2 compatibility shim for PSL1GHT (PPC64 BE)
// Phase 1: compile-sweep stubs + functionally-correct endian/rwops/timer.
// GPLv3 (project-wide). Single-header core; all SDL2/*.h aliases include this.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------- basic types
typedef uint8_t Uint8;
typedef int8_t Sint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef uint64_t Uint64;
typedef int64_t Sint64;
typedef enum
{
    SDL_FALSE = 0,
    SDL_TRUE = 1
} SDL_bool;

#ifndef SDLCALL
#define SDLCALL
#endif

#ifndef SDL_FALLTHROUGH
#if defined(__GNUC__) && __GNUC__ >= 7
#define SDL_FALLTHROUGH __attribute__((fallthrough))
#else
#define SDL_FALLTHROUGH
#endif
#endif

// ---------------------------------------------------------------- SDL_endian
// PS3 PPC64 is big endian. Float word order matches integer order.
#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#define SDL_BYTEORDER SDL_BIG_ENDIAN
#define SDL_FLOATWORDORDER SDL_BIG_ENDIAN

static inline constexpr Uint16 SDL_Swap16(Uint16 x)
{
    return __builtin_bswap16(x);
}
static inline constexpr Uint32 SDL_Swap32(Uint32 x)
{
    return __builtin_bswap32(x);
}
static inline constexpr Uint64 SDL_Swap64(Uint64 x)
{
    return __builtin_bswap64(x);
}

// On BE host: BE reads are identity, LE reads swap.
#define SDL_SwapBE16(x) (x)
#define SDL_SwapBE32(x) (x)
#define SDL_SwapBE64(x) (x)
#define SDL_SwapLE16(x) SDL_Swap16(x)
#define SDL_SwapLE32(x) SDL_Swap32(x)
#define SDL_SwapLE64(x) SDL_Swap64(x)

// ---------------------------------------------------------------- SDL_rwops (functional, stdio-backed)
#define RW_SEEK_SET 0
#define RW_SEEK_CUR 1
#define RW_SEEK_END 2

typedef struct SDL_RWops
{
    FILE *fp;            // stdio mode
    const Uint8 *mem;    // memory mode
    size_t memSize;
    size_t memPos;
    int autoCloseDone;
} SDL_RWops;

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode);
SDL_RWops *SDL_RWFromConstMem(const void *mem, int size);
size_t SDL_RWread(SDL_RWops *ctx, void *ptr, size_t size, size_t num);
Sint64 SDL_RWseek(SDL_RWops *ctx, Sint64 offset, int whence);
Sint64 SDL_RWtell(SDL_RWops *ctx);
Sint64 SDL_RWsize(SDL_RWops *ctx);
int SDL_RWclose(SDL_RWops *ctx);
Uint16 SDL_ReadLE16(SDL_RWops *ctx);
Uint32 SDL_ReadLE32(SDL_RWops *ctx);

// ---------------------------------------------------------------- init/error/log
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
void SDL_Log(const char *fmt, ...);
SDL_bool SDL_SetHint(const char *name, const char *value);
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"

// ---------------------------------------------------------------- timer
Uint32 SDL_GetTicks(void);
Uint64 SDL_GetTicks64(void);
void SDL_Delay(Uint32 ms);
typedef Uint32 (*SDL_TimerCallback)(Uint32 interval, void *param);
typedef int SDL_TimerID;
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *param);
SDL_bool SDL_RemoveTimer(SDL_TimerID id);

// ---------------------------------------------------------------- pixels / rect / color
typedef enum
{
    SDL_PIXELFORMAT_UNKNOWN = 0,
    SDL_PIXELFORMAT_ARGB8888,
    SDL_PIXELFORMAT_RGB24,
    SDL_PIXELFORMAT_RGB565,
    SDL_PIXELFORMAT_RGBA32,
    SDL_PIXELFORMAT_RGBA4444,
    SDL_PIXELFORMAT_RGBA5551
} SDL_PixelFormatEnum;

// Bits/bytes tables keyed by the enum above (shim-local, not SDL bit encoding)
int SDL_BITSPERPIXEL(Uint32 fmt);
int SDL_BYTESPERPIXEL(Uint32 fmt);

typedef struct SDL_Rect
{
    int x, y, w, h;
} SDL_Rect;

typedef struct SDL_Color
{
    Uint8 r, g, b, a;
} SDL_Color;

typedef struct SDL_PixelFormat
{
    Uint32 format;
    Uint8 BitsPerPixel;
    Uint8 BytesPerPixel;
    Uint32 Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormat;

typedef struct SDL_Surface
{
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h;
    int pitch;
    void *pixels;
    int refcount;
} SDL_Surface;

typedef enum
{
    SDL_BLENDMODE_NONE = 0,
    SDL_BLENDMODE_BLEND = 1
} SDL_BlendMode;

SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height, int depth, Uint32 format);
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int width, int height, int depth, int pitch,
                                                Uint32 format);
SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 pixel_format, Uint32 flags);
void SDL_FreeSurface(SDL_Surface *surface);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
int SDL_BlitScaled(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
int SDL_SoftStretchLinear(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect);
int SDL_SetSurfaceBlendMode(SDL_Surface *surface, SDL_BlendMode blendMode);
int SDL_ConvertPixels(int width, int height, Uint32 src_format, const void *src, int src_pitch, Uint32 dst_format,
                      void *dst, int dst_pitch);

// ---------------------------------------------------------------- video / window / renderer
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef void *SDL_GLContext;

#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000
#define SDL_WINDOW_FULLSCREEN 0x00000001
#define SDL_WINDOW_OPENGL 0x00000002
#define SDL_WINDOW_SHOWN 0x00000004
#define SDL_RENDERER_SOFTWARE 0x00000001
#define SDL_RENDERER_ACCELERATED 0x00000002
#define SDL_TEXTUREACCESS_STREAMING 1

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h);
SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface);
void SDL_DestroyTexture(SDL_Texture *texture);
int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect);
int SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_Rect *rect);
int SDL_RenderDrawRect(SDL_Renderer *renderer, const SDL_Rect *rect);
int SDL_RenderDrawLine(SDL_Renderer *renderer, int x1, int y1, int x2, int y2);
int SDL_RenderDrawPoint(SDL_Renderer *renderer, int x, int y);
int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
void SDL_RenderPresent(SDL_Renderer *renderer);

// GL attribute plumbing (GL backends are excluded on PS3; declarations keep shared code compiling)
typedef enum
{
    SDL_GL_CONTEXT_MAJOR_VERSION,
    SDL_GL_CONTEXT_MINOR_VERSION,
    SDL_GL_CONTEXT_PROFILE_MASK
} SDL_GLattr;
#define SDL_GL_CONTEXT_PROFILE_COMPATIBILITY 0x0002
#define SDL_GL_CONTEXT_PROFILE_ES 0x0004
int SDL_GL_SetAttribute(SDL_GLattr attr, int value);
SDL_GLContext SDL_GL_CreateContext(SDL_Window *window);
void SDL_GL_DeleteContext(SDL_GLContext context);
int SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context);
SDL_bool SDL_GL_ExtensionSupported(const char *extension);
void *SDL_GL_GetProcAddress(const char *proc);
int SDL_GL_SetSwapInterval(int interval);
void SDL_GL_SwapWindow(SDL_Window *window);

// ---------------------------------------------------------------- mouse / cursor
#define SDL_ENABLE 1
#define SDL_DISABLE 0
#define SDL_BUTTON_LEFT 1
int SDL_ShowCursor(int toggle);

// ---------------------------------------------------------------- keyboard / scancode
typedef enum
{
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_Q = 20,
    SDL_SCANCODE_S = 22,
    SDL_SCANCODE_X = 27,
    SDL_SCANCODE_Z = 29,
    SDL_SCANCODE_RETURN = 40,
    SDL_SCANCODE_ESCAPE = 41,
    SDL_SCANCODE_HOME = 74,
    SDL_SCANCODE_RIGHT = 79,
    SDL_SCANCODE_LEFT = 80,
    SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_KP_1 = 89,
    SDL_SCANCODE_KP_2 = 90,
    SDL_SCANCODE_KP_3 = 91,
    SDL_SCANCODE_KP_4 = 92,
    SDL_SCANCODE_KP_6 = 94,
    SDL_SCANCODE_KP_7 = 95,
    SDL_SCANCODE_KP_8 = 96,
    SDL_SCANCODE_KP_9 = 97,
    SDL_SCANCODE_LCTRL = 224,
    SDL_SCANCODE_LSHIFT = 225,
    SDL_SCANCODE_RCTRL = 228,
    SDL_SCANCODE_RSHIFT = 229,
    SDL_NUM_SCANCODES = 512
} SDL_Scancode;
const Uint8 *SDL_GetKeyboardState(int *numkeys);
void SDL_StartTextInput(void);
void SDL_StopTextInput(void);

// ---------------------------------------------------------------- joystick / gamecontroller
typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_GameController SDL_GameController;

typedef enum
{
    SDL_CONTROLLER_AXIS_INVALID = -1,
    SDL_CONTROLLER_AXIS_LEFTX,
    SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX,
    SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_MAX
} SDL_GameControllerAxis;

typedef enum
{
    SDL_CONTROLLER_BUTTON_INVALID = -1,
    SDL_CONTROLLER_BUTTON_A = 0,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_MAX
} SDL_GameControllerButton;

int SDL_NumJoysticks(void);
SDL_bool SDL_IsGameController(int joystick_index);
SDL_GameController *SDL_GameControllerOpen(int joystick_index);
void SDL_GameControllerClose(SDL_GameController *gamecontroller);
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gamecontroller);
Uint8 SDL_GameControllerGetButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button);
Sint16 SDL_GameControllerGetAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis);
SDL_bool SDL_GameControllerHasAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis);

// ---------------------------------------------------------------- events
#define SDL_QUIT 0x100
#define SDL_MOUSEBUTTONDOWN 0x401

typedef struct SDL_MouseButtonEvent
{
    Uint32 type;
    Uint8 button;
    Sint32 x, y;
} SDL_MouseButtonEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_MouseButtonEvent button;
    Uint8 padding[64];
} SDL_Event;

int SDL_PollEvent(SDL_Event *event);

// ---------------------------------------------------------------- messagebox
#define SDL_MESSAGEBOX_ERROR 0x00000010
#define SDL_MESSAGEBOX_INFORMATION 0x00000040
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);

// ---------------------------------------------------------------- audio
typedef Uint16 SDL_AudioFormat;
#define AUDIO_S16LSB 0x8010
#define AUDIO_S16MSB 0x9010
#define AUDIO_S16SYS AUDIO_S16MSB // BE host
typedef Uint32 SDL_AudioDeviceID;
typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

typedef struct SDL_AudioSpec
{
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;

typedef struct SDL_AudioCVT
{
    int needed;
    SDL_AudioFormat src_format;
    SDL_AudioFormat dst_format;
    Uint8 src_channels;
    Uint8 dst_channels;
    int src_rate;
    int dst_rate;
    double rate_incr;
    Uint8 *buf;
    int len;
    int len_cvt;
    int len_mult;
    double len_ratio;
} SDL_AudioCVT;

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int allowed_changes);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
int SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len);
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev);
SDL_AudioSpec *SDL_LoadWAV_RW(SDL_RWops *src, int freesrc, SDL_AudioSpec *spec, Uint8 **audio_buf, Uint32 *audio_len);
void SDL_FreeWAV(Uint8 *audio_buf);
int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat src_format, Uint8 src_channels, int src_rate,
                      SDL_AudioFormat dst_format, Uint8 dst_channels, int dst_rate);
int SDL_ConvertAudio(SDL_AudioCVT *cvt);

// ---------------------------------------------------------------- SDL_image
#define IMG_INIT_JPG 0x00000001
#define IMG_INIT_PNG 0x00000002
int IMG_Init(int flags);
void IMG_Quit(void);
SDL_Surface *IMG_Load_RW(SDL_RWops *src, int freesrc);

// ---------------------------------------------------------------- SDL_ttf
typedef struct TTF_Font TTF_Font;
int TTF_Init(void);
void TTF_Quit(void);
TTF_Font *TTF_OpenFont(const char *file, int ptsize);
void TTF_CloseFont(TTF_Font *font);
int TTF_SetFontSize(TTF_Font *font, int ptsize);
SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font, const char *text, SDL_Color fg);
int TTF_SizeUTF8(TTF_Font *font, const char *text, int *w, int *h);
const char *TTF_GetError(void);
