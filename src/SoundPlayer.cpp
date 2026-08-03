#include "SoundPlayer.hpp"

#include "ArcadeGcmGuard.hpp"
#include "FileSystem.hpp"
#include "Supervisor.hpp"
#include "i18n.hpp"
#include "utils.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_timer.h>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

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

    this->backgroundMusicThreadHandle = std::thread(&SoundPlayer::BackgroundMusicPlayerThread, this);

    g_GameErrorContext.Log(TH_DBG_SOUNDPLAYER_INIT_SUCCESS);
    return ZUN_SUCCESS;

fail:
    g_GameErrorContext.Log(TH_ERR_SOUNDPLAYER_FAILED_TO_INITIALIZE_OBJECT);
    return ZUN_ERROR;
}

ZunResult SoundPlayer::Release(void)
{
    this->terminateFlag = true;
    this->backgroundMusicThreadHandle.join();
    this->terminateFlag = false;

    StopBGM();

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
    if (this->backgroundMusic.srcWav.data != NULL)
    {
        this->soundBufMutex.lock();
        delete[] this->backgroundMusic.srcWav.data;
        this->backgroundMusic.srcWav.data = NULL;
        this->soundBufMutex.unlock();

        utils::DebugPrint2("stop BGM\n");
    }
}

void SoundPlayer::FadeOut(f32 seconds)
{
    if (this->backgroundMusic.srcWav.data != NULL)
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

    fileStream = SDL_RWFromFile(path, "r");

    if (fileStream == NULL)
    {
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

    // Preload the whole data chunk. One bulk read on the load screen replaces
    // 88,200 tiny reads/sec inside the mixer mutex (the measured stutter root).
    {
        const u32 sampleCount = wavDataSize / 2;
        i16 *bgmData = new (std::nothrow) i16[sampleCount];
        if (bgmData == NULL)
        {
            goto fail;
        }
        if (SDL_RWread(fileStream, bgmData, wavDataSize, 1) != 1)
        {
            delete[] bgmData;
            goto fail;
        }
        // WAV samples are little-endian; store native-endian for the mixer.
        for (u32 i = 0; i < sampleCount; i++)
        {
            bgmData[i] = (i16)SDL_SwapLE16((u16)bgmData[i]);
        }
        SDL_RWclose(fileStream);
        fileStream = NULL;

        // The mixer only touches BGM state under soundBufMutex; publish under it.
        this->soundBufMutex.lock();
        this->backgroundMusic.srcWav.data = bgmData;
        this->backgroundMusic.loopStart = 0;
        this->backgroundMusic.loopEnd = this->backgroundMusic.srcWav.samples;
        this->backgroundMusic.fadeoutLen = 0;
        this->backgroundMusic.fadeoutProgress = 0;
        this->backgroundMusic.pos = 0;
        this->soundBufMutex.unlock();
    }

    return ZUN_SUCCESS;

fail:
    SDL_RWclose(fileStream);
    return ZUN_ERROR;
}

ZunResult SoundPlayer::LoadPos(const char *path)
{
    u8 *fileData;

    if (this->audioDev == 0 || g_Supervisor.cfg.playSounds == 0 || backgroundMusic.srcWav.data == NULL)
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

    if (this->backgroundMusic.srcWav.data == NULL)
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
    std::vector<i16> finalBuffer(samples);
    std::vector<i32> mixBuffer(samples);
    u8 playingChannels = 0;

#ifdef TH06_PS3
    const u64 audTReq = AudNowUs();
#endif
    this->soundBufMutex.lock();
#ifdef TH06_PS3
    const u64 audTLock = AudNowUs();
#endif

#ifdef TH06_PS3
    // The PS3 MIDI parser and TinySoundFont synth are sample-clocked by this
    // existing worker. No SDL timer or third PPU thread is involved.
    if (g_Supervisor.cfg.musicMode == MIDI && g_Supervisor.midiOutput != nullptr &&
        g_Supervisor.midiOutput->RenderAudio(mixBuffer.data(), samples / 2))
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

        // Sounds are all mono, so we need to duplicate each sample for stereo output
        const u32 samplesToMix = std::min(samples / 2, this->soundBuffers[i].len - this->soundBuffers[i].pos);

        for (u32 j = 0; j < samplesToMix; j++)
        {
            mixBuffer[j * 2] += this->soundBuffers[i].samples[this->soundBuffers[i].pos + j];
            mixBuffer[j * 2 + 1] += this->soundBuffers[i].samples[this->soundBuffers[i].pos + j];
        }

        this->soundBuffers[i].pos += samplesToMix;

        if (this->soundBuffers[i].pos == this->soundBuffers[i].len)
        {
            this->soundBuffers[i].isPlaying = false;
        }
    }

#ifdef TH06_PS3
    const u64 audTSe = AudNowUs();
#endif

    if (this->backgroundMusic.srcWav.data != NULL)
    {
        u32 samplesMixed = 0;
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
            const u32 samplesToMix =
                std::min((samples / 2) - samplesMixed, this->backgroundMusic.loopEnd - this->backgroundMusic.pos);
            const i16 *bgm = this->backgroundMusic.srcWav.data + (size_t)this->backgroundMusic.pos * 2;
            // samplesMixed counts frames, mixBuffer is interleaved: frame N starts at 2*N.
            i32 *out = mixBuffer.data() + (size_t)samplesMixed * 2;

            for (u32 j = 0; j < samplesToMix; j++)
            {
                out[j * 2] += bgm[j * 2] * fadeoutMult;
                out[j * 2 + 1] += bgm[j * 2 + 1] * fadeoutMult;
            }

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
                    delete[] this->backgroundMusic.srcWav.data;
                    this->backgroundMusic.srcWav.data = NULL;

                    break;
                }
            }
        }

        if (this->backgroundMusic.fadeoutLen != 0 && this->backgroundMusic.srcWav.data != NULL)
        {
            this->backgroundMusic.fadeoutProgress += samplesMixed;

            if (this->backgroundMusic.fadeoutProgress >= this->backgroundMusic.fadeoutLen)
            {
                delete[] this->backgroundMusic.srcWav.data;
                this->backgroundMusic.srcWav.data = NULL;
            }
        }

        playingChannels++;
    }

#ifdef TH06_PS3
    const u64 audTBgm = AudNowUs();
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

    const int mixDivisor = std::max(8, (int)playingChannels);

    for (u32 i = 0; i < samples; i++)
    {
        // Integer division like this doesn't get optimized at all by the compiler. If it becomes
        //   a problem, it could be a good idea to convert to float, or to do the division as
        //   fixed point multiplication by the inverse of mixDivisor, depending on what's faster
        //   on any particular platform
        finalBuffer[i] = mixBuffer[i] / mixDivisor;
    }

    SDL_QueueAudio(this->audioDev, finalBuffer.data(), samples * 2);
}

// EoSD originally just used this function to manage the streaming of the music WAV file.
//   We also use it to mix and queue audio, since we have to do that manually and doing it
//   in a thread keeps sound running continuously, even if the main thread runs into lag
void SoundPlayer::BackgroundMusicPlayerThread()
{
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
}
