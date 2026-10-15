#!/bin/bash
# Compile the real ME state layout for PSP, then apply those ABI sizes to
# Synth::memoryNeeded using a private blob. No PSP/ME execution is implied.
#   tools/midi/me_budget.sh PRIVATE_BLOB [voices] [PRIVATE_DIR]
# PRIVATE_DIR (or MIDI_FIXTURE_DIR) must contain run_pc_checks.sh results.
# Temporary objects stay under TMPDIR, outside the repository.
set -euo pipefail
[ $# -ge 1 ] && [ $# -le 3 ] || { sed -n '2,5p' "$0"; exit 64; }
here=$(cd "$(dirname "$0")/../.." && pwd)
source "$here/tools/midi/tool_paths.sh"
tdls_dir=${TDLS_DIR:-$here/third_party/touhou_dls_common}
budget_private=${3:-${MIDI_FIXTURE_DIR:-}}
[ -n "$budget_private" ] || { echo 'PRIVATE_DIR or MIDI_FIXTURE_DIR is required; run run_pc_checks.sh first.' >&2; exit 64; }
budget_private=$(midi_external_directory "$budget_private" PRIVATE_DIR)
budget_pc=$budget_private/th06psp_pc/midi_pc_check
[ -x "$budget_pc" ] || { echo 'run tools/midi/run_pc_checks.sh first' >&2; exit 1; }
if [ -n "${PSPDEV:-}" ]; then export PATH="$PSPDEV/bin:$PATH"; fi
PSP_CXX=${PSP_CXX:-psp-g++}
PSP_NM=${PSP_NM:-psp-nm}
psp_prefix=$(psp-config --psp-prefix)
out=$(midi_temp_directory th06_midi_budget)
printf 'ME BUDGET PRIVATE OUTPUT %s\n' "$out"
"$PSP_CXX" -std=gnu++17 -O2 -G0 -march=allegrex -mtune=allegrex \
  -fno-exceptions -fno-rtti -fno-pic -fno-jump-tables -mno-check-zero-division \
  -DPSP -DTH06_PSP -D_PSP_FW_VERSION=660 -I"$here/psp" -I"$tdls_dir/include" \
  -I"$psp_prefix/include" -I"$psp_prefix/sdk/include" \
  -c "$here/tools/midi/me_budget_probe.cpp" -o "$out/probe.o"
declare -A size
while read -r key bytes; do size[${key#th06_midi_budget_}]=$bytes; done < <(
  "$PSP_NM" -S --radix=d "$out/probe.o" | \
    awk '$4 ~ /^th06_midi_budget_/ { print $4, $2 + 0 }'
)
layout=${size[state_offset]}:${size[arena]}:${size[art]}:${size[wave]}:${size[region]}:${size[instrument]}:${size[map]}:${size[voice]}
# Match the root Makefile's ordered source/header hash without invoking make
# (which would also inspect/build unrelated PSP dependencies).
read -r -a tdls_names <<< "$(awk '$1=="TDLS_NAMES" && $2==":=" {for(i=3;i<=NF;i++) printf "%s ", $i}' "$here/Makefile")"
[ "${#tdls_names[@]}" -gt 0 ] || { echo 'cannot determine common source list from Makefile' >&2; exit 2; }
tdls_sources=()
for name in "${tdls_names[@]}"; do tdls_sources+=("$tdls_dir/src/$name.cpp"); done
budget_source_id=0x$(cat "${tdls_sources[@]}" "$tdls_dir"/include/tdls/*.hpp | sha256sum | cut -c1-8)
echo "ME common_source_id $budget_source_id"
echo "PSP ABI state_offset:arena:art:wave:region:instrument:map:voice $layout"
"$budget_pc" budget --blob "$1" --voices "${2:-40}" --psp-layout "$layout"
