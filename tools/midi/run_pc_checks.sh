#!/bin/bash
# PC checks for psp/midi (never touches the PSP or the repository's data).
#   tools/midi/run_pc_checks.sh PRIVATE_DIR   (or set MIDI_FIXTURE_DIR)
# Inputs : $P/gm/gm.dls, $P/midi/th06/th06_*.mid (extracted from 紅魔郷MD.DAT),
#          $P/out/summary.txt and $P/out_blobs/ (independently generated references)
# Reference fixtures are not distributed. This script never creates expected
# hashes from its own candidate output: missing references are an error.
# TDLS_DIR defaults to third_party/touhou_dls_common; CXX selects the host compiler.
# Outputs: $P/th06psp_pc/ only (gm.dls derivatives stay private).
#  1. psp/midi/MidiBuild.cpp builds the rule A and rule C blobs with the same
#     subset-list and blob hashes as dls_subset_tool (out/summary.txt).
#  2. Every PCM script in psp/midi/MidiScript.cpp, at the game's default
#     settings and at 22,050 Hz, hashes the same as the dls_render_tool command
#     it mirrors.
#  3. The boot self-test schedule's PCM hash (the PSP log must show the same).
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -le 1 ] || { sed -n '2,10p' "$0" >&2; exit 64; }
source "$here/tools/midi/tool_paths.sh"
P=${1:-${MIDI_FIXTURE_DIR:-}}
[ -n "$P" ] || { echo 'PRIVATE_DIR or MIDI_FIXTURE_DIR is required; provide your own data and independent reference fixtures.' >&2; exit 64; }
P=$(midi_external_directory "$P" PRIVATE_DIR)
TDLS=${TDLS_DIR:-$here/third_party/touhou_dls_common}
OUT=$(midi_external_directory "$P/th06psp_pc" 'PC output directory')
for input in "$P/gm/gm.dls" "$P/out/summary.txt" \
             "$P/out_blobs/blob_th06_A_capital_f0.tdsb" "$P/out_blobs/blob_th06_C_strict_lsb_f0.tdsb"; do
  [ -f "$input" ] || { printf 'missing private reference input: %s\nSee the input layout at the top of %s.\n' "$input" "$0" >&2; exit 2; }
done
[ -d "$P/midi/th06" ] || { echo "missing private MIDI directory: $P/midi/th06" >&2; exit 2; }
[ -f "$TDLS/tools/dls_render_tool.cpp" ] && [ -f "$TDLS/src/synth.cpp" ] || {
  echo "missing common host sources at $TDLS; set TDLS_DIR to the complete source directory" >&2; exit 2;
}
mkdir -p "$OUT"
BIN=$OUT/midi_pc_check
HOST_CXX=${CXX:-c++}
# Compile both actual tools outside the repository, including with the bundled
# common sources. No optional/private common Makefile test targets are required.
"$HOST_CXX" -std=c++17 -O2 -Wall -Wextra -I"$TDLS/include" \
    "$TDLS/tools/dls_render_tool.cpp" "$TDLS"/src/*.cpp -o "$OUT/dls_render_tool"
"$HOST_CXX" -std=c++17 -O2 -Wall -Wextra -I"$TDLS/include" -I"$here/psp/midi" \
    "$here/tools/midi/midi_pc_check.cpp" "$here/psp/midi/MidiBuild.cpp" "$here/psp/midi/MidiScript.cpp" \
    "$TDLS"/src/*.cpp -o "$BIN"
fail=0
ng(){ echo "NG: $*"; fail=1; }

for rule in A:A_capital C:C_strict_lsb; do
  r=${rule%%:*}; long=${rule#*:}
  echo "== 1$r. blob build (rule $r) vs dls_subset_tool"
  "$BIN" build --rule "$r" --dls "$P/gm/gm.dls" --midi "$P/midi/th06" > "$OUT/build_$r.txt"
  grep -E '^(usage|manifest|blob flags=0)' "$OUT/build_$r.txt"
  want_manifest=$(awk -v l="$long" '$1=="th06" && $2==l && $3 ~ /^manifest_sha256=/ {sub("manifest_sha256=","",$3); print $3}' "$P/out/summary.txt")
  want_blob=$(awk -v l="$long" '$1=="th06" && $2==l && $3=="flags=0" {sub("sha256=","",$5); print $5}' "$P/out/summary.txt")
  got_manifest=$(awk '$1=="manifest_sha256" {print $2}' "$OUT/build_$r.txt")
  got_blob=$(awk '$1=="blob" && $2=="flags=0" {sub("sha256=","",$4); print $4}' "$OUT/build_$r.txt")
  [ -n "$want_manifest" ] && [ "$got_manifest" = "$want_manifest" ] && echo "rule $r list MATCH $got_manifest" || ng "rule $r list $got_manifest != $want_manifest"
  [ -n "$want_blob" ] && [ "$got_blob" = "$want_blob" ] && echo "rule $r blob MATCH $got_blob" || ng "rule $r blob $got_blob != $want_blob"
done

BLOB=$P/out_blobs/blob_th06_C_strict_lsb_f0.tdsb
for rate in 44100 22050; do
  echo "== 2. PCM scripts vs dls_render_tool (game defaults, rate $rate)"
  "$BIN" script --blob "$BLOB" --midi "$P/midi/th06" --rate "$rate" > "$OUT/scripts_$rate.txt"
  grep -E '^(settings|script|selftest)' "$OUT/scripts_$rate.txt"
  while read -r _ name args; do
    read -r -a render_args <<< "$args"
    for ((i=0; i<${#render_args[@]}; ++i)); do
      if [ "${render_args[$i]}" = --mid ]; then
        i=$((i + 1))
        render_args[$i]="$P/midi/th06/${render_args[$i]}"
      fi
    done
    want=$("$OUT/dls_render_tool" render --blob "$BLOB" "${render_args[@]}" \
            | awk '$1=="pcm_sha256" {print $2}')
    got=$(awk -v n="$name" '$1=="script" && $2==n {print $6}' "$OUT/scripts_$rate.txt")
    [ -n "$want" ] && [ "$got" = "$want" ] && echo "$name@$rate MATCH $got" || ng "$name@$rate $got != $want"
  done < <(grep '^pccmd ' "$OUT/scripts_$rate.txt")
done

echo "== 3. self-test schedule hash (rule A for comparison)"
"$BIN" selftest --blob "$P/out_blobs/blob_th06_A_capital_f0.tdsb" --rule A | grep '^selftest'

if [ $fail = 0 ]; then echo "ALL OK"; else echo "NG"; exit 1; fi
