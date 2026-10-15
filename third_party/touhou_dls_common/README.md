# touhou_dls_common

Shared integer DLS synthesizer and Touhou MIDI sequencer used by this port.
This source is included under GNU GPL version 3, with the project owner's
permission. See LICENSE in this directory. The published files contain code
and mathematical tables, not instrument waveforms or downloaded instrument
parameters.

The PSP Makefile builds these sources directly; no separate download or
machine-specific home directory is required. TDLS_DIR can override this copy.
The PSP port's settings determine timing and rendering: its default uses
22,050 Hz synthesis, repeated samples for 44,100 Hz output, 32 playable voices
plus eight reserve slots, and event quantization disabled (Q0).

Instrument and song inputs must be supplied by the user. gm.dls, extracted
MIDI, rendered WAV, instrument dumps and derived caches must not be committed
or distributed with this source. The PSP creates its own cache on the user's
Memory Stick.

src/synth_tables.cpp contains numerical tables generated from the formulas
documented in tools/gen_synth_tables.py. The generator does not read gm.dls,
a Windows synth binary, or extracted instrument parameters.

MSGS-compatible behavior is an implementation goal, not a claim that complete
songs are bit-identical to Windows MSGS. Runtime performance, audio supply and
PSP ME execution require separate hardware testing.
