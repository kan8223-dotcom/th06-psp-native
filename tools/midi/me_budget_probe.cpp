// Compile only; never link this object into an EBOOT. Including the actual
// ME translation unit makes these sizes track its private state layout.
#include "../../psp/midi/MidiMe.cpp"

extern "C" {
unsigned char th06_midi_budget_state_offset[kArenaOffset];
unsigned char th06_midi_budget_arena[TH06_MIDI_EDRAM_STATE_BYTES - kArenaOffset];
unsigned char th06_midi_budget_art[sizeof(tdls::SynthArt)];
unsigned char th06_midi_budget_wave[sizeof(tdls::SynthWave)];
unsigned char th06_midi_budget_region[sizeof(tdls::SynthRegion)];
unsigned char th06_midi_budget_instrument[sizeof(tdls::SynthInstrument)];
unsigned char th06_midi_budget_map[sizeof(tdls::SynthMapEntry)];
unsigned char th06_midi_budget_voice[sizeof(tdls::SynthVoice)];
}
