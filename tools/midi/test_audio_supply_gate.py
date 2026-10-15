#!/usr/bin/env python3
"""In-memory gate controls derived from an existing complete real ME boot.

Usage: python3 tools/midi/test_audio_supply_gate.py real-r2-with-underruns.LOG log.txt
No altered log is saved or reported as real run evidence. The deliberately
cleaned synthetic control tests the gate, not the product or sound quality.
"""
import contextlib
import io
from pathlib import Path
import re
import sys
from unittest.mock import patch

sys.dont_write_bytecode = True
import check_audio_supply as gate


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    boot, engine = map(Path, sys.argv[1:])
    original = gate.trajectory.read_log(boot)
    engine_text = gate.trajectory.read_log(engine)
    checked = 0

    def run(text, expected, label):
        nonlocal checked
        def read(path):
            return text if path == boot else engine_text
        output = io.StringIO()
        with patch.object(gate.trajectory, "read_log", side_effect=read), \
             patch.object(sys, "argv", ["check_audio_supply.py", str(boot), "--engine-log", str(engine)]), \
             contextlib.redirect_stdout(output):
            result = gate.main()
        if result != expected:
            raise AssertionError(f"{label}: got rc{result}, expected {expected}\n{output.getvalue()}")
        checked += 1
        print(f"CONTROL {label} EXPECTED_RC{expected} OK")
        return output.getvalue()

    # Preserve the original real trajectory, health, note-loss reporting, and
    # timing fields. Only the explicit synthetic audio-counter fields differ.
    cleaned, active = [], False
    for line in original.splitlines():
        if "RECORD SOAK PASS BEGIN P2 MODE MIDI" in line:
            active = True
        if "RECORD SOAK AUDIO P2 " in line:
            continue
        if active:
            line = re.sub(r"(\bAUD U\+)\d+", r"\g<1>0", line)
            line = re.sub(r"(\bAUD! .*? U)\d+", r"\g<1>0", line)
            line = re.sub(r"\bDROP\d+", "DROP0", line)
            line = re.sub(r"(\bAUD! .*? D)\d+", r"\g<1>0", line)
            line = re.sub(r"(\bRUN F\d+ \+\d+ Q\d+ U)\d+", r"\g<1>100", line)
        cleaned.append(line)
        if "RECORD SOAK PASS END P2 MODE MIDI" in line:
            active = False
    without_endpoints = "\n".join(cleaned) + "\n"
    run(without_endpoints, 2, "synthetic-clean-but-missing-endpoints-insufficient")
    complete = []
    for line in cleaned:
        complete.append(line)
        if "RECORD SOAK PASS BEGIN P2 MODE MIDI" in line:
            complete.append("[000000] RECORD SOAK AUDIO P2 BEGIN U100 Q3072")
        if "RECORD SOAK PASS END P2 MODE MIDI" in line:
            complete.append("[999999] RECORD SOAK AUDIO P2 END U100 Q3072")
    synthetic = "\n".join(complete) + "\n"
    run(synthetic, 0, "explicitly-synthetic-zero-delta-pass")
    mutations = {
        "positive-whole-pass-delta": synthetic.replace("END U100 Q3072", "END U101 Q3072"),
        "duplicate-endpoint": synthetic + "[999999] RECORD SOAK AUDIO P2 END U100 Q3072\n",
        "truncated-endpoint": synthetic.replace("END U100 Q3072", "END U100"),
        "counter-out-of-range": synthetic.replace("END U100 Q3072", "END U4294967296 Q3072"),
        "queue-out-of-range": synthetic.replace("END U100 Q3072", "END U100 Q8192"),
        "terminal-pin": synthetic.replace("PIN0/0", "PIN0/1"),
        "terminal-queue-drop": re.sub(r"(RECORD SOAK MIDI FINAL.* QD)0", r"\g<1>1", synthetic),
        "unexpected-priority-exit": synthetic + "[999999] EXIT REQUEST DIAG PRIORITY F1\n",
        "audio-priority-failure-after-complete": synthetic + "[999999] AUDIO PRODUCER PRIORITY FAILED\n",
        "audio-priority-exit-after-complete": synthetic + "[999999] EXIT REQUEST AUDIO PRIORITY F1\n",
    }
    for label, text in mutations.items():
        run(text, 1, label)
    # A periodic capture in the first crossing window may be outside BEGIN.
    # Exact zero endpoints resolve that ambiguity; it must not be miscounted.
    first = True
    crossing = []
    active = False
    for line in synthetic.splitlines():
        if "RECORD SOAK PASS BEGIN P2" in line:
            active = True
        if active and first and "AUD U+0 " in line:
            line = line.replace("AUD U+0 ", "AUD U+1 ", 1)
            first = False
        crossing.append(line)
    boundary_report = run("\n".join(crossing), 0, "crossing-window-not-added-to-exact-zero")
    assert "boundary/uncertain periodic U+ sum: 1" in boundary_report
    # Known inside-window failures remain errors even if corrupted endpoints
    # claim zero. Do not trust the two endpoint numbers in isolation.
    rows, _, _, _, _ = gate.inspect(synthetic, 2)
    target = next(row.line for row in rows if row.kind == "periodic" and row.scope == "inside")
    inside = synthetic.splitlines()
    inside[target - 1] = inside[target - 1].replace("AUD U+0 ", "AUD U+1 ", 1)
    run("\n".join(inside), 1, "inside-window-contradiction-rejected")
    run(original, 1, "unchanged-real-r2-underruns-fail")
    print(f"AUDIO_SUPPLY_GATE CONTROLS{checked} PASS; SYNTHETIC CONTROLS ARE NOT RUN EVIDENCE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
