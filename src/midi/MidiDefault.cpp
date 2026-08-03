#include "MidiDefault.hpp"
#include "GameErrorContext.hpp"
#include "i18n.hpp"

#include <cstring>

#ifdef TH06_PS3
#include <cstdlib>
#include <cstdio>
#define TSF_IMPLEMENTATION
#include "thirdparty/tsf.h"

#ifndef TH06_APPID
#define TH06_APPID "TH06PS300"
#endif
#define TH06_MIDI_SF2_PATH "/dev_hdd0/game/" TH06_APPID "/USRDIR/TimGM6mb.sf2"
#define TH06_MIDI_SF2_SIZE 5969788u

extern "C" void th06_breadcrumb(const char *msg);

static u32 ReadSf2Le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool PrepareSoundFontPcmForPs3(u8 *data, u32 size)
{
    if (data == nullptr || size < 12 || std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "sfbk", 4) != 0)
        return false;

    // SF2 stores its PCM16 sample pool in little-endian order.  The PMDVIS
    // TinySoundFont port keeps a zero-copy pointer to that pool, so convert it
    // once in place instead of byte-swapping every sample in the audio loop.
    u32 top = 12;
    while (top + 8 <= size)
    {
        u32 chunkSize = ReadSf2Le32(data + top + 4);
        u64 chunkEnd64 = (u64)top + 8u + chunkSize;
        if (chunkEnd64 > size)
            return false;
        u32 chunkEnd = (u32)chunkEnd64;

        if (std::memcmp(data + top, "LIST", 4) == 0 && chunkSize >= 4 &&
            std::memcmp(data + top + 8, "sdta", 4) == 0)
        {
            u32 child = top + 12;
            while (child + 8 <= chunkEnd)
            {
                u32 childSize = ReadSf2Le32(data + child + 4);
                u64 childEnd64 = (u64)child + 8u + childSize;
                if (childEnd64 > chunkEnd)
                    return false;
                if (std::memcmp(data + child, "smpl", 4) == 0)
                {
                    if ((childSize & 1u) != 0)
                        return false;
                    u8 *pcm = data + child + 8;
                    for (u32 i = 0; i < childSize; i += 2)
                    {
                        u8 lo = pcm[i];
                        pcm[i] = pcm[i + 1];
                        pcm[i + 1] = lo;
                    }
                    return true;
                }
                child = (u32)((childEnd64 + 1u) & ~1ULL);
            }
            return false;
        }
        top = (u32)((chunkEnd64 + 1u) & ~1ULL);
    }
    return false;
}
#endif

MidiDevice::MidiDevice()
#ifdef TH06_PS3
    : synth(nullptr), soundFontData(nullptr), soundFontSize(0)
#endif
{
    printedWarning = false;
}

MidiDevice::~MidiDevice()
{
#ifdef TH06_PS3
    if (this->synth != nullptr)
    {
        tsf_close(this->synth);
        this->synth = nullptr;
    }
    std::free(this->soundFontData);
    this->soundFontData = nullptr;
    this->soundFontSize = 0;
#endif
}

bool MidiDevice::OpenDevice(u32 uDeviceId)
{
    (void)uDeviceId;

#ifdef TH06_PS3
    if (this->synth == nullptr)
    {
        FILE *sf2 = std::fopen(TH06_MIDI_SF2_PATH, "rb");
        if (sf2 == nullptr)
        {
            if (!this->printedWarning)
            {
                printf("[th06_midi] SoundFont open failed: %s\n", TH06_MIDI_SF2_PATH);
                th06_breadcrumb("MIDI SF2 OPEN NG");
                this->printedWarning = true;
            }
            return false;
        }

        std::fseek(sf2, 0, SEEK_END);
        long fileSize = std::ftell(sf2);
        std::fseek(sf2, 0, SEEK_SET);
        if (fileSize != (long)TH06_MIDI_SF2_SIZE)
        {
            std::fclose(sf2);
            if (!this->printedWarning)
            {
                printf("[th06_midi] SoundFont size mismatch: %ld\n", fileSize);
                th06_breadcrumb("MIDI SF2 SIZE NG");
                this->printedWarning = true;
            }
            return false;
        }

        this->soundFontData = (u8 *)std::malloc((size_t)fileSize);
        if (this->soundFontData == nullptr)
        {
            std::fclose(sf2);
            th06_breadcrumb("MIDI SF2 MALLOC NG");
            return false;
        }
        size_t bytesRead = std::fread(this->soundFontData, 1, (size_t)fileSize, sf2);
        std::fclose(sf2);
        if (bytesRead != (size_t)fileSize)
        {
            std::free(this->soundFontData);
            this->soundFontData = nullptr;
            th06_breadcrumb("MIDI SF2 READ NG");
            return false;
        }
        this->soundFontSize = (u32)fileSize;
        if (!PrepareSoundFontPcmForPs3(this->soundFontData, this->soundFontSize))
        {
            std::free(this->soundFontData);
            this->soundFontData = nullptr;
            this->soundFontSize = 0;
            th06_breadcrumb("MIDI SF2 PCM NG");
            return false;
        }

        // The PMDVIS PS3 TinySoundFont port keeps sample data as a zero-copy
        // pointer into the caller's memory stream.  tsf_load_filename() uses a
        // FILE stream and is incompatible with that port, so retain this
        // complete SF2 image for the lifetime of the synthesizer.
        this->synth = tsf_load_memory(this->soundFontData, (int)this->soundFontSize);
        if (this->synth == nullptr)
        {
            char msg[32];
            snprintf(msg, sizeof(msg), "MIDI SF2 PARSE NG %d", tsf_dbg_error);
            th06_breadcrumb(msg);
            std::free(this->soundFontData);
            this->soundFontData = nullptr;
            this->soundFontSize = 0;
            if (!this->printedWarning)
            {
                printf("[th06_midi] SoundFont parse failed: %s (error %d)\n",
                       TH06_MIDI_SF2_PATH, tsf_dbg_error);
                this->printedWarning = true;
            }
            return false;
        }
        if (!tsf_set_max_voices(this->synth, 96))
        {
            tsf_close(this->synth);
            this->synth = nullptr;
            std::free(this->soundFontData);
            this->soundFontData = nullptr;
            this->soundFontSize = 0;
            th06_breadcrumb("MIDI VOICES NG");
            return false;
        }
        printf("[th06_midi] TinySoundFont ready: %s\n", TH06_MIDI_SF2_PATH);
        th06_breadcrumb("MIDI SF2 OK");
    }

    tsf_reset(this->synth);
    // SoundPlayer applies the same /8 mix scale as WAV BGM, which gives this
    // 0 dB synth output roughly the -18 dB final level proven by PMDVIS.
    tsf_set_output(this->synth, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
    tsf_channel_set_bank_preset(this->synth, 9, 128, 0);
    return true;
#else
    if (!printedWarning)
    {
        g_GameErrorContext.Log(TH_ERR_NO_MIDI_SUPPORT);
        printedWarning = true;
    }

    return true;
#endif
}

ZunResult MidiDevice::Close()
{
#ifdef TH06_PS3
    if (this->synth != nullptr)
    {
        tsf_reset(this->synth);
        tsf_set_output(this->synth, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
        tsf_channel_set_bank_preset(this->synth, 9, 128, 0);
    }
#endif
    return ZUN_SUCCESS;
}

bool MidiDevice::SendLongMsg(const u8 *buf, u32 len)
{
#ifdef TH06_PS3
    if (this->synth == nullptr)
        return false;

    // TH06 sends a reset SysEx at the start of its sequences. TinySoundFont
    // does not parse SysEx, so treat a complete SysEx as the intended reset.
    if (buf != nullptr && len >= 2 && buf[0] == 0xf0)
    {
        tsf_reset(this->synth);
        tsf_set_output(this->synth, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
        tsf_channel_set_bank_preset(this->synth, 9, 128, 0);
    }
    return true;
#else
    (void)buf;
    (void)len;

    return true;
#endif
}

bool MidiDevice::SendShortMsg(u8 midiStatus, u8 firstByte, u8 secondByte)
{
#ifdef TH06_PS3
    if (this->synth == nullptr)
        return false;

    const int channel = midiStatus & 0x0f;
    switch (midiStatus & 0xf0)
    {
    case 0x80:
        tsf_channel_note_off(this->synth, channel, firstByte & 0x7f);
        break;
    case 0x90:
        if ((secondByte & 0x7f) == 0)
            tsf_channel_note_off(this->synth, channel, firstByte & 0x7f);
        else
            tsf_channel_note_on(this->synth, channel, firstByte & 0x7f,
                                (secondByte & 0x7f) / 127.0f);
        break;
    case 0xb0:
        tsf_channel_midi_control(this->synth, channel, firstByte & 0x7f, secondByte & 0x7f);
        break;
    case 0xc0:
        tsf_channel_set_presetnumber(this->synth, channel, firstByte & 0x7f, channel == 9);
        break;
    case 0xe0:
        tsf_channel_set_pitchwheel(this->synth, channel,
                                   ((secondByte & 0x7f) << 7) | (firstByte & 0x7f));
        break;
    default:
        break;
    }
    return true;
#else
    (void)midiStatus;
    (void)firstByte;
    (void)secondByte;

    return true;
#endif
}

bool MidiDevice::Render(i16 *stereoBuffer, u32 frames)
{
#ifdef TH06_PS3
    if (this->synth == nullptr || stereoBuffer == nullptr || frames == 0)
        return false;
    tsf_render_short(this->synth, stereoBuffer, (int)frames, 0);
    return true;
#else
    if (stereoBuffer != nullptr)
        std::memset(stereoBuffer, 0, frames * 2 * sizeof(*stereoBuffer));
    return false;
#endif
}

bool MidiDevice::IsReady() const
{
#ifdef TH06_PS3
    return this->synth != nullptr;
#else
    return false;
#endif
}
