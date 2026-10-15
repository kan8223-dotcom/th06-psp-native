#!/usr/bin/env bash
# Shared path checks only. Source this file from the host-side runners.
MIDI_REPO_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)

midi_external_directory() {
    local directory
    directory=$(realpath -m -- "$1") || return
    case "$directory/" in
        "$MIDI_REPO_DIR/"*)
            printf '%s must be outside the repository: %s\n' "${2:-output directory}" "$directory" >&2
            return 2 ;;
    esac
    printf '%s\n' "$directory"
}

midi_temp_directory() {
    local temporary_root
    temporary_root=$(midi_external_directory "${TMPDIR:-/tmp}" TMPDIR) || return
    mktemp -d "$temporary_root/$1.XXXXXX"
}
