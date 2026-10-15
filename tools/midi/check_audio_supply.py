#!/usr/bin/env python3
"""Separate strict PCM-supply gate for an unattended real MIDI pass.

  python3 tools/midi/check_audio_supply.py BOOT.LOG --engine-log log.txt --pass 2

The trajectory/memory parser remains unchanged. This gate additionally
requires exact non-reset audio counter endpoints:
  RECORD SOAK AUDIO P2 BEGIN U28 Q3072
  RECORD SOAK AUDIO P2 END U28 Q3072
Their difference covers the whole measured pass, including its last partial
periodic window. Historical logs without endpoints cannot prove U=0. Notes
lost are reported separately, not silently reclassified as queue corruption
or declared native-correct. MEMSIM never proves audio supply.
"""
import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
import compare_soak_memory as trajectory

ENDPOINT = re.compile(r"RECORD SOAK AUDIO P(\d+) (BEGIN|END) U(\d+) Q(\d+)\s*$")
PERIODIC = re.compile(r"\bAUD U\+(\d+) MIN(\d+) DROP(\d+) GAP(\d+)us MIX(\d+)us IO(\d+)us\b")
CHECKPOINT = re.compile(r"\bAUD! (\S+) C([0-9A-Fa-f]+) U(\d+) Q(\d+) G(\d+) I(\d+) .*\bD(\d+) M(\d+)\b")


@dataclass
class AudioRow:
    line: int
    timestamp: int | None
    context: str
    kind: str
    underruns: int
    drop: int
    gap: int
    mix: int
    minimum: int
    scope: str = "boundary/uncertain"


def inspect(text, selected_pass):
    lines = text.splitlines()
    begin, end, points, endpoints = None, None, [], {"BEGIN": [], "END": []}
    rows, observed_notes, run_counters = [], [], []
    current_pass, context = None, "before pass"
    problems = []
    preceding_batch_ends = []
    previous_checkpoint = 0
    for number, line in enumerate(lines, 1):
        marker = trajectory.BEGIN.search(line)
        if marker:
            current_pass = int(marker[1])
            context = "TITLE_STARTUP"
            if current_pass == selected_pass:
                begin = number
        marker = trajectory.POINT.search(line)
        if marker and int(marker[1]) == selected_pass:
            context = marker[3]
            points.append(number)
        ending = trajectory.END.search(line)
        if ending and int(ending[1]) == selected_pass:
            end = number
        endpoint = ENDPOINT.search(line)
        if endpoint and int(endpoint[1]) == selected_pass:
            endpoints[endpoint[2]].append((number, int(endpoint[3]), int(endpoint[4])))
        elif "RECORD SOAK AUDIO" in line and re.search(rf"\bP{selected_pass}\b", line):
            problems.append(f"line {number}: malformed/truncated PCM endpoint")
        stamp = re.match(r"\[\s*(\d+)\]", line)
        timestamp = int(stamp[1]) if stamp else None
        audio = PERIODIC.search(line)
        checkpoint = CHECKPOINT.search(line)
        cumulative = re.search(r"\bRUN F\d+ \+\d+ Q\d+ U(\d+)\b", line)
        if cumulative and current_pass == selected_pass and begin is not None and end is None:
            if len(preceding_batch_ends) >= 2 and preceding_batch_ends[-2] > begin:
                run_counters.append((number, int(cumulative[1])))
        if audio:
            u, minimum, drop, gap, mix, _io = map(int, audio.groups())
            row = AudioRow(number, timestamp, context, "periodic", u, drop, gap, mix, minimum)
            # Captures precede FAT writes. A prior printed batch alone does
            # not timestamp its capture. Two completed batches after BEGIN
            # conservatively place this window's previous and current
            # captures after BEGIN. Boundary deltas are never waived.
            if begin is not None and end is None and len(preceding_batch_ends) >= 2:
                if preceding_batch_ends[-2] > begin:
                    row.scope = "inside"
            if current_pass == selected_pass:
                rows.append(row)
        if checkpoint and current_pass == selected_pass:
            tag, _bits, u, minimum, gap, _io, drop, mix = checkpoint.groups()
            rows.append(AudioRow(number, timestamp, context, "checkpoint:" + tag,
                                 int(u), int(drop), int(gap), int(mix), int(minimum),
                                 "inside checkpoint; do not sum" if begin is not None and
                                 previous_checkpoint > begin and end is None else
                                 "boundary checkpoint; do not sum"))
        if checkpoint:
            previous_checkpoint = number
        # PACE followed by MIDI (or PACE only for OFF) ends the printable
        # batch. This bound does not assert that its sample was taken then.
        if re.search(r"\bMIDI (?:ME|SC|MEMSIM|OFF) J\d+/\d+ F\d+", line):
            preceding_batch_ends.append(number)
        if current_pass == selected_pass:
            lost = re.search(r"\bLOST(\d+)\b", line)
            if lost:
                observed_notes.append(int(lost[1]))
        if ending and int(ending[1]) == selected_pass:
            current_pass = None
            context = "after pass"
    if begin is None or end is None:
        problems.append("selected pass has no complete BEGIN/END interval")
    exact_delta = None
    for kind in ("BEGIN", "END"):
        if len(endpoints[kind]) != 1:
            problems.append(f"exactly one PCM {kind} endpoint required; got {len(endpoints[kind])}")
    if all(len(endpoints[k]) == 1 for k in ("BEGIN", "END")):
        first, last = endpoints["BEGIN"][0], endpoints["END"][0]
        if not points or begin is None or end is None or not (
                begin < first[0] < points[0] <= points[-1] < end < last[0]):
            problems.append("PCM endpoints must follow PASS BEGIN/END and bracket all selected canonical points")
        if any(u > 0xffffffff or q > 8191 for _, u, q in (first, last)):
            problems.append("PCM endpoint counter/queue out of range")
        exact_delta = (last[1] - first[1]) & 0xffffffff
        if exact_delta:
            problems.append(f"whole-pass cumulative PCM underrun delta {exact_delta}")
        for number, observed in run_counters:
            if ((observed - first[1]) & 0xffffffff) > exact_delta:
                problems.append(f"line {number}: inside RUN cumulative U contradicts exact endpoints")
    for row in rows:
        inside = row.scope.startswith("inside")
        if row.drop and inside:
            problems.append(f"line {row.line}: PCM queue dropped {row.drop} frames")
        if row.underruns and inside:
            problems.append(f"line {row.line}: {row.kind} U{row.underruns} ({row.scope})")
    return rows, endpoints, exact_delta, max(observed_notes, default=None), problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("boot_log", type=Path)
    parser.add_argument("--engine-log", type=Path, required=True)
    parser.add_argument("--pass", dest="selected_pass", type=int, default=2)
    parser.add_argument("--backend", choices=("me", "sc"), default="me")
    args = parser.parse_args()
    run = trajectory.parse_boot(args.boot_log)
    _, base_problems, _ = trajectory.gate([run], [args.engine_log], ["MIDI"], args.backend,
                                         selected_pass=args.selected_pass, require_midi_final=True)
    rows, endpoints, exact_delta, lost, problems = inspect(trajectory.read_log(args.boot_log), args.selected_pass)
    problems = list(dict.fromkeys(base_problems + problems))
    missing = [p for p in problems if re.search(r"PCM (?:BEGIN|END) endpoint required; got 0$", p)]
    failures = [p for p in problems if p not in missing]
    status = "FAIL" if failures else ("INSUFFICIENT" if missing else "PASS")
    periodic = [r for r in rows if r.kind == "periodic"]
    inner = [r for r in periodic if r.scope == "inside"]
    boundary = [r for r in periodic if r.scope != "inside"]
    print(status, f"PCM SUPPLY P{args.selected_pass} {args.backend.upper()}")
    print(f"Whole-pass exact U delta: {exact_delta if exact_delta is not None else 'UNKNOWN (missing endpoints)'}")
    print(f"Strictly inside periodic U+ sum: {sum(r.underruns for r in inner)}; "
          f"boundary/uncertain periodic U+ sum: {sum(r.underruns for r in boundary)}")
    print(f"Observed notesLost maximum: {lost if lost is not None else 'UNKNOWN'}; "
          "reported separately, not a native-fidelity pass.")
    print("Periodic/checkpoint maxima are window statistics, not paired per-call durations. "
          "Checkpoints overlap periodic counters and must not be added to U+ totals.")
    print("Boundary windows can include pre-pass events; exact endpoints decide whole-pass U. "
          "Only wholly inside positive windows independently fail the selected pass.")
    print("PCM DROP checks cover observed inside windows; exact endpoint coverage applies to U, not DROP.")
    print("Startup U before the selected pass BEGIN is outside its interval, not forgiven MIDI underruns. "
          "No memory-budget acceptance is implied.")
    print("\n| Line | ms | Last point | Source | U | PCM drop | GAP us | MIX us | MIN frames | Scope |")
    print("|---:|---:|---|---|---:|---:|---:|---:|---:|---|")
    for row in rows:
        if row.underruns or row.drop or row.kind.startswith("checkpoint"):
            print(f"| {row.line} | {row.timestamp} | {row.context} | {row.kind} | {row.underruns} | "
                  f"{row.drop} | {row.gap} | {row.mix} | {row.minimum} | {row.scope} |")
    if problems:
        print("\nReasons:")
        for problem in problems:
            print("- " + problem)
    return 1 if failures else (2 if missing else 0)


if __name__ == "__main__":
    raise SystemExit(main())
