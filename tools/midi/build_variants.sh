#!/bin/bash
# Build the EBOOT variants used by the MIDI checks into OUTPUT_DIR (outside
# the repo; defaults to a fresh TMPDIR directory). Each is a clean build: make
# not track -D flag changes.  The last build is the normal product EBOOT.
#   tools/midi/build_variants.sh [variant ...]   (default: PCMTEST MEMSIM REPLAY_TRACE PRODUCT)
# Optional: PIXEL_HASH (REPLAY_TRACE + RENDER_HASH_TEST), RECORD, RECORD_MEMSIM.
# RECORD is the MIDI-only unattended hardware harness (one 24-point pass);
# RECORD_MEMSIM follows the same route silently for PPSSPP 32MB checks.
# Existing WAV baselines are reused. Do not rerun WAV for MIDI-only changes.
# Finish with PRODUCT so no automatic-test code remains in the normal EBOOT.
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
source "$here/tools/midi/tool_paths.sh"
cd "$here"
if [ -n "${OUTPUT_DIR:-}" ]; then
  OUT=$(midi_external_directory "$OUTPUT_DIR" OUTPUT_DIR)
else
  OUT=$(midi_temp_directory th06_midi_variants)
fi
mkdir -p "$OUT"
printf 'VARIANT OUTPUT %s\n' "$OUT"
variants=${*:-"PCMTEST MEMSIM REPLAY_TRACE PRODUCT"}
for v in $variants; do
  make clean > /dev/null 2>&1 || true
  case $v in
    PCMTEST) make -j16 MIDI_PCMTEST=1 > "$OUT/build_$v.log" 2>&1 ;;
    MEMSIM) make -j16 MIDI_MEMSIM=1 REPLAY_TRACE=1 > "$OUT/build_$v.log" 2>&1 ;;
    RECORD) make -j16 RECORD_SOAK=1 RECORD_START_MIDI=1 > "$OUT/build_$v.log" 2>&1 ;;
    RECORD_MEMSIM) make -j16 RECORD_SOAK=1 RECORD_START_MIDI=1 MIDI_MEMSIM=1 > "$OUT/build_$v.log" 2>&1 ;;
    REPLAY_TRACE) make -j16 REPLAY_TRACE=1 > "$OUT/build_$v.log" 2>&1 ;;
    PIXEL_HASH) make -j16 REPLAY_TRACE=1 RENDER_HASH_TEST=1 > "$OUT/build_$v.log" 2>&1 ;;
    PRODUCT) make -j16 > "$OUT/build_$v.log" 2>&1 ;;
    *) echo "unknown variant $v"; exit 64 ;;
  esac
  cp EBOOT.PBP "$OUT/EBOOT_$v.PBP"
  printf '%-13s %9s %s\n' "$v" "$(stat -c %s "$OUT/EBOOT_$v.PBP")" "$(sha256sum "$OUT/EBOOT_$v.PBP" | cut -c1-16)"
done
