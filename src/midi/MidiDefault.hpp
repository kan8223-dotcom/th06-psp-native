#pragma once

#include "ZunResult.hpp"
#include "inttypes.hpp"

#ifdef TH06_PS3
struct tsf;
#endif

// The MIDI interface used if a specific platform MIDI API is not supported
// Obviously can't do much, but something needs to be linked

struct MidiDevice
{
  public:
    MidiDevice();
    ~MidiDevice();

    ZunResult Close();
    bool OpenDevice(u32 uDeviceId);
    bool SendShortMsg(u8 midiStatus, u8 firstByte, u8 secondByte);
    bool SendLongMsg(const u8 *buf, u32 len);
    bool Render(i16 *stereoBuffer, u32 frames);
    bool IsReady() const;

  private:
    bool printedWarning;
#ifdef TH06_PS3
    tsf *synth;
    u8 *soundFontData;
    u32 soundFontSize;
#endif
};
