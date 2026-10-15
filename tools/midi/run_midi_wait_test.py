#!/usr/bin/env python3
"""Regress actual cooperative MIDI completion waits on a fake SC clock.

Usage: python3 tools/midi/run_midi_wait_test.py /private/output-directory
Optional --sleep-reference PRIVATE_OLD_HEADER compares a previously captured
actual sleeping policy with the current cooperative implementation. Tests
require real yield calls during RENDER, retain timeout/ACK/pin contracts, and
save a 70-ms delayed-wake UND1 case as EXPECTED_LIMITATION, not a success.
This model does not measure PSP scheduling, CPU load, or frame rate.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
from run_audio_mix_test import closing_brace, function, replace_once, structure


def plain_structure(source, name):
    match = re.search(r"^struct " + re.escape(name) + r"\s*\{", source, re.MULTILINE)
    if not match:
        raise ValueError(f"missing {name}")
    end = closing_brace(source, match.end() - 1)
    return source[match.start():source.index(";", end) + 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--sleep-reference", type=Path,
                        help="old actual_midi_wait.hpp; never overwritten, used only as historical policy control")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    output = args.output_directory.resolve()
    if output == repo or repo in output.parents:
        raise SystemExit("output must be outside repository")
    output.mkdir(parents=True, exist_ok=True)
    midi_path, audio_path = repo / "psp/midi/MidiPsp.cpp", repo / "psp/audio_me.c"
    midi, audio = midi_path.read_text(), audio_path.read_text()
    types = [structure(audio, name) for name in ("MeMixInput", "MeSharedMailbox")]
    types.extend(plain_structure(midi, name) for name in ("MeJobOutput", "MeRenderInfo"))
    (output / "actual_midi_wait_types.hpp").write_text("\n\n".join(types) + "\n")
    finish = function(audio, "th06_psp_me_midi_finish")
    finish = replace_once(finish, '__asm__ volatile("sync");', 'HostMemoryBarrier();')
    finish = replace_once(finish, "box->command = ME_CMD_MIDI;",
                          "box->command = ME_CMD_MIDI;\n    HostAfterPublish(box);")
    # A deliberately reintroduced production busy wait can still be hosted,
    # but must fail the cooperative-yield assertions rather than hang.
    busy_instruction = '__asm__ volatile("nop; nop; nop; nop;" ::: "memory");'
    if busy_instruction in finish:
        finish = replace_once(finish, busy_instruction, 'HostSpinTick();')
    functions = [f"#define WAIT_HAS_LEGACY {1 if args.sleep_reference else 0}", function(audio, "record_max"), finish,
                 "template <class Fill, class Read>\n" + function(midi, "RunMeJob"), function(midi, "MeRender")]
    if args.sleep_reference:
        reference = args.sleep_reference.resolve()
        if reference == output / "actual_midi_wait.hpp":
            raise ValueError("use a fresh output directory; historical reference must remain immutable")
        legacy_text = reference.read_text()
        legacy = function(legacy_text, "th06_psp_me_midi_finish")
        if "renderPending" in legacy or "sceKernelDelayThread(50)" not in legacy:
            raise ValueError("reference is not the historical sleeping finish function")
        functions.append(legacy.replace("th06_psp_me_midi_finish", "LegacyMidiFinish", 1))
        print(f"LEGACY_CAPTURE {reference} SHA256 {hashlib.sha256(reference.read_bytes()).hexdigest()}", flush=True)
    actual = "\n\n".join(functions) + "\n"
    (output / "actual_midi_wait.hpp").write_text(actual)
    for path in (midi_path, audio_path):
        print(f"ACTUAL_SOURCE {path} SHA256 {hashlib.sha256(path.read_bytes()).hexdigest()}", flush=True)
    print("HOST_FAKE_CLOCK; NOT ME/PSP EXECUTION", flush=True)
    flags = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra"]
    if os.environ.get("MIDI_WAIT_ASAN", "1") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    def run_case(folder):
        binary = folder / "midi_wait_test"
        subprocess.run([os.environ.get("CXX", "c++"), *flags, f"-I{folder}", f"-I{output}", f"-I{repo / 'psp'}",
                        str(repo / "tools/midi/midi_wait_test.cpp"), "-o", str(binary)], check=True)
        return subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    result = run_case(output)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    (output / "midi_wait_test.tsv").write_text(result.stdout)
    if result.returncode:
        return result.returncode
    if args.negative_controls:
        candidate, historical_marker, historical_body = actual.partition("int LegacyMidiFinish")
        historical = historical_marker + historical_body
        variants = (
            ("busy_spin_reintroduced", "sceKernelDelayThread(50);", "HostSpinTick();"),
            ("missing_done_guard", "firstSleepUs > 100u && box->status != ME_STAT_DONE",
             "firstSleepUs > 100u"),
            ("missing_payload_pin", "PinUnacknowledgedMePayloads();", "(void)0;"),
            ("doubled_timeout", "sceKernelGetSystemTimeLow() - startUs >= timeoutUs",
             "sceKernelGetSystemTimeLow() - startUs >= timeoutUs * 2u"),
        )
        for name, old, new in variants:
            folder = output / name
            folder.mkdir(exist_ok=True)
            (folder / "actual_midi_wait.hpp").write_text(replace_once(candidate, old, new) + historical)
            rejected = run_case(folder)
            (folder / "negative.log").write_text(rejected.stdout + rejected.stderr)
            if rejected.returncode == 0 or "FAIL " not in rejected.stderr:
                raise RuntimeError(f"negative control {name} did not fail a concrete assertion")
            print(f"NEGATIVE_CONTROL {name} REJECTED")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
