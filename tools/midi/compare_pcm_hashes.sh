#!/bin/bash
# Compare all 3 song scripts and the self-test, at both rates, against the
# PC results created by run_pc_checks.sh. Frames and SHA-256 must match.
#   tools/midi/compare_pcm_hashes.sh PPSSPP.LOG PRIVATE_DIR
# PRIVATE_DIR may instead be provided explicitly through MIDI_FIXTURE_DIR.
set -euo pipefail
[ $# -ge 1 ] && [ $# -le 2 ] || { sed -n '2,5p' "$0"; exit 64; }
source "$(dirname "$0")/tool_paths.sh"
pcm_private=${2:-${MIDI_FIXTURE_DIR:-}}
[ -n "$pcm_private" ] || { echo 'PRIVATE_DIR or MIDI_FIXTURE_DIR is required (run_pc_checks.sh reference results).' >&2; exit 64; }
pcm_private=$(midi_external_directory "$pcm_private" PRIVATE_DIR)
pcm_temporary=$(midi_temp_directory th06_pcm_compare)
pcm_expected=$pcm_temporary/expected
pcm_observed=$pcm_temporary/observed
trap 'rm -f "$pcm_expected" "$pcm_observed"; rmdir -- "$pcm_temporary"' EXIT

for rate in 44100 22050; do
  awk -v rate="$rate" '
    $1 == "script" { print $2 "@" rate, $4, $6 }
    $1 == "selftest" { print "selftest@" rate, $3, $5 }
  ' "$pcm_private/th06psp_pc/scripts_$rate.txt"
done | sort > "$pcm_expected"

awk '
  /MIDI PCMTEST .*@/ {
    for (i = 1; i <= NF; ++i) {
      if ($i != "PCMTEST") continue
      name = $(i + 1)
      if ($(i + 2) == "OK") {
        frames = $(i + 3); hash = $(i + 4)
      } else if (name ~ /^selftest@/) {
        frames = $(i + 2); hash = $(i + 3)
      } else {
        print "FAILED", name
        next
      }
      sub(/^F/, "", frames)
      print name, frames, hash
      next
    }
  }
' "$1" | sort > "$pcm_observed"

[ "$(wc -l < "$pcm_expected")" -eq 8 ] || { echo 'FAIL: PC results must contain 8 hashes'; exit 1; }
[ "$(wc -l < "$pcm_observed")" -eq 8 ] || { echo 'FAIL: PSP log must contain 8 hashes'; exit 1; }
if ! cmp -s "$pcm_expected" "$pcm_observed"; then
  echo 'FAIL: PSP PCM frames or hashes differ from PC'
  diff -u "$pcm_expected" "$pcm_observed" || true
  exit 1
fi
echo 'PCM HASH MATCH: all 8 frame counts and SHA-256 hashes match PC'
