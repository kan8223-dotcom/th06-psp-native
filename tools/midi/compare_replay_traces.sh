#!/bin/bash
# Compare the REPLAY_TRACE snapshot lines (every 120 frames: stage, player
# state, bomb, boss life, score, RNG seed/count, player position bits, lives,
# bombs) of several boot logs.  Timestamps are dropped; everything else must be
# byte-identical up to the shortest log.
#   tools/midi/compare_replay_traces.sh A.LOG B.LOG [C.LOG ...]
set -euo pipefail
[ $# -ge 2 ] || { sed -n 2,6p "$0"; exit 64; }
files=("$@")
source "$(dirname "$0")/tool_paths.sh"
tmp=$(midi_temp_directory th06_replay_compare)
trap 'rm -rf "$tmp"' EXIT
i=0
for f in "$@"; do
  grep -a 'REPLAY F' "$f" | sed -E 's/^\[ *[0-9]+\] //' > "$tmp/$i.txt"
  echo "$(wc -l < "$tmp/$i.txt") snapshots, last: $(tail -1 "$tmp/$i.txt") <- $f"
  i=$((i + 1))
done
n=$(for k in $(seq 0 $((i - 1))); do wc -l < "$tmp/$k.txt"; done | sort -n | head -1)
[ "$n" -gt 0 ] || { echo 'FAIL: replay snapshots are required in every log' >&2; exit 1; }
fail=0
for k in $(seq 1 $((i - 1))); do
  if cmp -s <(head -n "$n" "$tmp/0.txt") <(head -n "$n" "$tmp/$k.txt"); then
    echo "IDENTICAL first $n snapshots: ${files[0]} == ${files[$k]}"
  else
    echo "DIFFERENT: ${files[0]} vs ${files[$k]}"
    diff <(head -n "$n" "$tmp/0.txt") <(head -n "$n" "$tmp/$k.txt") | head -5
    fail=1
  fi
done
exit $fail
