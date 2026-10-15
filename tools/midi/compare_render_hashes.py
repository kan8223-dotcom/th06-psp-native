#!/usr/bin/env python3
"""Compare actual PPSSPP PSP-backend pixels, never state-only replay traces.

All input/output artifacts must stay in the caller's private test directory.
The stdout summary deliberately states FPS-excluded vs whole-frame coverage.
"""
import argparse
import re
import sys
import unittest
from pathlib import Path

META = re.compile(r"RENDERHASH ST(\d+) F(\d+) P(\d+) 480x272 FPS(\d+),(\d+),(\d+),(\d+) OK([01]) NZ(\d+) VAR(\d+)\s*$")
HASH = re.compile(r"RENDERHASH ST(\d+) F(\d+) (RAW|NOFPS) ([0-9a-f]{64})\s*$")
END = re.compile(r"(RENDEREND ST(\d+) F\d+ SC\d+ R[0-9a-f]{4}/\d+ X[0-9a-f]{8} Y[0-9a-f]{8} L-?\d+ BB-?\d+ NS\d+)\s*$")
PROOF = "RENDERHASH EMULATOR software frameskip=0"
MASKS = {(384, 262, 480, 272), (348, 262, 421, 272)}
DEFAULT_KEYS = {(1, 120), (1, 240), (1, 360), (1, 600)}


def parse_log(text):
    records = {}
    for line in text.splitlines():
        match = META.search(line)
        if match:
            stage, frame, present, *rest = map(int, match.groups())
            key = (stage, frame)
            record = records.setdefault(key, {})
            if "meta" in record:
                raise ValueError(f"duplicate metadata {key}")
            record["meta"] = (present, tuple(rest[:4]), *rest[4:])
            continue
        match = HASH.search(line)
        if match:
            stage, frame, kind, digest = match.groups()
            key = (int(stage), int(frame))
            record = records.setdefault(key, {})
            if kind in record:
                raise ValueError(f"duplicate {kind} {key}")
            record[kind] = digest
        elif "RENDERHASH ST" in line:
            raise ValueError("malformed/truncated pixel hash record")
    if not records:
        raise ValueError("no pixel hashes (REPLAY state snapshots are not pixel evidence)")
    for key, record in records.items():
        if set(record) != {"meta", "RAW", "NOFPS"}:
            raise ValueError(f"incomplete pixel hash {key}")
        _, mask, ok, nonblack, varied = record["meta"]
        if mask not in MASKS or ok != 1 or nonblack <= 64 or varied <= 64:
            raise ValueError(f"blank/constant image or unsupported FPS layout {key}")
        pixels = 480 * 272 - (mask[2] - mask[0]) * (mask[3] - mask[1])
        if not (nonblack <= pixels and varied < pixels):
            raise ValueError(f"invalid pixel statistics {key}")
        if any(record[kind] == "0" * 64 for kind in ("RAW", "NOFPS")):
            raise ValueError(f"zero diagnostic hash {key}")
    return records


def compare(left_text, right_text, left_emulator, right_emulator, required, full=False):
    if PROOF not in left_emulator or PROOF not in right_emulator:
        raise ValueError("missing software GPU frameskip=0 proof; fixed-skip8 runs are not pixel oracles")
    left, right = parse_log(left_text), parse_log(right_text)
    if set(left) != set(right):
        raise ValueError(f"frame sets differ: left-only {sorted(set(left)-set(right))}, right-only {sorted(set(right)-set(left))}")
    missing = required - set(left)
    if missing:
        raise ValueError(f"missing requested (stage, frame) keys: {sorted(missing)}")
    if len(left) < 2 or len({r["NOFPS"] for r in left.values()}) < 2:
        raise ValueError("fewer than two distinct real framebuffer images (possible stale image)")
    raw_same = 0
    for key in sorted(left):
        a, b = left[key], right[key]
        if a["meta"][1] != b["meta"][1]:
            raise ValueError(f"FPS rectangles differ {key}")
        if a["meta"][2:] != b["meta"][2:]:
            raise ValueError(f"non-FPS pixel statistics differ {key}")
        if a["NOFPS"] != b["NOFPS"]:
            raise ValueError(f"non-FPS actual pixels differ {key}: {a['NOFPS']} != {b['NOFPS']}")
        raw_same += a["RAW"] == b["RAW"]
    if full and raw_same != len(left):
        raise ValueError(f"whole-frame hashes differ at {len(left)-raw_same}/{len(left)} frames")
    return len(left), raw_same, sorted(left), left[next(iter(left))]["meta"][1]


def compare_ends(left_text, right_text, required):
    def parse(text):
        records = {}
        for line in text.splitlines():
            match = END.search(line)
            if match:
                record, stage = match.groups()
                stage = int(stage)
                if stage in records:
                    raise ValueError(f"duplicate stage-completion record {stage}")
                records[stage] = record
            elif "RENDEREND ST" in line:
                raise ValueError("malformed/truncated stage-completion record")
        return records
    left, right = parse(left_text), parse(right_text)
    if left != right:
        raise ValueError("replay fixture stage-completion state/score differs")
    if required - set(left):
        raise ValueError(f"missing stage-completion records: {sorted(required-set(left))}")
    return sorted(left)


def fixture(keys=(120, 240), blank=False):
    lines = []
    for i, frame in enumerate(keys):
        lines += [f"RENDERHASH ST1 F{frame} P{frame+60} 480x272 FPS384,262,480,272 OK{0 if blank else 1} NZ1000 VAR999",
                  f"RENDERHASH ST1 F{frame} RAW {str(i+1)*64}",
                  f"RENDERHASH ST1 F{frame} NOFPS {str(i+3)*64}"]
    return "\n".join(lines)


class SelfTest(unittest.TestCase):
    def test_identical(self):
        self.assertEqual(compare(fixture(), fixture(), PROOF, PROOF, set())[:2], (2, 2))

    def test_fps_only_and_full_requirement(self):
        other = fixture().replace("RAW " + "1" * 64, "RAW " + "9" * 64)
        self.assertEqual(compare(fixture(), other, PROOF, PROOF, set())[:2], (2, 1))
        with self.assertRaises(ValueError):
            compare(fixture(), other, PROOF, PROOF, set(), True)

    def test_reject_missing_duplicate_zero_blank_stale_skip_and_real_difference(self):
        bad = ["REPLAY F120 ST1", fixture(blank=True), fixture() + "\n" + fixture(),
               fixture().replace("NOFPS " + "3" * 64, "NOFPS " + "0" * 64),
               fixture().replace("NOFPS " + "4" * 64, "NOFPS " + "3" * 64),
               fixture().replace("NZ1000", "NZ1001"),
               "\n".join(fixture().splitlines()[:-1]),
               fixture().replace("NOFPS " + "3" * 64, "NOFPS " + "5" * 64)]
        for text in bad:
            with self.subTest(text=text), self.assertRaises(ValueError):
                compare(fixture(), text, PROOF, PROOF, set())
        with self.assertRaises(ValueError):
            compare(fixture(), fixture(), "frameskip=8", PROOF, set())
        with self.assertRaises(ValueError):
            compare(fixture(), fixture(), PROOF, PROOF, {(2, 120)})

    def test_stage_completion(self):
        text = "[3] RENDEREND ST3 F15985 SC28694500 Rb448/92953 X433e7370 Y43d490a1 L4 BB0 NS6"
        self.assertEqual(compare_ends(text, text, {3}), [3])
        for other in ("", text.replace("28694500", "28694510"), text + "\n" + text):
            with self.assertRaises(ValueError):
                compare_ends(text, other, {3})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", nargs="?", type=Path)
    parser.add_argument("right", nargs="?", type=Path)
    parser.add_argument("--emulator-logs", nargs=2, type=Path)
    parser.add_argument("--expect", default="1:120,1:240,1:360,1:600",
                        help="required stage-local replay stage:frame keys; every captured record is also compared")
    parser.add_argument("--require-full", action="store_true", help="also reject differences inside FPS text slot")
    parser.add_argument("--expect-ends", default="", help="required stage-cut state/score records, e.g. 1,2,3")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(SelfTest)
        return 0 if unittest.TextTestRunner().run(suite).wasSuccessful() else 1
    if not args.left or not args.right or not args.emulator_logs:
        parser.error("two boot logs and --emulator-logs <left.stdout> <right.stdout> are required")
    try:
        required = {tuple(map(int, key.split(":"))) for key in args.expect.split(",") if key}
        if any(len(key) != 2 for key in required):
            raise ValueError("--expect keys must be stage:frame")
        left_text, right_text = args.left.read_text(), args.right.read_text()
        count, raw, keys, mask = compare(left_text, right_text,
                                         *(p.read_text() for p in args.emulator_logs), required, args.require_full)
        ends = compare_ends(left_text, right_text, {int(stage) for stage in args.expect_ends.split(",") if stage})
    except (ValueError, OSError) as error:
        print(f"FAIL render hash: {error}", file=sys.stderr)
        return 1
    scope = "WHOLE FRAME" if raw == count else "FPS-EXCLUDED ONLY"
    print(f"PASS {scope}: {count} actual 480x272 framebuffer hashes; raw {raw}/{count}, no-FPS {count}/{count}; mask {mask}")
    print("Selected (stage, stage-local replay frame): " + ", ".join(f"{stage}:{frame}" for stage, frame in keys))
    print("Only naturally drawn frames: dialogue fast-forward can perform multiple replay/calculation ticks per draw.")
    print("Fixture stage-completion state/score compared: " + (", ".join(map(str, ends)) if ends else "none captured"))
    print("PC PPSSPP software GPU only; not PC-native/real-PSP pixel equality, performance, all-frame coverage or final score.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
