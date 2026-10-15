// TH06 PSP: fixed command schedules for checking the synthesizer.
//
//  - The boot self-test: a synthetic SMF (built here, no game data) played by
//    the SC player and by the ME player through the same schedule; the two PCM
//    streams must be identical byte for byte (psp/midi/MidiPsp.cpp).  The PC
//    harness runs the same schedule, so the PSP log hash can also be compared
//    with the PC.
//  - The PCM scripts (test EBOOT `make MIDI_PCMTEST=1` and the PC harness):
//    game songs with fade/stop at fixed times.  Each script mirrors one
//    dls_render_tool command line, so its hash must equal the PC tool's.
//
// The timing rules are those of dls_render_tool: commands are pushed at the
// frame floor(ms * rate / 1000) and render calls never cross that frame, and
// each render call is at most `chunk` frames.
#pragma once

#include "MidiSettings.hpp"
#include "tdls/sha256.hpp"
#include "tdls/zun_seq.hpp"

namespace th06midi
{

using tdls::u32;
using tdls::u8;

struct ScheduleEvent
{
    u32 ms;   // from the start of the schedule
    u32 type; // tdls::ZunCommandType
    u32 arg;
};

struct Schedule
{
    u32 totalMs;
    u32 chunkFrames;
    const ScheduleEvent *events; // sorted by ms; the PLAY at ms 0 is pushed by the runner
    u32 eventCount;
};

// What a schedule drives: one player (SC object or ME job).
class ScheduleSink
{
  public:
    virtual ~ScheduleSink() {}
    virtual bool Push(const tdls::ZunCommand &command) = 0;
    virtual bool Render(tdls::s16 *out, u32 frames) = 0;
};

// Plays `smf` through the schedule, hashing the little-endian PCM.  pcm points
// to a caller buffer of chunkFrames * 2 samples.  Returns the frame count.
bool RunSchedule(const Schedule &schedule, const u8 *smf, u32 smfBytes, u32 sampleRate, ScheduleSink &sink,
                 tdls::s16 *pcm, tdls::Sha256 &hash, u32 *frames, const char **error);

// The self-test SMF (format 0, one track, ~0.4 s of dense GS writing: reset,
// programs, drums, bend, modulation, pan, expression, sustain).  Returns its
// size; cap >= kSelfTestSmfMaxBytes.
constexpr u32 kSelfTestSmfMaxBytes = 2048;
u32 BuildSelfTestSmf(u8 *out, u32 cap);
extern const Schedule kSelfTestSchedule;

// ---- PCM scripts (test builds and the PC harness only)
struct Script
{
    const char *name;
    const char *song; // base name inside MD.DAT, e.g. "th06_02.mid"
    Schedule schedule;
    // dls_render_tool arguments for the song and timing; the synthesizer
    // settings (rule, voices, reserve, gain, rate) are appended by the caller.
    const char *pcCommand;
};

extern const Script kScripts[];
extern const u32 kScriptCount;

struct ScriptHost
{
    const u8 *blob;
    u32 blobBytes;
    bool (*findSong)(void *user, const char *name, const u8 **data, u32 *size);
    void *user;
};

struct ScriptResult
{
    u32 frames;
    u8 pcmSha256[32];
    u32 noteOns;
    u32 notesLost;
    u32 maxActive;
    const char *error;
};

// Runs one script on a fresh SC synthesizer with `settings` at the synthesis
// rate (no upsampling), i.e. what dls_render_tool renders with the same
// --rule/--voices/--reserve/--gain-adjust/--master-q16/--rate options.
bool RunScript(const Script &script, const ScriptHost &host, const Settings &settings, ScriptResult &out);
// The boot self-test schedule as the game runs it: 44.1 kHz output (22,050 Hz
// synthesis is upsampled with RenderUpsampled2x).
bool RunSelfTestScript(const ScriptHost &host, const Settings &settings, ScriptResult &out);

} // namespace th06midi
