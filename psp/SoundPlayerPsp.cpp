#include "SoundPlayer.hpp"

#include "ArcadeGcmGuard.hpp"
#include "FileSystem.hpp"
#include "Supervisor.hpp"
#include "diag.hpp"
#include "i18n.hpp"
#include "memory.hpp"
#include "utils.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_timer.h>
#include <array>
#include <cmath>
#include <cstring>
#include <new>

#ifdef TH06_PSP
#include <pspkernel.h>
#include <pspsdk.h>
#include <pspthreadman.h>
#include <malloc.h>
#include "audio_me.h"
#include "midi/MidiPsp.hpp"
#endif

#ifdef TH06_PS3
#include <sys/systime.h>
#include <sys/thread.h>
extern "C" void th06_shikigami_u32(u16 id, u32 a, u32 b, u32 c, u32 d, u32 e);
extern "C" void th06_audio_wait_and_pump(unsigned int timeoutUs);
extern "C" int th06_audio_is_suspended(void);
extern "C" void th06_breadcrumb(const char *msg);
extern "C" void th06_audio_diag_window(unsigned int *minRingFrames, unsigned int *queueDrops);
extern "C" void th06_audio_diag_snapshot(unsigned int *events, unsigned int *underruns, unsigned int *maxGapUs,
                                         unsigned int *ringFrames, unsigned int *busySkips);

// ==== SE stutter調査 (2026-08-03、指示書§1): 5秒窓・全件観測 ====
// 書き手: MixAudio統計=ミキサスレッドのみ / SE棄却=ゲームスレッド(atomic)
static u64 AudNowUs()
{
    u64 sec, nsec;
    sysGetCurrentTime(&sec, &nsec);
    return sec * 1000000ull + nsec / 1000ull;
}
static u32 s_AudMaxWaitUs;   // soundBufMutex取得待ちmax
static u32 s_AudMaxHoldUs;   // 保持時間max
static u32 s_AudSumHoldUs;   // 保持時間累積 (avg用)
static u32 s_AudMixCalls;    // MixAudio呼び出し数
static u32 s_AudMaxSeUs;     // 保持中のSEミックス部max
static u32 s_AudMaxBgmUs;    // 保持中のBGMストリーミング部max
static u32 s_AudMaxChans;    // playingChannels max
static u32 s_AudSeTrig;      // SE発火 全件数
static u32 s_AudMaxTargetFrames; // 1mixのフレーム数max (catch-up規模)
static std::atomic<u32> s_AudSeDrop{0}; // 3枠/フレーム溢れ棄却 全件数
#endif

// This would all be a lot easier with SDL_mixer, but SDL_mixer doesn't permit any way of doing custom
//   loop points that would be accurate to the sample like EoSD needs. So instead we get to read WAVs and
//   mix everything by hand. Yay

#define BACKGROUND_MUSIC_WAV_NUM_CHANNELS 2
#define BACKGROUND_MUSIC_WAV_SAMPLE_RATE 44100
#define BACKGROUND_MUSIC_WAV_BITS_PER_SAMPLE 16
#define BACKGROUND_MUSIC_WAV_BLOCK_ALIGN (BACKGROUND_MUSIC_WAV_BITS_PER_SAMPLE / 8 * BACKGROUND_MUSIC_WAV_NUM_CHANNELS)
#define BACKGROUND_MUSIC_WAV_BYTE_RATE (BACKGROUND_MUSIC_WAV_BLOCK_ALIGN * BACKGROUND_MUSIC_WAV_SAMPLE_RATE)

// DirectSound deals with volume by subtracting a number measured in hundredths of decibels from the source sound.
//   The scale is from 0 (no volume modification) to -10,000 (subtraction of 100 decibels, and basically silent).
//   20 decibels affects wave amplitude by a factor of 10

static const SoundBufferIdxVolume g_SoundBufferIdxVol[32] = {
    {0, -1500}, {0, -2000}, {1, -1200}, {1, -1400}, {2, -1000},  {3, -500},   {4, -500},   {5, -1700},
    {6, -1700}, {7, -1700}, {8, -1000}, {9, -1000}, {10, -1900}, {11, -1200}, {12, -900},  {5, -1500},
    {13, -900}, {14, -900}, {15, -600}, {16, -400}, {17, -1100}, {18, -900},  {5, -1800},  {6, -1800},
    {7, -1800}, {19, -300}, {20, -600}, {21, -800}, {22, -100},  {23, -500},  {24, -1000}, {25, -1000},
};
static const char *const g_SFXList[26] = {
    "data/wav/plst00.wav", "data/wav/enep00.wav",   "data/wav/pldead00.wav", "data/wav/power0.wav",
    "data/wav/power1.wav", "data/wav/tan00.wav",    "data/wav/tan01.wav",    "data/wav/tan02.wav",
    "data/wav/ok00.wav",   "data/wav/cancel00.wav", "data/wav/select00.wav", "data/wav/gun00.wav",
    "data/wav/cat00.wav",  "data/wav/lazer00.wav",  "data/wav/lazer01.wav",  "data/wav/enep01.wav",
    "data/wav/nep00.wav",  "data/wav/damage00.wav", "data/wav/item00.wav",   "data/wav/kira00.wav",
    "data/wav/kira01.wav", "data/wav/kira02.wav",   "data/wav/extend.wav",   "data/wav/timeout.wav",
    "data/wav/graze.wav",  "data/wav/powerup.wav",
};

namespace
{
// A TH06 BGM is 44.1 kHz stereo s16 (176,400 bytes/s).  Six 64 KiB blocks keep
// the existing 384 KiB allocation (about 2.23 s) but let the I/O worker refill
// a consumed block almost two seconds before playback needs it.  64 KiB is
// also the transfer size for which the real-PSP latency record exists.
constexpr u32 kBgmCacheFrames = 16 * 1024;
constexpr int kBgmCacheBlockCount = 6;
constexpr u32 kMaxMixSamples = 2048; // 1024 stereo frames per worker pass
#ifdef TH06_PSP
// PSP priorities become less urgent as the number increases.  Normal storage
// refill must not preempt the game every 64 KiB (~372 ms); the six-block cache
// gives it roughly two seconds of slack.  Startup and an actual cache miss may
// temporarily outrank the game so audio still fails toward silence, not a
// permanently drained read-ahead window.
constexpr int kBgmIoUrgentPriority = 0x1c;
constexpr int kBgmIoBackgroundPriority = 0x21;
#endif

struct BgmCacheBlock
{
    u32 startFrame;
    u32 frames;
    u32 requestedStartFrame;
    u32 requestedGeneration;
    volatile u32 state;
};

enum BgmCacheState : u32
{
    BGM_CACHE_EMPTY = 0,
    BGM_CACHE_FILLING = 1,
    BGM_CACHE_READY = 2,
};

struct PspBgmStream
{
    SDL_RWops *file;
    u32 dataOffset;
    u32 dataBytes;
    // 384 KiB の先読みキャッシュ。WAV を最初に開くときにヒープへ確保する（静的に持たない）。
    // MIDI モード（gm.dls）では確保されず、その分がゲームの空きに残る。PSP-1000（32MB）
    // は 3 面の読み込みと EX の読み込みで数百 KB 足りずに落ちていた（2026-10-14 実機ログ）。
    i16 (*pcm)[kBgmCacheFrames * 2];
    BgmCacheBlock blocks[kBgmCacheBlockCount];
    volatile u32 generation;
    u32 fills;
    u32 shortReads;
};

PspBgmStream gBgmStream{};
constexpr u32 kBgmCacheBytes = kBgmCacheBlockCount * kBgmCacheFrames * 2u * sizeof(i16);
static_assert(kBgmCacheBytes == 384 * 1024, "BGM cache RAM budget changed");
alignas(64) i16 gFinalMix[kMaxMixSamples];
#ifndef TH06_PSP
alignas(64) i32 gWideMix[kMaxMixSamples];
#endif

#ifdef TH06_PSP
volatile u32 gPspAudioMaxProducerGapUs;
volatile u32 gPspAudioMaxMixUs;
volatile u32 gPspAudioMaxBgmReadUs;
volatile u32 gPspAudioMixCalls;
volatile u32 gPspBgmReadCount;
volatile u32 gPspBgmCacheMissPasses;
volatile u32 gPspBgmCacheMissFrames;
SceUID gBgmPrefetchSema = -1;
SceUID gBgmPrefetchThread = -1;
volatile bool gBgmPrefetchAlive;
std::mutex gBgmFileMutex;

void RecordPspAudioMaximum(volatile u32 *value, u32 sample)
{
    u32 old = __atomic_load_n(value, __ATOMIC_RELAXED);
    while (sample > old &&
           !__atomic_compare_exchange_n(value, &old, sample, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    {
    }
}
#endif

bool BgmIsOpen()
{
    return gBgmStream.file != nullptr;
}

void CloseBgmUnlocked(SoundPlayer *player)
{
    // Revoke queued I/O before waiting for an in-flight read.  The reader owns
    // the FILE cursor under gBgmFileMutex and will discard an old-generation
    // result rather than publishing it into a newly opened song.
    __atomic_add_fetch(&gBgmStream.generation, 1u, __ATOMIC_ACQ_REL);
    gBgmFileMutex.lock();
    if (gBgmStream.file != nullptr)
        SDL_RWclose(gBgmStream.file);
    gBgmStream.file = nullptr;
    gBgmStream.dataOffset = 0;
    gBgmStream.dataBytes = 0;
    gBgmStream.fills = 0;
    gBgmStream.shortReads = 0;
    for (BgmCacheBlock &block : gBgmStream.blocks)
    {
        block.startFrame = 0;
        block.frames = 0;
        block.requestedStartFrame = 0;
        block.requestedGeneration = 0;
        __atomic_store_n(&block.state, BGM_CACHE_EMPTY, __ATOMIC_RELEASE);
    }
    gBgmFileMutex.unlock();
    player->backgroundMusic.srcWav.data = nullptr;
    player->backgroundMusic.srcWav.samples = 0;
    player->backgroundMusic.pos = 0;
    player->backgroundMusic.loopStart = 0;
    player->backgroundMusic.loopEnd = 0;
    player->backgroundMusic.fadeoutLen = 0;
    player->backgroundMusic.fadeoutProgress = 0;
}

bool FillBgmBlockLocked(int index, u32 startFrame, u32 generation)
{
    if (!BgmIsOpen() || index < 0 || index >= kBgmCacheBlockCount ||
        generation != __atomic_load_n(&gBgmStream.generation, __ATOMIC_ACQUIRE) ||
        startFrame >= gBgmStream.dataBytes / 4)
        return false;

    BgmCacheBlock &block = gBgmStream.blocks[index];
    const u32 totalFrames = gBgmStream.dataBytes / 4;
    const u32 requestedFrames = std::min(kBgmCacheFrames, totalFrames - startFrame);
    const u32 requestedBytes = requestedFrames * 4;
#ifdef TH06_PSP
    const u64 readStartUs = sceKernelGetSystemTimeWide();
    __atomic_fetch_add(&gPspBgmReadCount, 1u, __ATOMIC_RELAXED);
#endif
    if (SDL_RWseek(gBgmStream.file, gBgmStream.dataOffset + startFrame * 4, RW_SEEK_SET) < 0)
    {
#ifdef TH06_PSP
        RecordPspAudioMaximum(&gPspAudioMaxBgmReadUs,
                              static_cast<u32>(sceKernelGetSystemTimeWide() - readStartUs));
#endif
        return false;
    }
    const u32 bytesRead =
        static_cast<u32>(SDL_RWread(gBgmStream.file, gBgmStream.pcm[index], 1, requestedBytes));
#ifdef TH06_PSP
    RecordPspAudioMaximum(&gPspAudioMaxBgmReadUs,
                          static_cast<u32>(sceKernelGetSystemTimeWide() - readStartUs));
#endif
    const u32 framesRead = bytesRead / 4;
    if (framesRead == 0 || generation != __atomic_load_n(&gBgmStream.generation, __ATOMIC_ACQUIRE))
        return false;
#ifdef TH06_PSP
    // ME reads the physical backing store, not the SC's dirty cache lines.
    sceKernelDcacheWritebackRange(gBgmStream.pcm[index], framesRead * 4);
#endif
    block.startFrame = startFrame;
    block.frames = framesRead;
    ++gBgmStream.fills;
    if (bytesRead != requestedBytes)
        ++gBgmStream.shortReads;
    __atomic_store_n(&block.state, BGM_CACHE_READY, __ATOMIC_RELEASE);
    return true;
}

bool FillBgmBlockNow(int index, u32 startFrame)
{
    BgmCacheBlock &block = gBgmStream.blocks[index];
    const u32 generation = __atomic_load_n(&gBgmStream.generation, __ATOMIC_ACQUIRE);
    block.requestedStartFrame = startFrame;
    block.requestedGeneration = generation;
    __atomic_store_n(&block.state, BGM_CACHE_FILLING, __ATOMIC_RELEASE);
    const bool filled = FillBgmBlockLocked(index, startFrame, generation);
    if (!filled && generation == __atomic_load_n(&gBgmStream.generation, __ATOMIC_ACQUIRE))
        __atomic_store_n(&block.state, BGM_CACHE_EMPTY, __ATOMIC_RELEASE);
    return filled;
}

int BgmPrefetchThread(SceSize, void *)
{
    while (__atomic_load_n(&gBgmPrefetchAlive, __ATOMIC_ACQUIRE))
    {
        sceKernelWaitSema(gBgmPrefetchSema, 1, nullptr);
        if (!__atomic_load_n(&gBgmPrefetchAlive, __ATOMIC_ACQUIRE))
            break;

        for (int index = 0; index < kBgmCacheBlockCount; ++index)
        {
            BgmCacheBlock &block = gBgmStream.blocks[index];
            if (__atomic_load_n(&block.state, __ATOMIC_ACQUIRE) != BGM_CACHE_FILLING)
                continue;

            const u32 startFrame = block.requestedStartFrame;
            const u32 generation = block.requestedGeneration;
            gBgmFileMutex.lock();
            const bool filled = FillBgmBlockLocked(index, startFrame, generation);
            gBgmFileMutex.unlock();
            if (!filled && generation == __atomic_load_n(&gBgmStream.generation, __ATOMIC_ACQUIRE) &&
                block.requestedGeneration == generation)
            {
                __atomic_store_n(&block.state, BGM_CACHE_EMPTY, __ATOMIC_RELEASE);
            }
        }

        // At track start the thread is created urgent and fills the initial
        // four asynchronous blocks promptly.  Every steady-state wake runs
        // below main afterwards, using GE/vblank sleep time instead of
        // stealing a render frame at the 64 KiB refill cadence.
        sceKernelChangeThreadPriority(sceKernelGetThreadId(), kBgmIoBackgroundPriority);
    }
    sceKernelExitThread(0);
    return 0;
}

bool StartBgmPrefetchThread()
{
    if (gBgmPrefetchThread >= 0)
        return true;
    gBgmPrefetchSema =
        sceKernelCreateSema("th06_bgm_io_sem", 0, 0, kBgmCacheBlockCount, nullptr);
    if (gBgmPrefetchSema < 0)
        return false;
    __atomic_store_n(&gBgmPrefetchAlive, true, __ATOMIC_RELEASE);
    // PSP priorities increase numerically as urgency decreases.  Keep storage
    // Start urgent only long enough to establish the initial read-ahead
    // window.  BgmPrefetchThread lowers itself after draining that batch.
    gBgmPrefetchThread = sceKernelCreateThread("th06_bgm_io", BgmPrefetchThread, kBgmIoUrgentPriority, 0x4000,
                                                PSP_THREAD_ATTR_USER, nullptr);
    if (gBgmPrefetchThread < 0 || sceKernelStartThread(gBgmPrefetchThread, 0, nullptr) < 0)
    {
        __atomic_store_n(&gBgmPrefetchAlive, false, __ATOMIC_RELEASE);
        if (gBgmPrefetchThread >= 0)
            sceKernelDeleteThread(gBgmPrefetchThread);
        sceKernelDeleteSema(gBgmPrefetchSema);
        gBgmPrefetchThread = -1;
        gBgmPrefetchSema = -1;
        return false;
    }
    return true;
}

void StopBgmPrefetchThread()
{
    if (gBgmPrefetchThread < 0)
        return;
    __atomic_store_n(&gBgmPrefetchAlive, false, __ATOMIC_RELEASE);
    sceKernelSignalSema(gBgmPrefetchSema, 1);
    sceKernelWaitThreadEnd(gBgmPrefetchThread, nullptr);
    sceKernelDeleteThread(gBgmPrefetchThread);
    sceKernelDeleteSema(gBgmPrefetchSema);
    gBgmPrefetchThread = -1;
    gBgmPrefetchSema = -1;
}

int FindBgmBlock(u32 frame)
{
    for (int i = 0; i < kBgmCacheBlockCount; ++i)
    {
        const BgmCacheBlock &block = gBgmStream.blocks[i];
        if (__atomic_load_n(&block.state, __ATOMIC_ACQUIRE) == BGM_CACHE_READY &&
            frame >= block.startFrame && frame < block.startFrame + block.frames)
            return i;
    }
    return -1;
}

bool BgmFillAlreadyQueued(u32 frame)
{
    const u32 generation = __atomic_load_n(&gBgmStream.generation, __ATOMIC_ACQUIRE);
    for (const BgmCacheBlock &block : gBgmStream.blocks)
    {
        if (__atomic_load_n(&block.state, __ATOMIC_ACQUIRE) == BGM_CACHE_FILLING &&
            block.requestedGeneration == generation && frame >= block.requestedStartFrame &&
            frame < block.requestedStartFrame + kBgmCacheFrames)
            return true;
    }
    return false;
}

int FindBgmBlockOrQueued(u32 frame)
{
    const int ready = FindBgmBlock(frame);
    if (ready >= 0)
        return ready;

    const u32 generation = __atomic_load_n(&gBgmStream.generation, __ATOMIC_ACQUIRE);
    for (int i = 0; i < kBgmCacheBlockCount; ++i)
    {
        const BgmCacheBlock &block = gBgmStream.blocks[i];
        if (__atomic_load_n(&block.state, __ATOMIC_ACQUIRE) == BGM_CACHE_FILLING &&
            block.requestedGeneration == generation && frame >= block.requestedStartFrame &&
            frame < block.requestedStartFrame + kBgmCacheFrames)
            return i;
    }
    return -1;
}

bool RequestBgmBlock(int index, u32 startFrame)
{
    if (!BgmIsOpen() || index < 0 || index >= kBgmCacheBlockCount ||
        startFrame >= gBgmStream.dataBytes / 4)
        return false;
    BgmCacheBlock &block = gBgmStream.blocks[index];
    if (__atomic_load_n(&block.state, __ATOMIC_ACQUIRE) == BGM_CACHE_FILLING)
        return false;
    block.requestedStartFrame = startFrame;
    block.requestedGeneration = __atomic_load_n(&gBgmStream.generation, __ATOMIC_ACQUIRE);
    __atomic_store_n(&block.state, BGM_CACHE_FILLING, __ATOMIC_RELEASE);
    if (gBgmPrefetchSema >= 0 && sceKernelSignalSema(gBgmPrefetchSema, 1) >= 0)
        return true;
    __atomic_store_n(&block.state, BGM_CACHE_EMPTY, __ATOMIC_RELEASE);
    return false;
}

int AcquireBgmBlock(u32 frame, u32 protectedBlocks)
{
    int index = FindBgmBlock(frame);
    if (index >= 0)
        return index;

    if (BgmFillAlreadyQueued(frame))
    {
#ifdef TH06_PSP
        if (gBgmPrefetchThread >= 0)
            sceKernelChangeThreadPriority(gBgmPrefetchThread, kBgmIoUrgentPriority);
#endif
        return -1;
    }

    // A missed prefetch must never turn back into synchronous FAT I/O on the
    // mixer. Queue a safe block and let this pass emit silence for the missing
    // tail. protectedBlocks contains PCM already handed to the current ME job.
    int replace = -1;
    for (int candidate = 0; candidate < kBgmCacheBlockCount; ++candidate)
    {
        if ((protectedBlocks & (1u << candidate)) == 0 &&
            __atomic_load_n(&gBgmStream.blocks[candidate].state, __ATOMIC_ACQUIRE) != BGM_CACHE_FILLING)
        {
            replace = candidate;
            break;
        }
    }
    if (replace >= 0)
    {
#ifdef TH06_PSP
        if (gBgmPrefetchThread >= 0)
            sceKernelChangeThreadPriority(gBgmPrefetchThread, kBgmIoUrgentPriority);
#endif
        RequestBgmBlock(replace, frame);
    }
    return -1;
}

void MaintainBgmReadAhead(SoundPlayer *player, u32 position, u32 protectedBlocks)
{
    if (!BgmIsOpen() || position >= player->backgroundMusic.loopEnd ||
        player->backgroundMusic.loopStart >= player->backgroundMusic.loopEnd)
        return;

    // First describe the six-block playback window and protect every existing
    // member.  Computing the complete keep mask before replacement prevents a
    // hole near the front from evicting a valid block farther ahead.
    u32 desiredStarts[kBgmCacheBlockCount]{};
    bool missing[kBgmCacheBlockCount]{};
    u32 keepBlocks = protectedBlocks;
    u32 cursor = position;
    int desiredCount = 0;
    for (; desiredCount < kBgmCacheBlockCount; ++desiredCount)
    {
        desiredStarts[desiredCount] = cursor;
        const int index = FindBgmBlockOrQueued(cursor);
        u32 blockEnd = 0;
        if (index >= 0)
        {
            keepBlocks |= 1u << index;
            const BgmCacheBlock &block = gBgmStream.blocks[index];
            const u32 state = __atomic_load_n(&block.state, __ATOMIC_ACQUIRE);
            const u32 start = state == BGM_CACHE_READY ? block.startFrame : block.requestedStartFrame;
            const u32 frames = state == BGM_CACHE_READY
                                   ? block.frames
                                   : std::min(kBgmCacheFrames, gBgmStream.dataBytes / 4 - start);
            blockEnd = start + frames;
        }
        else
        {
            missing[desiredCount] = true;
            blockEnd = cursor +
                       std::min(kBgmCacheFrames, gBgmStream.dataBytes / 4 - cursor);
        }

        const u32 playbackEnd = std::min(blockEnd, player->backgroundMusic.loopEnd);
        if (playbackEnd <= cursor)
            break;
        const u32 next = playbackEnd < player->backgroundMusic.loopEnd
                             ? playbackEnd
                             : player->backgroundMusic.loopStart;
        if (next == position)
            break;
        cursor = next;
    }

    for (int desired = 0; desired <= desiredCount && desired < kBgmCacheBlockCount; ++desired)
    {
        if (!missing[desired] || FindBgmBlockOrQueued(desiredStarts[desired]) >= 0)
            continue;

        int replace = -1;
        for (int candidate = 0; candidate < kBgmCacheBlockCount; ++candidate)
        {
            if ((keepBlocks & (1u << candidate)) == 0 &&
                __atomic_load_n(&gBgmStream.blocks[candidate].state, __ATOMIC_ACQUIRE) !=
                    BGM_CACHE_FILLING)
            {
                replace = candidate;
                break;
            }
        }
        if (replace < 0 || !RequestBgmBlock(replace, desiredStarts[desired]))
            break;
        keepBlocks |= 1u << replace;
    }
}
} // namespace

SoundPlayer g_SoundPlayer;

SoundPlayer::SoundPlayer() : terminateFlag(false), pendingSoundMask(0)
{
    // Note: memset of an std::mutex crashes on windows
    // std::memset(this, 0, sizeof(SoundPlayer));
}

ZunResult SoundPlayer::InitializeDSound()
{
    SDL_AudioSpec desiredAudio;
    SDL_AudioSpec obtainedAudio;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        goto fail;
    }

    desiredAudio.freq = 44100;
    desiredAudio.format = AUDIO_S16SYS;
    desiredAudio.channels = 2;
    desiredAudio.samples = 2048;
    desiredAudio.padding = 0;
    desiredAudio.callback = NULL;

    this->audioDev = SDL_OpenAudioDevice(NULL, 0, &desiredAudio, &obtainedAudio, 0);

    if (this->audioDev == 0)
    {
        goto fail;
    }

    // Reserve the existing WAV window before title/stage textures fragment
    // the heap. Its old static storage existed before any of those assets;
    // allocating it in the first LoadWav inserted 384 KiB between them and
    // left the ending -> Extra path unable to allocate face12a_a.png.
    if (g_Supervisor.cfg.musicMode == WAV && gBgmStream.pcm == nullptr)
    {
        gBgmStream.pcm = static_cast<i16 (*)[kBgmCacheFrames * 2]>(memalign(64, kBgmCacheBytes));
        if (gBgmStream.pcm == nullptr)
            goto fail;
    }
    // MIDI reserves its smaller replacement window at the same early
    // ownership boundary, before menu textures and the producer thread.
    if (g_Supervisor.cfg.musicMode == MIDI && th06_midi_playable() &&
        !th06_midi_prepare_song_storage(nullptr))
        goto fail;

    if (!StartBgmPrefetchThread())
        goto fail;

    this->backgroundMusicThreadHandle = std::thread(&SoundPlayer::BackgroundMusicPlayerThread, this);

    g_GameErrorContext.Log(TH_DBG_SOUNDPLAYER_INIT_SUCCESS);
    return ZUN_SUCCESS;

fail:
    StopBgmPrefetchThread();
    free(gBgmStream.pcm);
    gBgmStream.pcm = nullptr;
    if (this->audioDev != 0)
    {
        SDL_CloseAudioDevice(this->audioDev);
        this->audioDev = 0;
    }
    g_GameErrorContext.Log(TH_ERR_SOUNDPLAYER_FAILED_TO_INITIALIZE_OBJECT);
    return ZUN_ERROR;
}

ZunResult SoundPlayer::Release(void)
{
    this->terminateFlag = true;
    this->backgroundMusicThreadHandle.join();
    this->terminateFlag = false;

    StopBGM();
    StopBgmPrefetchThread();
    free(gBgmStream.pcm);
    gBgmStream.pcm = nullptr;

    for (int i = 0; i < ARRAY_SIZE_SIGNED(this->soundBuffers); i++)
    {
        if (this->soundBuffers[i].samples != NULL)
        {
            delete[] this->soundBuffers[i].samples;
            this->soundBuffers[i].samples = NULL;
            this->soundBuffers[i].isPlaying = false;
        }
    }

    if (this->audioDev != 0)
    {
        SDL_CloseAudioDevice(this->audioDev);
        this->audioDev = 0;
    }

    return ZUN_SUCCESS;
}

void SoundPlayer::StopBGM()
{
    ArcadeGcmNonRenderGuard arcadeGcmGuard(ARCADE_GCM_PHASE_BGM_STOP);
    if (BgmIsOpen() || (gBgmStream.pcm != nullptr && g_Supervisor.cfg.musicMode != WAV))
    {
        this->soundBufMutex.lock();
        const u32 fills = gBgmStream.fills;
        const u32 shortReads = gBgmStream.shortReads;
        CloseBgmUnlocked(this);
        // CloseBgmUnlocked also runs inside MixAudio while its descriptors
        // still reference PCM. Release storage here, on the game thread,
        // after the mixer and the old-generation file reader have finished.
        const bool releaseCache = gBgmStream.pcm != nullptr && g_Supervisor.cfg.musicMode != WAV;
        bool transferredCache = false;
        if (releaseCache)
        {
            transferredCache = g_Supervisor.cfg.musicMode == MIDI && th06_midi_playable() &&
                               th06_midi_prepare_song_storage(gBgmStream.pcm);
            if (!transferredCache)
                free(gBgmStream.pcm);
            gBgmStream.pcm = nullptr;
        }
        this->soundBufMutex.unlock();
        if (releaseCache)
            th06_psp_boot_note(transferredCache ? "BGM CACHE TRANSFER 384KB -> MIDI 208KB"
                                                : "BGM CACHE RELEASE 384KB");
        th06_psp_boot_notef("BGM CLOSE R%lu S%lu", static_cast<unsigned long>(fills),
                            static_cast<unsigned long>(shortReads));
        utils::DebugPrint2("stop BGM\n");
    }
}

void SoundPlayer::FadeOut(f32 seconds)
{
    if (BgmIsOpen())
    {
        this->backgroundMusic.fadeoutLen = seconds * 44100;
        this->backgroundMusic.fadeoutProgress = 0;
    }
}

ZunResult SoundPlayer::LoadWav(const char *path)
{
    ArcadeGcmNonRenderGuard arcadeGcmGuard(ARCADE_GCM_PHASE_BGM_LOAD);
    SDL_RWops *fileStream;
    char idBuf[4];
    u32 riffSize;
    u32 wavDataSize;
    u32 dataOffset;
    u32 initialStart = 0;
    bool firstBlockReady = false;
    Th06PspHeapSnapshot heap;

    if (this->audioDev == 0)
    {
        return ZUN_ERROR;
    }

    if (g_Supervisor.cfg.playSounds == 0)
    {
        return ZUN_ERROR;
    }

    this->StopBGM();

    utils::DebugPrint2("load BGM\n");

    fileStream = SDL_RWFromFile(path, "rb");

    if (fileStream == NULL)
    {
        th06_psp_boot_notef("BGM OPEN NG %s", path);
        utils::DebugPrint2("error : wav file load error %s\n", path);
        return ZUN_ERROR;
    }

    // Minimum size of RIFF header and chunk info preceeding the sample data
    if (SDL_RWsize(fileStream) < 44)
    {
        goto fail;
    }

    if (SDL_RWread(fileStream, idBuf, 4, 1) != 1 || std::strncmp(idBuf, "RIFF", 4) != 0)
    {
        goto fail;
    }

    riffSize = SDL_ReadLE32(fileStream);

    // Same bounds check done earlier on the total filesize
    if (riffSize < 36 || riffSize > SDL_RWsize(fileStream) - 8)
    {
        goto fail;
    }

    if (SDL_RWread(fileStream, idBuf, 4, 1) != 1 || std::strncmp(idBuf, "WAVE", 4) != 0)
    {
        goto fail;
    }

    // Checks here are quite a bit less flexible than what WAV can represent. EoSD uses 44.1 kHz, stereo, 16-bit PCM
    //   so that's what we handle. We also assume that fmt and data are the only subchunks, which is definitely not
    //   a general guarantee, but it'll work fine with EoSD's WAV files.

    if (SDL_RWread(fileStream, idBuf, 4, 1) != 1 || std::strncmp(idBuf, "fmt ", 4) != 0)
    {
        goto fail;
    }

    // Format subchunk size. Guaranteed 16 for PCM data
    if (SDL_ReadLE32(fileStream) != 16)
    {
        goto fail;
    }

    // Audio format. 1 represents raw PCM samples
    if (SDL_ReadLE16(fileStream) != 1)
    {
        goto fail;
    }

    // Number of channels. We expect stereo
    if (SDL_ReadLE16(fileStream) != BACKGROUND_MUSIC_WAV_NUM_CHANNELS)
    {
        goto fail;
    }

    // Sample frequency rate
    if (SDL_ReadLE32(fileStream) != BACKGROUND_MUSIC_WAV_SAMPLE_RATE)
    {
        goto fail;
    }

    // Byte rate
    if (SDL_ReadLE32(fileStream) != BACKGROUND_MUSIC_WAV_BYTE_RATE)
    {
        goto fail;
    }

    // Block alignment
    if (SDL_ReadLE16(fileStream) != BACKGROUND_MUSIC_WAV_BLOCK_ALIGN)
    {
        goto fail;
    }

    // Bits per sample
    if (SDL_ReadLE16(fileStream) != BACKGROUND_MUSIC_WAV_BITS_PER_SAMPLE)
    {
        goto fail;
    }

    if (SDL_RWread(fileStream, idBuf, 4, 1) != 1 || std::strncmp(idBuf, "data", 4) != 0)
    {
        goto fail;
    }

    wavDataSize = SDL_ReadLE32(fileStream);

    if (wavDataSize > riffSize - 44)
    {
        goto fail;
    }

    this->backgroundMusic.srcWav.samples = wavDataSize / BACKGROUND_MUSIC_WAV_BLOCK_ALIGN;

    if (this->backgroundMusic.srcWav.samples == 0)
    {
        goto fail;
    }

    dataOffset = static_cast<u32>(SDL_RWtell(fileStream));

    // Keep the file open and publish the fixed six-block read-ahead window.
    // srcWav.data is only a non-null compatibility sentinel; PSP mixing never
    // dereferences it and never allocates wavDataSize bytes.
    this->soundBufMutex.lock();
    gBgmFileMutex.lock();
    gBgmStream.file = fileStream;
    gBgmStream.dataOffset = dataOffset;
    gBgmStream.dataBytes = wavDataSize;
    gBgmStream.fills = 0;
    gBgmStream.shortReads = 0;
    for (BgmCacheBlock &block : gBgmStream.blocks)
    {
        block.startFrame = 0;
        block.frames = 0;
        block.requestedStartFrame = 0;
        block.requestedGeneration = 0;
        __atomic_store_n(&block.state, BGM_CACHE_EMPTY, __ATOMIC_RELEASE);
    }
    // Two blocks (about 743 ms) are ready before playback begins.  The other
    // four are filled by the dedicated worker, shortening the stage-load I/O
    // burst while retaining the same 384 KiB steady-state read-ahead window.
    if (!gBgmStream.pcm)
    {
        gBgmStream.pcm = static_cast<i16 (*)[kBgmCacheFrames * 2]>(memalign(64, kBgmCacheBytes));
        if (!gBgmStream.pcm)
            th06_psp_boot_note("BGM CACHE ALLOC NG");
    }
    firstBlockReady = gBgmStream.pcm != nullptr && FillBgmBlockNow(0, 0);
    initialStart = firstBlockReady ? gBgmStream.blocks[0].frames : 0;
    if (firstBlockReady && initialStart < gBgmStream.dataBytes / 4 &&
        FillBgmBlockNow(1, initialStart))
        initialStart += gBgmStream.blocks[1].frames;
    for (int index = 2; firstBlockReady && index < kBgmCacheBlockCount &&
                        initialStart < gBgmStream.dataBytes / 4;
         ++index)
    {
        RequestBgmBlock(index, initialStart);
        initialStart += std::min(kBgmCacheFrames, gBgmStream.dataBytes / 4 - initialStart);
    }
    gBgmFileMutex.unlock();
    if (!firstBlockReady)
    {
        CloseBgmUnlocked(this);
        this->soundBufMutex.unlock();
        return ZUN_ERROR;
    }
    fileStream = nullptr; // owned by gBgmStream from here
    this->backgroundMusic.srcWav.data = reinterpret_cast<i16 *>(1);
    this->backgroundMusic.loopStart = 0;
    this->backgroundMusic.loopEnd = this->backgroundMusic.srcWav.samples;
    this->backgroundMusic.fadeoutLen = 0;
    this->backgroundMusic.fadeoutProgress = 0;
    this->backgroundMusic.pos = 0;
    this->soundBufMutex.unlock();

    heap = th06_psp_heap_snapshot();
    th06_psp_boot_notef("BGM OPEN %s %luKB C%luKB H%u/%uKB", path,
                        static_cast<unsigned long>(wavDataSize / 1024),
                        static_cast<unsigned long>(kBgmCacheBytes / 1024),
                        heap.usedBytes / 1024u, heap.freeBytes / 1024u);

    return ZUN_SUCCESS;

fail:
    th06_psp_boot_notef("BGM WAV INVALID %s", path);
    SDL_RWclose(fileStream);
    return ZUN_ERROR;
}

ZunResult SoundPlayer::LoadPos(const char *path)
{
    u8 *fileData;

    if (this->audioDev == 0 || g_Supervisor.cfg.playSounds == 0 || !BgmIsOpen())
    {
        return ZUN_ERROR;
    }

    fileData = FileSystem::OpenPath(path, 0);

    if (fileData == NULL)
    {
        return ZUN_ERROR;
    }

    u32 loopStart = SDL_SwapLE32(*((u32 *)fileData));
    u32 loopEnd = SDL_SwapLE32(*(u32 *)(fileData + 4));

    free(fileData);

    const bool valid = loopStart < loopEnd && loopEnd <= this->backgroundMusic.srcWav.samples;

    if (!valid)
    {
        loopStart = 0;
        loopEnd = this->backgroundMusic.srcWav.samples;
    }

    // The mixer thread is already streaming this BGM and reads (pos, loopStart,
    // loopEnd) as a set. Publishing them unlocked can hand it pos > loopEnd,
    // whose u32 subtraction underflows and reads past the end of the buffer.
    this->soundBufMutex.lock();
    this->backgroundMusic.loopStart = loopStart;
    this->backgroundMusic.loopEnd = loopEnd;
    if (this->backgroundMusic.pos >= loopEnd)
    {
        this->backgroundMusic.pos = loopStart;
    }
    this->soundBufMutex.unlock();

    return valid ? ZUN_SUCCESS : ZUN_ERROR;
}

ZunResult SoundPlayer::InitSoundBuffers()
{
    if (this->audioDev == 0)
    {
        return ZUN_ERROR;
    }

    std::fill_n(this->soundBuffersToPlay, ARRAY_SIZE(this->soundBuffersToPlay), -1);

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_SoundBufferIdxVol); idx++)
    {
        if (this->LoadSound(idx, g_SFXList[g_SoundBufferIdxVol[idx].bufferIdx],
                            1.0f / ZUN_POWF(10.0f, (float)g_SoundBufferIdxVol[idx].volume / -2000)) != ZUN_SUCCESS)
        {
            g_GameErrorContext.Log(TH_ERR_SOUNDPLAYER_FAILED_TO_LOAD_SOUND_FILE, g_SFXList[idx]);
            return ZUN_ERROR;
        }

        this->soundBuffers[idx].isPlaying = false;
        this->soundBuffers[idx].pos = 0;
    }

    return ZUN_SUCCESS;
}

ZunResult SoundPlayer::LoadSound(i32 idx, const char *path, f32 volumeMultiplier)
{
    SDL_AudioCVT sampleConversionDesc;
    SDL_AudioSpec wavFormat;
    u8 *wavRawData;
    u8 *wavRawSamples;
    u32 wavRawSampleByteCount;

    soundBufMutex.lock();

    if (this->soundBuffers[idx].samples != NULL)
    {
        delete[] this->soundBuffers[idx].samples;
        this->soundBuffers[idx].samples = NULL;
    }

    wavRawData = (u8 *)FileSystem::OpenPath(path, 0);

    if (wavRawData == NULL)
    {
        goto fail;
    }

    if (SDL_LoadWAV_RW(SDL_RWFromConstMem(wavRawData, g_LastFileSize), 1, &wavFormat, &wavRawSamples,
                       &wavRawSampleByteCount) == NULL)
    {
        g_GameErrorContext.Log(TH_ERR_NOT_A_WAV_FILE, path);
        goto fail;
    }

    // EoSD's sound files are all 22050 Hz, and some even use 8-bit samples. Converting them
    //   here only uses a few hundred extra kilobytes of RAM compared to the original code,
    //   but it might be worth looking into avoiding it for especially RAM-limited systems

    if (SDL_BuildAudioCVT(&sampleConversionDesc, wavFormat.format, wavFormat.channels, wavFormat.freq, AUDIO_S16SYS, 1,
                          44100) == 1)
    {
        sampleConversionDesc.len = wavRawSampleByteCount;
        sampleConversionDesc.buf = new u8[wavRawSampleByteCount * sampleConversionDesc.len_mult];
        std::memcpy(sampleConversionDesc.buf, wavRawSamples, wavRawSampleByteCount);

        SDL_ConvertAudio(&sampleConversionDesc);

        this->soundBuffers[idx].len = sampleConversionDesc.len_cvt / 2;
        this->soundBuffers[idx].samples = new i16[this->soundBuffers[idx].len];
        std::memcpy(this->soundBuffers[idx].samples, sampleConversionDesc.buf, sampleConversionDesc.len_cvt);

        delete[] sampleConversionDesc.buf;
    }
    else
    {
        this->soundBuffers[idx].len = wavRawSampleByteCount / 2;
        this->soundBuffers[idx].samples = new i16[this->soundBuffers[idx].len];
        std::memcpy(this->soundBuffers[idx].samples, wavRawSamples, wavRawSampleByteCount);
    }

    SDL_FreeWAV(wavRawSamples);

    for (u32 i = 0; i < this->soundBuffers[idx].len; i++)
    {
        this->soundBuffers[idx].samples[i] *= volumeMultiplier;
    }
#ifdef TH06_PSP
    // SFX data is immutable after load. Publish it once instead of flushing
    // every mix job, matching PSPPMD's producer/ME ownership rule.
    sceKernelDcacheWritebackRange(this->soundBuffers[idx].samples,
                                  this->soundBuffers[idx].len * sizeof(i16));
#endif

    this->soundBuffers[idx].pos = 0;
    this->soundBuffers[idx].isPlaying = false;

    soundBufMutex.unlock();
    return ZUN_SUCCESS;

fail:
    soundBufMutex.unlock();
    return ZUN_ERROR;
}

ZunResult SoundPlayer::PlayBGM(bool isLooping)
{
    utils::DebugPrint2("play BGM\n");

    if (!BgmIsOpen())
    {
        return ZUN_ERROR;
    }

    //    res = this->backgroundMusic->Reset();
    //    if (FAILED(res))
    //    {
    //        return ZUN_ERROR;
    //    }
    //
    //    buffer = this->backgroundMusic->GetBuffer(0);
    //    res = this->backgroundMusic->FillBufferWithSound(buffer, isLooping);
    //    if (FAILED(res))
    //    {
    //        return ZUN_ERROR;
    //    }
    //    res = this->backgroundMusic->Play(0, DSBPLAY_LOOPING);
    //    if (FAILED(res))
    //    {
    //        return ZUN_ERROR;
    //    }
    utils::DebugPrint2("comp\n");
    this->isLooping = isLooping;
    return ZUN_SUCCESS;
}

void SoundPlayer::PlaySounds()
{
    i32 idx;
    i32 sndBufIdx;
    u32 pendingMask = 0;

    if (this->audioDev == 0 || !g_Supervisor.cfg.playSounds)
    {
        return;
    }

    for (idx = 0; idx < ARRAY_SIZE_SIGNED(this->soundBuffersToPlay); idx++)
    {
        if (this->soundBuffersToPlay[idx] < 0)
        {
            break;
        }

        sndBufIdx = this->soundBuffersToPlay[idx];
        this->soundBuffersToPlay[idx] = -1;

        if (sndBufIdx >= 0 && sndBufIdx < 32)
            pendingMask |= 1u << sndBufIdx;
    }

    // The mixer owns SoundData state.  Do not make the game thread wait while
    // it is reading BGM data or mixing a catch-up block under soundBufMutex.
    // A bit set before or during exchange() is either consumed by this mix or
    // remains pending for the next one, so concurrent requests cannot be lost.
    if (pendingMask != 0)
        this->pendingSoundMask.fetch_or(pendingMask, std::memory_order_release);
}

void SoundPlayer::PlaySoundByIdx(SoundIdx idx)
{
    u32 i;

    for (i = 0; i < ARRAY_SIZE(this->soundBuffersToPlay); i++)
    {
        if (this->soundBuffersToPlay[i] < 0)
        {
            break;
        }

        if (this->soundBuffersToPlay[i] == idx)
        {
            return;
        }
    }

    if (i >= 3)
    {
#ifdef TH06_PS3
        s_AudSeDrop.fetch_add(1, std::memory_order_relaxed);
#endif
        return;
    }

    this->soundBuffersToPlay[i] = idx;
}

void SoundPlayer::MixAudio(u32 samples)
{
#ifdef TH06_PSP
    const u64 pspMixStartUs = sceKernelGetSystemTimeWide();
#endif
    // BackgroundMusicPlayerThread caps each pass at 1024 stereo frames.  Fixed
    // work buffers avoid two heap allocations on every 5 ms mixer iteration.
    samples = std::min(samples, kMaxMixSamples);
    samples &= ~1u;
    u8 playingChannels = 0;
#ifdef TH06_PSP
    Th06PspMixJob mixJob{};
    mixJob.frames = samples / 2;
    const auto addMixInput = [&mixJob](const i16 *source, u32 frames, u32 destinationFrame,
                                       u32 channels, f32 gain) {
        if (!source || frames == 0 || mixJob.inputCount >= TH06_PSP_ME_MAX_MIX_INPUTS)
            return;
        Th06PspMixInput &input = mixJob.inputs[mixJob.inputCount++];
        input.samples = source;
        input.frames = frames;
        input.destinationFrame = destinationFrame;
        input.channels = channels;
        input.gain = gain;
    };
#else
    std::memset(gFinalMix, 0, samples * sizeof(gFinalMix[0]));
    std::memset(gWideMix, 0, samples * sizeof(gWideMix[0]));
#endif

#ifdef TH06_PSP
    // MIDI BGM (psp/midi): synthesized on the ME, or on the SC as a fallback,
    // before taking soundBufMutex -- the player state belongs to this thread
    // and the game thread only queues commands.  It enters the same mix as the
    // WAV BGM: one stereo input at gain 1.0, counted as one playing channel.
    const short *midiPcm = th06_midi_render_for_mix(samples / 2);
#endif
#ifdef TH06_PS3
    const u64 audTReq = AudNowUs();
#endif
    this->soundBufMutex.lock();
#ifdef TH06_PS3
    const u64 audTLock = AudNowUs();
#endif

#ifdef TH06_PSP
    if (midiPcm != nullptr)
    {
        addMixInput(midiPcm, samples / 2, 0, 2, 1.0f);
        playingChannels++;
    }
#endif
#ifdef TH06_PS3
    // The PS3 MIDI parser and TinySoundFont synth are sample-clocked by this
    // existing worker. No SDL timer or third PPU thread is involved.
    if (g_Supervisor.cfg.musicMode == MIDI && g_Supervisor.midiOutput != nullptr &&
        g_Supervisor.midiOutput->RenderAudio(gWideMix, samples / 2))
    {
        playingChannels++;
    }
#endif

    const u32 pendingMask = this->pendingSoundMask.exchange(0, std::memory_order_acquire);
#ifdef TH06_PS3
    s_AudSeTrig += (u32)__builtin_popcount(pendingMask);
#endif
    for (u32 i = 0; i < 32; i++)
    {
        if ((pendingMask & (1u << i)) != 0 && this->soundBuffers[i].samples != NULL)
        {
            this->soundBuffers[i].pos = 0;
            this->soundBuffers[i].isPlaying = true;
        }
    }

    for (int i = 0; i < ARRAY_SIZE_SIGNED(this->soundBuffers); i++)
    {
        if (!this->soundBuffers[i].isPlaying)
        {
            continue;
        }

        playingChannels++;

        // Sounds are all mono, so ME/SC duplicates each source sample to L/R.
        const u32 samplesToMix = std::min(samples / 2, this->soundBuffers[i].len - this->soundBuffers[i].pos);

#ifdef TH06_PSP
        addMixInput(this->soundBuffers[i].samples + this->soundBuffers[i].pos, samplesToMix, 0, 1, 1.0f);
#else
        for (u32 j = 0; j < samplesToMix; j++)
        {
            gWideMix[j * 2] += this->soundBuffers[i].samples[this->soundBuffers[i].pos + j];
            gWideMix[j * 2 + 1] += this->soundBuffers[i].samples[this->soundBuffers[i].pos + j];
        }
#endif

        this->soundBuffers[i].pos += samplesToMix;

        if (this->soundBuffers[i].pos == this->soundBuffers[i].len)
        {
            this->soundBuffers[i].isPlaying = false;
        }
    }

#ifdef TH06_PS3
    const u64 audTSe = AudNowUs();
#endif

    if (BgmIsOpen())
    {
        u32 samplesMixed = 0;
        u32 protectedBgmBlocks = 0;
        f32 fadeoutMult;

        if (this->backgroundMusic.fadeoutLen != 0)
        {
            f32 fadeoutInterp =
                mapRange(this->backgroundMusic.fadeoutProgress, 0, this->backgroundMusic.fadeoutLen, 0, 5);
            fadeoutMult = 1.0f / ZUN_POWF(10.0f, fadeoutInterp / 2.0f);
        }
        else
        {
            fadeoutMult = 1.0f;
        }

        while (samplesMixed < samples / 2)
        {
            if (this->backgroundMusic.pos >= this->backgroundMusic.loopEnd)
            {
                if (this->isLooping)
                    this->backgroundMusic.pos = this->backgroundMusic.loopStart;
                else
                {
                    CloseBgmUnlocked(this);
                    break;
                }
            }

            const int blockIndex = AcquireBgmBlock(this->backgroundMusic.pos, protectedBgmBlocks);
            if (blockIndex < 0)
            {
#ifdef TH06_PSP
                __atomic_fetch_add(&gPspBgmCacheMissPasses, 1u, __ATOMIC_RELAXED);
                __atomic_fetch_add(&gPspBgmCacheMissFrames, (samples / 2) - samplesMixed,
                                   __ATOMIC_RELAXED);
#endif
                break;
            }
            const BgmCacheBlock &block = gBgmStream.blocks[blockIndex];
            const u32 blockOffset = this->backgroundMusic.pos - block.startFrame;
            const u32 blockFrames = block.frames - blockOffset;
            const u32 loopFrames = this->backgroundMusic.loopEnd - this->backgroundMusic.pos;
            const u32 samplesToMix = std::min((samples / 2) - samplesMixed, std::min(blockFrames, loopFrames));
            const i16 *bgm = gBgmStream.pcm[blockIndex] + static_cast<size_t>(blockOffset) * 2;
            // samplesMixed counts frames; a descriptor preserves that output
            // offset while avoiding all per-sample work on SC when ME is on.
#ifdef TH06_PSP
            addMixInput(bgm, samplesToMix, samplesMixed, 2, fadeoutMult);
            protectedBgmBlocks |= 1u << blockIndex;
#else
            i32 *out = gWideMix + static_cast<size_t>(samplesMixed) * 2;

            for (u32 j = 0; j < samplesToMix; j++)
            {
                out[j * 2] += bgm[j * 2] * fadeoutMult;
                out[j * 2 + 1] += bgm[j * 2 + 1] * fadeoutMult;
            }
#endif

            this->backgroundMusic.pos += samplesToMix;
            samplesMixed += samplesToMix;

            if (this->backgroundMusic.pos == this->backgroundMusic.loopEnd)
            {
                if (this->isLooping)
                {
                    this->backgroundMusic.pos = this->backgroundMusic.loopStart;
                }
                else
                {
                    CloseBgmUnlocked(this);
                    break;
                }
            }
            if (BgmIsOpen())
                MaintainBgmReadAhead(this, this->backgroundMusic.pos, protectedBgmBlocks);
        }

        if (this->backgroundMusic.fadeoutLen != 0 && BgmIsOpen())
        {
            this->backgroundMusic.fadeoutProgress += samplesMixed;

            if (this->backgroundMusic.fadeoutProgress >= this->backgroundMusic.fadeoutLen)
                CloseBgmUnlocked(this);
        }

        playingChannels++;
    }

#ifdef TH06_PS3
    const u64 audTBgm = AudNowUs();
#endif
#ifdef TH06_PSP
    // Keep source buffers protected until ME is done.  A timeout mixes the
    // same descriptors on SC into a separate output buffer before returning.
    // The original mixes DirectSound buffers at unity and clips at 16 bits; the
    // inherited max(8, channels) divisor made the PSP about 18 dB quieter (WAV and
    // MIDI alike).  Mix at unity; SC and ME saturate the 32-bit sum identically.
    mixJob.mixDivisor = 1;
    // MIDI has already completed one synchronous ME synthesis job above.
    // Do not claim/sleep on that worker a second time just to add unity SE:
    // reuse the existing SC work area (or copy the sole stereo MIDI input).
    // Non-unity/WAV jobs keep the original Q16 ME path unchanged. This changes
    // neither PCM lookahead nor the synth/event clock or any buffer lifetime.
    if (midiPcm == nullptr || !th06_psp_me_audio_mix_unity_nowait(&mixJob, gFinalMix))
        th06_psp_me_audio_mix(&mixJob, gFinalMix);
#endif
    this->soundBufMutex.unlock();
#ifdef TH06_PS3
    {
        const u32 waitUs = (u32)(audTLock - audTReq);
        const u32 holdUs = (u32)(audTBgm - audTLock);
        const u32 seUs = (u32)(audTSe - audTLock);
        const u32 bgmUs = (u32)(audTBgm - audTSe);
        if (waitUs > s_AudMaxWaitUs)
            s_AudMaxWaitUs = waitUs;
        if (holdUs > s_AudMaxHoldUs)
            s_AudMaxHoldUs = holdUs;
        if (seUs > s_AudMaxSeUs)
            s_AudMaxSeUs = seUs;
        if (bgmUs > s_AudMaxBgmUs)
            s_AudMaxBgmUs = bgmUs;
        s_AudSumHoldUs += holdUs;
        s_AudMixCalls++;
        if (playingChannels > s_AudMaxChans)
            s_AudMaxChans = playingChannels;
        if (samples / 2 > s_AudMaxTargetFrames)
            s_AudMaxTargetFrames = samples / 2;
    }
#endif

    // DirectSound supports playing from an arbitrary number of buffers at once, but that's kind of
    //   difficult to get right as it turns out. Instead we use 8 as an assumption of the
    //   max number of channels that could possibly be playing at once. If more channels end up in use,
    //   the input volume of each channel will start scaling down, which isn't correct, but would
    //   likely be imperceptible with that many channels anyway.

#ifndef TH06_PSP
    const int mixDivisor = std::max(8, (int)playingChannels);
    for (u32 i = 0; i < samples; i++)
    {
        // Integer division like this doesn't get optimized at all by the compiler. If it becomes
        //   a problem, it could be a good idea to convert to float, or to do the division as
        //   fixed point multiplication by the inverse of mixDivisor, depending on what's faster
        //   on any particular platform
        gFinalMix[i] = gWideMix[i] / mixDivisor;
    }
#endif

    SDL_QueueAudio(this->audioDev, gFinalMix, samples * 2);
#ifdef TH06_PSP
    RecordPspAudioMaximum(&gPspAudioMaxMixUs,
                          static_cast<u32>(sceKernelGetSystemTimeWide() - pspMixStartUs));
    __atomic_fetch_add(&gPspAudioMixCalls, 1, __ATOMIC_RELAXED);
#endif
}

#ifdef TH06_PSP
extern "C" void th06_psp_sound_diag_window(unsigned int *maxProducerGapUs, unsigned int *maxMixUs,
                                             unsigned int *maxBgmReadUs, unsigned int *mixCalls)
{
    if (maxProducerGapUs)
        *maxProducerGapUs = __atomic_exchange_n(&gPspAudioMaxProducerGapUs, 0, __ATOMIC_ACQ_REL);
    if (maxMixUs)
        *maxMixUs = __atomic_exchange_n(&gPspAudioMaxMixUs, 0, __ATOMIC_ACQ_REL);
    // BGM I/O owns a separate window so reads and cache misses are reset as
    // one coherent sample.  Keep this argument for source compatibility.
    if (maxBgmReadUs)
        *maxBgmReadUs = 0;
    if (mixCalls)
        *mixCalls = __atomic_exchange_n(&gPspAudioMixCalls, 0, __ATOMIC_ACQ_REL);
}

extern "C" void th06_psp_bgm_diag_window(unsigned int *reads, unsigned int *maxReadUs,
                                           unsigned int *missPasses, unsigned int *missFrames)
{
    if (reads)
        *reads = __atomic_exchange_n(&gPspBgmReadCount, 0, __ATOMIC_ACQ_REL);
    if (maxReadUs)
        *maxReadUs = __atomic_exchange_n(&gPspAudioMaxBgmReadUs, 0, __ATOMIC_ACQ_REL);
    if (missPasses)
        *missPasses = __atomic_exchange_n(&gPspBgmCacheMissPasses, 0, __ATOMIC_ACQ_REL);
    if (missFrames)
        *missFrames = __atomic_exchange_n(&gPspBgmCacheMissFrames, 0, __ATOMIC_ACQ_REL);
}
#endif

// EoSD originally just used this function to manage the streaming of the music WAV file.
//   We also use it to mix and queue audio, since we have to do that manually and doing it
//   in a thread keeps sound running continuously, even if the main thread runs into lag
void SoundPlayer::BackgroundMusicPlayerThread()
{
#ifdef TH06_PSP
    // FCR31 belongs to the thread context, and a new PSP thread starts with
    // invalid/divide-by-zero/overflow enabled.  Match the Windows default
    // before this worker performs fade and gain calculations.
    pspSdkDisableFPUExceptions();
#endif
#ifdef TH06_PS3
    // libstdc++ creates this worker at a general-purpose priority. During the
    // in-game XMB load, the process still runs but that worker can lose enough
    // scheduling time to starve cellAudio's ~37ms hardware queue. Give the
    // existing mixer/pump worker a small priority edge over the game thread.
    // This changes neither queue depth nor mixing/SE latency.
    sys_ppu_thread_t audioThreadId = 0;
    s32 oldPriority = -1;
    s32 newPriority = -1;
    s32 getIdRc = sysThreadGetId(&audioThreadId);
    s32 getPriorityRc =
        getIdRc == 0 ? sysThreadGetPriority(audioThreadId, &oldPriority) : getIdRc;
    s32 setPriorityRc = getPriorityRc;
    if (getPriorityRc == 0)
    {
        setPriorityRc =
            oldPriority > 999 ? sysThreadSetPriority(audioThreadId, 999) : 0;
        sysThreadGetPriority(audioThreadId, &newPriority);
    }
    th06_shikigami_u32(0x8160, (u32)getIdRc, (u32)getPriorityRc,
                       (u32)setPriorityRc, (u32)oldPriority, (u32)newPriority);
#endif

    SDL_PauseAudioDevice(this->audioDev, 0);

#ifdef TH06_PSP
    // The PSP DAC is the clock.  Feed a fixed software-ring watermark instead
    // of deriving production from a millisecond wall clock.  The old clock
    // path discarded elapsed time after a scheduler/I/O stall without moving
    // the BGM source cursor, permanently delaying the song, and normally kept
    // only ~1024 frames queued--one 23 ms scheduling miss from starvation.
    const int priorityRc = sceKernelChangeThreadPriority(sceKernelGetThreadId(), 0x18);
    const int actualPriority = sceKernelGetThreadCurrentPriority();
    // Completion polling relies on output/escape preempting the producer,
    // and on the producer outranking image decode. Verify that contract once
    // before producing any PCM; never put this logging in the runtime loop.
    th06_psp_boot_notef("AUDIO PRODUCER PRIORITY RC%d ACT%d EXPECT24", priorityRc, actualPriority);
    if (actualPriority != 0x18)
    {
        th06_psp_boot_note("AUDIO PRODUCER PRIORITY FAILED");
        th06_psp_request_exit(5);
        return;
    }
    constexpr u32 kTargetQueuedFrames = 3072; // 69.7 ms software cushion
    constexpr u32 kMaxFramesPerMix = kMaxMixSamples / 2;
    u64 lastProducerUs = 0;

    while (1)
    {
        const u64 nowUs = sceKernelGetSystemTimeWide();
        if (lastProducerUs != 0)
            RecordPspAudioMaximum(&gPspAudioMaxProducerGapUs, static_cast<u32>(nowUs - lastProducerUs));
        lastProducerUs = nowUs;

        if (this->terminateFlag)
            return;

        const u32 queuedFrames = SDL_GetQueuedAudioSize(this->audioDev) / BACKGROUND_MUSIC_WAV_BLOCK_ALIGN;
        if (queuedFrames < kTargetQueuedFrames)
        {
            const u32 frames = std::min(kMaxFramesPerMix, kTargetQueuedFrames - queuedFrames);
            this->MixAudio(frames * 2);
            continue;
        }

        // Two milliseconds is short relative to a 512-frame (11.6 ms) output
        // block and avoids burning the SC while the watermark is satisfied.
        SDL_Delay(2);
    }
#else
    // Bytes in the complete software+hardware queue. This must remain a fixed
    // ceiling: increasing it after every scheduler stall permanently turns
    // XMB pressure into SE latency (the real-hardware ring reached ~1.36s).
    const u32 latencyLimit = 14'700; // ~5 game frames
    u64 samplesSent = 0;
    u64 startTick = SDL_GetTicks64();
#ifdef TH06_PS3
    bool wasSuspended = false;
#endif
#ifdef TH06_PS3
    // cellAudio has 7 writable 256-frame blocks (~37ms). After XMB has
    // descheduled the process for hundreds of milliseconds, the generic
    // 1024-frame catch-up cap (~23ms) cannot refill even that hardware queue
    // in one pass and prolongs the audible breakup. Keep the normal latency
    // target unchanged, but permit a larger one-shot refill only after lag.
    const i32 catchupSampleLimit = 4096;
#else
    const i32 catchupSampleLimit = 1024;
#endif

    while (1)
    {
#ifdef TH06_PS3
        if (th06_audio_is_suspended())
        {
            if (this->terminateFlag)
                return;
            wasSuspended = true;
            th06_audio_wait_and_pump(5000);
            continue;
        }
        if (wasSuspended)
        {
            // Preserve the BGM source cursor while the game is stopped, and
            // restart wall-clock accounting from the resume instant so the
            // mixer does not attempt to catch up the paused interval.
            startTick = SDL_GetTicks64();
            samplesSent = 0;
            wasSuspended = false;
        }
#endif
        u64 curTicks = SDL_GetTicks64();

        // Keep slightly more than 1 frame's worth of samples in the audio buffer at all times
        i32 targetSamples = (curTicks - startTick) * 44.100 - samplesSent + 1024;

        // Quick and dirty checks to keep audio latency low
        //   Can probably be horribly broken, but I don't have weaker hardware to test on
        if (SDL_GetQueuedAudioSize(this->audioDev) > latencyLimit)
        {
            // Drop elapsed source time while the queued audio drains. Raising
            // the limit here lets repeated XMB stalls ratchet the ring all the
            // way to capacity, placing every new SE behind old BGM.
            samplesSent += targetSamples;
            targetSamples = 0;
        }
        else if (targetSamples > catchupSampleLimit)
        {
            samplesSent += targetSamples - catchupSampleLimit;
            targetSamples = catchupSampleLimit;
        }

        if (targetSamples > 0)
        {
            this->MixAudio(targetSamples * 2);
            samplesSent += targetSamples;
        }

        if (this->terminateFlag)
        {
            return;
        }

#ifdef TH06_PS3
        // Wake on the next 256-sample cellAudio block instead of polling.
        // The 10ms timeout keeps shutdown responsive if notifications stop.
        th06_audio_wait_and_pump(10 * 1000);

        // ==== SE stutter調査: 5秒窓の診断打電 (crumb+ttel、その後窓リセット) ====
        {
            static u64 lastEmitMs;
            static u32 lastUnderruns;
            const u64 nowMs = SDL_GetTicks64();
            if (lastEmitMs == 0)
                lastEmitMs = nowMs;
            if (nowMs - lastEmitMs >= 5000)
            {
                lastEmitMs = nowMs;
                u32 events, underruns, maxGapUs, ringFrames, busySkips;
                th06_audio_diag_snapshot(&events, &underruns, &maxGapUs, &ringFrames, &busySkips);
                const u32 underrunDelta = underruns - lastUnderruns;
                lastUnderruns = underruns;
                u32 minRing, qDrops;
                th06_audio_diag_window(&minRing, &qDrops);
                const u32 seDrop = s_AudSeDrop.exchange(0, std::memory_order_relaxed);
                const u32 avgHold = s_AudMixCalls ? s_AudSumHoldUs / s_AudMixCalls : 0;

                char m[24];
                snprintf(m, sizeof(m), "AH w%u h%u", s_AudMaxWaitUs, s_AudMaxHoldUs);
                th06_breadcrumb(m);
                snprintf(m, sizeof(m), "AB s%u b%u a%u", s_AudMaxSeUs, s_AudMaxBgmUs, avgHold);
                th06_breadcrumb(m);
                snprintf(m, sizeof(m), "AS t%u d%u c%u", s_AudSeTrig, seDrop, s_AudMaxChans);
                th06_breadcrumb(m);
                snprintf(m, sizeof(m), "AU u%u r%u q%u", underrunDelta, minRing, qDrops);
                th06_breadcrumb(m);
                th06_shikigami_u32(0x8164, s_AudMaxWaitUs, s_AudMaxHoldUs, s_AudMaxSeUs, s_AudMaxBgmUs, avgHold);
                th06_shikigami_u32(0x8165, s_AudSeTrig, seDrop, s_AudMaxChans, s_AudMaxTargetFrames, underrunDelta);
                th06_shikigami_u32(0x8166, minRing, qDrops, s_AudMixCalls, maxGapUs, busySkips);

                s_AudMaxWaitUs = s_AudMaxHoldUs = s_AudSumHoldUs = s_AudMixCalls = 0;
                s_AudMaxSeUs = s_AudMaxBgmUs = s_AudMaxChans = 0;
                s_AudSeTrig = s_AudMaxTargetFrames = 0;
            }
        }
#else
        SDL_Delay(5);
#endif
    }
#endif
}
