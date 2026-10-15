#!/usr/bin/env python3
"""Host-only test of the actual SC/ME final-mix arithmetic; no PSP build.

Usage: python3 tools/midi/run_audio_mix_test.py /private/output-directory
The generated header contains unchanged source functions, except that the
ME's physical address/cache boundary is replaced with a host pointer table.
All generated files/binaries/reports stay outside the repository. This is
an arithmetic/ownership oracle, not a hardware scheduling or underrun proof.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess


def closing_brace(source, start):
    depth, state, escaped = 0, "code", False
    i = start
    while i < len(source):
        char, following = source[i], source[i:i + 2]
        if state in ("string", "char"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == ('"' if state == "string" else "'"):
                state = "code"
        elif state == "line":
            if char == "\n":
                state = "code"
        elif state == "block":
            if following == "*/":
                state = "code"
                i += 1
        elif following in ("//", "/*"):
            state = "line" if following == "//" else "block"
            i += 1
        elif char in ('"', "'"):
            state = "string" if char == '"' else "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unclosed source body")


def function(source, name):
    matches = list(re.finditer(r"^[\w \t*]+\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{",
                               source, re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(f"expected one actual definition of {name}; found {len(matches)}")
    match = matches[0]
    return source[match.start():closing_brace(source, match.end() - 1)]


def structure(source, name):
    match = re.search(r"^typedef struct " + re.escape(name) + r"\s*\{", source, re.MULTILINE)
    if not match:
        raise ValueError(f"missing source structure {name}")
    end = closing_brace(source, match.end() - 1)
    return source[match.start():source.index(";", end) + 1]


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"ME address adaptation changed: {old}")
    return source.replace(old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--baseline", action="store_true",
                        help="only test legacy kernels before the unity-nowait API exists; never claim candidate PASS")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    output = args.output_directory.resolve()
    if output == repo or repo in output.parents:
        parser.error("output directory must be outside the repository")
    output.mkdir(parents=True, exist_ok=True)
    source_path = repo / "psp/audio_me.c"
    source = source_path.read_text()
    parts = [structure(source, name) for name in ("MeMixInput", "MeSharedMailbox")]
    parts.append("static int gScWide[TH06_PSP_ME_MAX_MIX_FRAMES * 2];")
    chunk = re.search(r"\bME_AUDIO_EDRAM_ACCUM_FRAMES\s*=\s*(\d+)", source)
    if not chunk:
        raise ValueError("missing actual ME chunk size")
    parts.append(f"enum {{ ME_AUDIO_EDRAM_ACCUM_FRAMES = {chunk[1]} }};")
    parts.extend(function(source, name) for name in ("gain_to_q16", "apply_gain_q16", "saturate16", "mix_on_sc"))
    me = function(source, "process_audio_on_me")
    me = replace_once(me, "(short *)(0x80000000u | box->audioOutputPhys)",
                      "static_cast<short *>(HostPhysical(box->audioOutputPhys))")
    me = replace_once(me, "(const short *)(0x80000000u | input->sourcePhys)",
                      "static_cast<const short *>(HostPhysical(input->sourcePhys))")
    # These casts only describe cache ranges; cache operations are no-ops on
    # the host. Do not change clipping, accumulation, gain, bounds or ordering.
    me = me.replace("(uint32_t)source", "(uintptr_t)source")
    me = me.replace("(uint32_t)output", "(uintptr_t)output")
    parts.append(me)
    if not args.baseline:
        parts.append(function(source, "th06_psp_me_audio_mix_unity_nowait"))
    parts.insert(0, f"#define AUDIO_MIX_CANDIDATE {0 if args.baseline else 1}\n")
    generated = output / "actual_audio_mix.hpp"
    generated.write_text("\n\n".join(parts) + "\n")
    print(f"ACTUAL_SOURCE {source_path} SHA256 {hashlib.sha256(source_path.read_bytes()).hexdigest()}", flush=True)
    print(f"HOST_SCOPE {'LEGACY ONLY; NO CANDIDATE VALIDATION' if args.baseline else 'UNITY NOWAIT'}; NOT PSP/ME EXECUTION", flush=True)
    flags = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function"]
    if os.environ.get("AUDIO_MIX_ASAN", "1") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    binary = output / "audio_mix_test"
    subprocess.run([os.environ.get("CXX", "c++"), *flags, f"-I{output}", f"-I{repo / 'psp'}",
                    str(repo / "tools/midi/audio_mix_test.cpp"), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], stdout=subprocess.PIPE, text=True)
    print(result.stdout, end="")
    (output / "audio_mix_test.tsv").write_text(result.stdout)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
