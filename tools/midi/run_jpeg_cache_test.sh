#!/usr/bin/env bash
# Build the ACTUAL shim helper for the host and compare with pre-stream GE.
# Usage: HOST_DEPS_PREFIX=/private/deps/usr bash tools/midi/run_jpeg_cache_test.sh \
#          /private/output-dir /private/jpegs/ED/*.jpg /private/jpegs/TL/*.jpg
# Host development headers/libjpeg are required; this script installs nothing.
# HOST_JPEG_LIBRARY selects a matching library file (default: compiler -ljpeg).
# All build/output paths and proprietary/derived images stay outside the repo.
set -euo pipefail
if [ "$#" -lt 2 ]; then
    printf 'usage: %s private-output-directory private.jpg [private2.jpg ...]\n' "$0" >&2
    exit 2
fi
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
source "$SCRIPT_DIR/tool_paths.sh"
OUTPUT_DIR=$(midi_external_directory "$1" 'output directory')
shift
mkdir -p -- "$OUTPUT_DIR"
JPEG_PREFIX=${HOST_DEPS_PREFIX:-/usr}
JPEG_LIBRARY=${HOST_JPEG_LIBRARY:--ljpeg}
HOST_CXX=${CXX:-c++}
if [ ! -f "$JPEG_PREFIX/include/jpeglib.h" ]; then
    printf 'missing host jpeglib.h; set HOST_DEPS_PREFIX to matching host development headers\n' >&2
    exit 2
fi
INCLUDE_FLAGS=(-I"$REPO_DIR/shim" -I"$JPEG_PREFIX/include")
HOST_MULTIARCH=$("$HOST_CXX" -print-multiarch 2>/dev/null || true)
if [ -n "$HOST_MULTIARCH" ]; then INCLUDE_FLAGS+=(-I"$JPEG_PREFIX/include/$HOST_MULTIARCH"); fi
BUILD_FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -ffunction-sections -fdata-sections)
if [ "${JPEG_CACHE_ASAN:-1}" = 1 ]; then
    BUILD_FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"$HOST_CXX" "${BUILD_FLAGS[@]}" "${INCLUDE_FLAGS[@]}" -c "$REPO_DIR/shim/img_decode.cpp" -o "$OUTPUT_DIR/img_decode.o"
"$HOST_CXX" "${BUILD_FLAGS[@]}" "${INCLUDE_FLAGS[@]}" "$SCRIPT_DIR/jpeg_cache_test.cpp" "$OUTPUT_DIR/img_decode.o" \
    -Wl,--gc-sections -Wl,--wrap=malloc -Wl,--wrap=calloc "$JPEG_LIBRARY" -o "$OUTPUT_DIR/jpeg_cache_test"
"$OUTPUT_DIR/jpeg_cache_test" "$@" | tee "$OUTPUT_DIR/jpeg_cache_test.tsv"
