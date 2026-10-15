#!/bin/bash
# Compile never-run ELF oracles, no EBOOT. All outputs remain private.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
source "$repo/tools/midi/tool_paths.sh"
if [ -n "${PSPDEV:-}" ]; then export PATH="$PSPDEV/bin:$PATH"; fi
PSP_CXX=${PSP_CXX:-psp-g++}
command -v "$PSP_CXX" >/dev/null || { echo 'PSP cross compiler required; put psp-g++ on PATH or set PSP_CXX/PSPDEV.' >&2; exit 2; }
out=$(midi_temp_directory th06_me_gate_controls)
printf 'ME GATE PRIVATE OUTPUT %s\n' "$out"
flags=(-O2 -G0 -march=allegrex -mtune=allegrex -fno-pic -fno-jump-tables -mno-check-zero-division -std=gnu++17 -fno-exceptions -fno-rtti -nostdlib -Wl,-e,meLibOnProcess,-Ttext,0x08800000,--build-id=none)
for kind in direct indirect tail; do
  indirect=0; tail=0
  [ "$kind" != direct ] && indirect=1
  [ "$kind" = tail ] && tail=1
  extra=()
  [ "$tail" = 0 ] && extra+=(-fno-optimize-sibling-calls)
  "$PSP_CXX" "${flags[@]}" "${extra[@]}" -DGATE_INDIRECT="$indirect" -DGATE_TAIL="$tail" "$repo/tools/midi/me_gate_fixture.cpp" -o "$out/$kind.elf"
  if "$repo/tools/midi/me_code_check.py" "$out/$kind.elf" > "$out/$kind.log"; then rc=0; else rc=$?; fi
  want=1; [ "$kind" = direct ] && want=0
  [ "$rc" = "$want" ] || { cat "$out/$kind.log"; exit 1; }
  if [ "$kind" != direct ]; then rg -q 'indirect jump' "$out/$kind.log"; fi
  printf 'CONTROL %s rc%s expected%s OK\n' "$kind" "$rc" "$want"
done
