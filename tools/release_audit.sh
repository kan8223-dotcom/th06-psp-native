#!/bin/sh
set -eu

fail=0
test_eboot_pattern='STAGE SOAK|RECORD SOAK|RESULT SOAK|PS3 REPLAY AUTO|EXACT REPLAY TRACE'

# Build the retired MECC repository name at runtime so the forbidden spelling
# does not become part of this repository merely because the audit checks it.
obsolete_mecc_pattern=$(printf '%s' 'T' 'iny[-_ ]?M' 'E|T' 'inyM' 'E')
obsolete_mecc_files=$(rg -l -i "$obsolete_mecc_pattern" README.md CREDITS.md AGENTS.md DECISIONS.md docs psp shim tools Makefile 2>/dev/null || true)
if [ -n "$obsolete_mecc_files" ]; then
    echo "[FAIL] obsolete MECC repository name remains in source or documentation:"
    echo "$obsolete_mecc_files"
    fail=1
fi

tracked_forbidden=$(git ls-files | grep -Ei '(^|/)(th6|bgm)(/|$)|紅魔郷.*\.DAT$|TH06[A-Z]*\.DAT$|\.(wav|rpy|sf2|PBP|PSP)$|(^|/)score\.dat$' || true)
if [ -n "$tracked_forbidden" ]; then
    echo "[FAIL] proprietary/user/generated files are tracked:"
    echo "$tracked_forbidden"
    fail=1
fi

if [ -f EBOOT.PBP ] && strings EBOOT.PBP | grep -Eq "$test_eboot_pattern"; then
    echo "[FAIL] PPSSPP-only automatic test marker entered EBOOT.PBP"
    fail=1
fi

if [ -d dist ]; then
    release_forbidden=$(find dist -type f | grep -Ei '/(th6|bgm)/|紅魔郷.*\.DAT$|TH06[A-Z]*\.DAT$|\.(wav|rpy|sf2)$|/score\.dat$' || true)
    if [ -n "$release_forbidden" ]; then
        echo "[FAIL] forbidden files entered dist/:"
        echo "$release_forbidden"
        fail=1
    fi
    for archive in dist/*.zip; do
        [ -f "$archive" ] || continue
        archive_entries=$(unzip -Z1 "$archive" | tr '\\' '/')
        archive_forbidden=$(printf '%s\n' "$archive_entries" | grep -Ei '(^|/)(th6|bgm)/|紅魔郷.*\.DAT$|TH06[A-Z]*\.DAT$|\.(wav|rpy|sf2)$|/score\.dat$' || true)
        if [ -n "$archive_forbidden" ]; then
            echo "[FAIL] forbidden files entered $archive:"
            echo "$archive_forbidden"
            fail=1
        fi
        if ! printf '%s\n' "$archive_entries" | grep -q '^TH06PSP/EBOOT\.PBP$'; then
            echo "[FAIL] $archive does not contain TH06PSP/EBOOT.PBP"
            fail=1
        fi
        if unzip -p "$archive" 2>/dev/null | strings | grep -Eiq "$obsolete_mecc_pattern"; then
            echo "[FAIL] obsolete MECC repository name entered $archive"
            fail=1
        fi
        if unzip -p "$archive" TH06PSP/EBOOT.PBP 2>/dev/null | strings | grep -Eq "$test_eboot_pattern"; then
            echo "[FAIL] PPSSPP-only automatic test marker entered $archive"
            fail=1
        fi
    done
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "[OK] release audit: no original TH06 data, replay, score, test EBOOT, or obsolete MECC name"
