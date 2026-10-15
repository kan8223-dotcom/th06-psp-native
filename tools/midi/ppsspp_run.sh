#!/bin/bash
# Run an EBOOT in headless PPSSPP (no window, no input) against a caller-owned
# test memstick. Replaces its EBOOT and boot logs; never use your only save copy.
#   tools/midi/ppsspp_run.sh <EBOOT.PBP> <seconds> [model 0|1] [log name]
# Required environment: PPSSPP_HEADLESS, MEMSTICK, OUTPUT_DIR (outside the repo).
# The memstick holds the user's TH06 data and gm.dls (private, never in the repo)
# and PSP/SYSTEM/ppsspp.ini, the marker that keeps MECC off under PPSSPP.
set -euo pipefail
[ $# -ge 1 ] && [ $# -le 4 ] || { sed -n '2,9p' "$0" >&2; exit 64; }
source "$(dirname "$0")/tool_paths.sh"
: "${PPSSPP_HEADLESS:?set PPSSPP_HEADLESS to your headless PPSSPP executable}"
: "${MEMSTICK:?set MEMSTICK to your disposable private test memstick}"
: "${OUTPUT_DIR:?set OUTPUT_DIR to a directory outside the repository}"
EBOOT=$(realpath "$1")
SECONDS_TO_RUN=${2:-60}
MODEL=${3:-1}
NAME=${4:-run}
[[ "$SECONDS_TO_RUN" =~ ^[1-9][0-9]{0,7}$ ]] || { echo 'seconds must be a positive decimal integer (at most 8 digits)' >&2; exit 64; }
[[ "$MODEL" = 0 || "$MODEL" = 1 ]] || { echo 'model must be 0 or 1' >&2; exit 64; }
[[ "$NAME" =~ ^[[:alnum:]][[:alnum:]_.-]*$ ]] || { echo 'log name must be a plain filename, not a path' >&2; exit 64; }
PPSSPP=$(command -v "$PPSSPP_HEADLESS") || { echo "PPSSPP executable not found: $PPSSPP_HEADLESS" >&2; exit 2; }
[ -x "$PPSSPP" ] && [ -f "$EBOOT" ] || { echo 'PPSSPP must be executable and EBOOT must be a regular file' >&2; exit 2; }
M=$(midi_external_directory "$MEMSTICK" MEMSTICK)
G=$M/PSP/GAME/TH06PSP
OUT=$(midi_external_directory "$OUTPUT_DIR" OUTPUT_DIR)
[ -f "$M/PSP/SYSTEM/ppsspp.ini" ] || { echo "missing PPSSPP marker $M/PSP/SYSTEM/ppsspp.ini" >&2; exit 1; }
[ -d "$G" ] || { echo "missing private game directory: $G" >&2; exit 2; }
G=$(midi_external_directory "$G" 'memstick game directory')
mkdir -p "$OUT"
cp "$EBOOT" "$G/EBOOT.PBP"
rm -f "$M/TH06PSP_BOOT.LOG" "$M/TH06PSP_BOOT_PREV.LOG"
start=$(date +%s.%N)
set +e
PPSSPP_MEMSTICK="$M" PPSSPP_PSP_MODEL="$MODEL" timeout --kill-after=10 $((SECONDS_TO_RUN + 30)) \
    "$PPSSPP" "$G/EBOOT.PBP" --timeout="$SECONDS_TO_RUN" > "$OUT/$NAME.stdout" 2>&1
rc=$?
set -e
end=$(date +%s.%N)
cp "$M/TH06PSP_BOOT.LOG" "$OUT/$NAME.LOG" 2>/dev/null || true
printf 'ppsspp rc=%s wall=%.1fs model=%s log=%s\n' "$rc" "$(awk -v end="$end" -v start="$start" 'BEGIN {print end - start}')" "$MODEL" "$OUT/$NAME.LOG"
