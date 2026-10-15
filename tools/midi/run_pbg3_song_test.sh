#!/usr/bin/env bash
# Host-only all-byte oracle for the direct song decoder, linked against the
# actual unchanged legacy PBG3 reader. No PSP/product build or device writes.
# Inputs are private MD.DAT plus preexisting private extracted SMFs; outputs
# are only host binary/log under a private directory outside this repository.
set -euo pipefail
if [ "$#" -ne 3 ]; then
    printf 'usage: %s private-output-directory private/MD.DAT private/extracted-midi-directory\n' "$0" >&2
    exit 2
fi
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
source "$SCRIPT_DIR/tool_paths.sh"
OUTPUT_DIR=$(midi_external_directory "$1" 'output directory')
mkdir -p -- "$OUTPUT_DIR"
HOST_CXX=${CXX:-c++}
BUILD_FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wno-sign-compare)
if [ "${PBG3_SONG_ASAN:-1}" = 1 ]; then
    BUILD_FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"$HOST_CXX" "${BUILD_FLAGS[@]}" -I"$REPO_DIR/src" -I"$REPO_DIR/psp/midi" \
    "$SCRIPT_DIR/pbg3_song_test.cpp" "$REPO_DIR/src/pbg3/Pbg3Archive.cpp" \
    "$REPO_DIR/src/pbg3/Pbg3Parser.cpp" "$REPO_DIR/src/pbg3/IPbg3Parser.cpp" \
    "$REPO_DIR/src/pbg3/FileAbstraction.cpp" -o "$OUTPUT_DIR/pbg3_song_test"
"$OUTPUT_DIR/pbg3_song_test" "$2" "$3" | tee "$OUTPUT_DIR/pbg3_song_test.tsv"
