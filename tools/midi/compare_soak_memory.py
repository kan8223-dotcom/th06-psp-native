#!/usr/bin/env python3
"""Gate unattended RECORD_SOAK trajectories and compare WAV/MIDI heap budgets.

Examples (all log and report artifacts belong in a private directory):
  python3 tools/midi/compare_soak_memory.py run.LOG --engine-log run.log.txt
  python3 tools/midi/compare_soak_memory.py run.LOG --engine-log run.log.txt \\
      --require-midi-backend memsim --markdown report.md --tsv report.tsv
  python3 tools/midi/compare_soak_memory.py wav.LOG --engine-log wav.log.txt --mode WAV
  # Repeated original WAV history: P2 WAV compared to candidate P2 MIDI.
  # Add --baseline-wav repeated.LOG --baseline-engine-log repeated.log.txt --baseline-pass 2

Exit 0 requires complete ordered points, pass ends, COMPLETE, ENGINE RETURN 0,
and no failures. Default requires MIDI TOTAL free >= WAV and MAXG >= WAV
at every point. TOTAL accounts for differing static/heap placement through
the actual reserved capacity; raw USED remains visible. TOTAL includes the
uncommitted reserved heap tail; SCAN1/FLAGS0 are required. A MEMSIM pass proves only
the emulated main-RAM footprint; it does not prove MIDI sound or real ME use.
Historical markers are reported as incomplete evidence, never as a pass.
Single-mode checks are trajectory-only unless a WAV baseline is supplied
for a MIDI trajectory; that combination strictly compares all 24 points.
--require-midi-final requires a complete healthy terminal record after the
candidate's ENGINE RETURN, without requiring new markers in the old baseline.
Malformed/duplicate records and nonzero terminal PIN are failures whenever
present, even without that option. FINAL counters never prove game-time ME.
"""

import argparse
import csv
import io
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


POINTS = (
    "TITLE_BEGIN",
    *(f"STAGE{s}_{event}" for s in range(1, 7) for event in ("LOAD", "END")),
    "ED_ENTER", "ED_LOAD", "ED_END", "RESULT_ENTER", "RESULT_END", "TITLE_RETURN",
    "EX_LOAD", "EX_END", "EX_RESULT_ENTER", "EX_RESULT_END", "EX_TITLE_RETURN",
)
BEGIN = re.compile(r"RECORD SOAK PASS BEGIN P(\d+) MODE (WAV|MIDI)\b")
END = re.compile(r"RECORD SOAK PASS END P(\d+) MODE (WAV|MIDI)\b")
POINT = re.compile(r"RECORD SOAK POINT P(\d+) M(WAV|MIDI) (\w+)\b(.*)")
HEAP = re.compile(r"\bH(\d+)/(\d+)KB\b")
MAXIMUM = re.compile(r"\bMAX(\d+)KB\b")
MIDI_FINAL = re.compile(
    r"RECORD SOAK MIDI FINAL BACKEND (OFF|SC|ME|MEMSIM) "
    r"J(\d+)/(\d+) F(\d+) QD(\d+) FB(\d+)/(\d+) "
    r"SCR(\d+)/(\d+)/(\d+) PIN(\d+)/(\d+)\s*$"
)
FAILURE = re.compile(
    r"\bR-\d+\b|\bOOM\b|\b(?:MALLOC|MEMALIGN) NG\b|WATCHDOG TIMEOUT|"
    r"POWER OFF REQUIRED|RECORD SOAK .*\b(?:FAILED|FAIL|ERROR|NG)\b|"
    r"MIDI (?:SONG NG|SONG POOL FULL|RETIRE LIST FULL|COMMAND QUEUE FULL|BOOT.*TIMEOUT)|"
    r"MIDI SONG (?:STOP ACK FAILED|POOL STATE|POOL SPAN|POOL FREE UNKNOWN)|"
    r"GE JPEG CACHE MISS SERIAL|"
    r"MIDI SELFTEST SC!=ME|MIDI ME (?:INFO|LOAD|INIT|REINIT) NG|"
    r"\b(?:SCRUB|BAD|FLIPS)[1-9]\d*\b"
)
ENGINE_FAILURE = re.compile(r"error\s*:|読み込めません|初期化に失敗", re.IGNORECASE)


@dataclass
class Sample:
    source: str
    used: int | None = None
    free: int | None = None
    maximum: int | None = None
    extras: dict = field(default_factory=dict)
    canonical: bool = False
    pass_id: int = 0
    mode: str = ""


@dataclass
class Run:
    path: Path
    samples: dict = field(default_factory=dict)
    begins: dict = field(default_factory=dict)
    ends: set = field(default_factory=set)
    order: dict = field(default_factory=dict)
    pass_order: list = field(default_factory=list)
    problems: list = field(default_factory=list)
    notes: list = field(default_factory=list)
    backend: str | None = None
    complete: bool = False
    returned: bool = False
    skipped_midi: bool = False
    runtime_me_jobs: int = 0
    runtime_me_frames: int = 0
    completed_pass_count: int | None = None
    midi_final_count: int = 0
    midi_final: dict | None = None


def read_log(path):
    raw = path.read_bytes()
    for encoding in ("utf-8-sig", "cp932"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            pass
    raise ValueError(f"{path}: neither UTF-8 nor CP932 (possibly truncated/corrupt log)")


def sample_from_line(line, source, canonical=False):
    heap, largest = HEAP.search(line), MAXIMUM.search(line)
    extras = {name: int(value) for name, value in
              re.findall(r"\b(ARENA|TOP|KFREE|KMAX)(\d+)KB\b", line)}
    extras.update({name: int(value) for name, value in
                   re.findall(r"\b(USED|TOTAL|CAP|MAXNG|MAXG)(\d+)B\b", line)})
    extras.update({name: int(value) for name, value in
                   re.findall(r"\b(SCAN|FLAGS)(\d+)\b", line)})
    return Sample(source, *(map(int, heap.groups()) if heap else (None, None)),
                  int(largest[1]) if largest else None, extras, canonical)


def parse_boot(path):
    run = Run(path)
    active = None
    engine_started = False
    midi_disabled = False
    last_mode = "WAV"
    last_pass = 0
    for number, line in enumerate(read_log(path).splitlines(), 1):
        source = f"{path}:{number}"
        if "ENGINE ENTER" in line:
            engine_started = True
        # MidiPsp.cpp BootPrepare deliberately probes 24 MiB to detect 32 MiB.
        probe = (not engine_started and
                 re.search(r"\bMALLOC NG 25165824B A0\b", line) is not None)
        if probe:
            run.notes.append(f"{source}: expected 24 MiB RAM-model probe failure")
        elif FAILURE.search(line) or ENGINE_FAILURE.search(line):
            run.problems.append(f"{source}: {line.strip()}")
        if ("RUNTIME STALL" in line or "AUDIO PRODUCER PRIORITY FAILED" in line or
                re.search(r"EXIT REQUEST (?:HOME|COMBO|STALL|DIAG PRIORITY|AUDIO PRIORITY)\b", line)):
            run.problems.append(f"{source}: unexpected diagnostic exit/stall")
        if "MIDI" in line:
            fallback = re.search(r"\bFB(\d+)/(\d+)\b", line)
            scrub = re.search(r"\bSCR(\d+)/(\d+)(?:/(\d+))?\b", line)
            if fallback and int(fallback[1]):
                run.problems.append(f"{source}: MIDI fallback count {fallback[1]}")
            if scrub:
                bad = int(scrub[2]) if scrub[3] is not None else int(scrub[1])
                repaired = int(scrub[3]) if scrub[3] is not None else int(scrub[2])
                if bad or repaired:
                    run.problems.append(f"{source}: MIDI scrub found/repaired corruption")
            drops = re.search(r"\b(?:QD|QUEUEDROP|QUEUEDROPS)(\d+)\b", line)
            if drops and int(drops[1]):
                run.problems.append(f"{source}: MIDI queue drops {drops[1]}")
            if re.search(r"MIDI.*(?:RUNTIMEFAIL|POISON|NOT ACKNOWLEDGED)", line):
                run.problems.append(f"{source}: MIDI runtime failure/poison/unacknowledged job")
            # READY includes boot selftests. Only counters observed after the
            # MIDI pass begins can prove game-time ME render execution.
            me_jobs = re.search(r"(?:MIDI ME|RECORD SOAK MIDI CHECK .*BACKEND ME)\s+J(\d+)/(\d+)\s+F(\d+)\b", line)
            if me_jobs and active is not None and active[1] == "MIDI":
                run.runtime_me_jobs += int(me_jobs[1])
                run.runtime_me_frames += int(me_jobs[3])
        if "RECORD SOAK MIDI FINAL" in line:
            run.midi_final_count += 1
            if run.midi_final_count != 1:
                run.problems.append(f"{source}: duplicate MIDI FINAL")
            if not run.returned or not run.complete or active is not None:
                run.problems.append(f"{source}: MIDI FINAL must follow COMPLETE and ENGINE RETURN")
            final = MIDI_FINAL.search(line)
            if not final:
                run.problems.append(f"{source}: malformed/truncated MIDI FINAL")
            else:
                names = ("me_jobs", "sc_jobs", "frames", "queue_drops", "fallbacks", "fallback_reason",
                         "scrub_passes", "scrub_bad", "scrub_repaired", "pinned_smf", "pinned_blob")
                values = dict(zip(names, map(int, final.groups()[1:])))
                run.midi_final = {"backend": final[1].lower(), **values}
                if any(value > 0xffffffff for value in values.values()):
                    run.problems.append(f"{source}: MIDI FINAL counter exceeds unsigned 32-bit range")
                if any(values[name] for name in ("queue_drops", "fallbacks", "fallback_reason",
                                                 "scrub_bad", "scrub_repaired", "pinned_smf", "pinned_blob")):
                    run.problems.append(f"{source}: MIDI FINAL has nonzero QD/FB/scrub/PIN failure state")
                if run.midi_final["backend"] != (run.backend or "off"):
                    run.problems.append(f"{source}: MIDI FINAL backend differs from MIDI READY")
        if "MIDI READY " in line:
            match = re.search(r"MIDI READY (ME|SC|MEMSIM)\b", line)
            if match:
                run.backend = match[1].lower()
                midi_disabled = False
        if re.search(r"MIDI (?:OFF|ME OFF)\b", line):
            midi_disabled = True
            if active is not None and active[1] == "MIDI":
                run.problems.append(f"{source}: MIDI disabled/fell back during MIDI pass")
        if "MIDI ME UNAVAILABLE -> SC" in line and engine_started and active:
            run.problems.append(f"{source}: runtime MIDI fallback")
        if "BGM OPEN " in line and active and active[1] == "MIDI":
            run.problems.append(f"{source}: WAV BGM opened during MIDI pass")
        if "RECORD SOAK MIDI SKIP" in line:
            run.skipped_midi = True
        match = BEGIN.search(line)
        if match:
            pass_id, mode = int(match[1]), match[2]
            if run.complete or run.returned:
                run.problems.append(f"{source}: pass began after COMPLETE/ENGINE RETURN")
            if active is not None:
                run.problems.append(f"{source}: pass began before previous pass ended")
            if pass_id in run.begins:
                run.problems.append(f"{source}: duplicate pass ID P{pass_id}")
            run.begins[pass_id] = mode
            run.pass_order.append(pass_id)
            run.order.setdefault(pass_id, [])
            active = (pass_id, mode)
            last_mode = mode
            last_pass = pass_id
            if mode == "MIDI" and (midi_disabled or run.backend is None):
                run.problems.append(f"{source}: MIDI pass without an enabled MIDI READY backend")
            continue
        match = POINT.search(line)
        if match:
            pass_id, mode, name, tail = match.groups()
            pass_id = int(pass_id)
            if run.complete or run.returned:
                run.problems.append(f"{source}: point after COMPLETE/ENGINE RETURN")
            if active != (pass_id, mode):
                run.problems.append(f"{source}: point mode/pass differs from active pass {active}")
            if name not in POINTS:
                run.problems.append(f"{source}: unknown point {name}")
            run.order.setdefault(pass_id, []).append(name)
            key = (pass_id, mode, name)
            if key in run.samples and run.samples[key].canonical:
                run.problems.append(f"{source}: duplicate point P{pass_id} {mode} {name}")
            run.samples[key] = sample_from_line(tail, source, True)
            sample = run.samples[key]
            sample.pass_id, sample.mode = pass_id, mode
            if None in (sample.used, sample.free, sample.maximum):
                run.problems.append(f"{source}: point missing Hused/freeKB or MAX")
            if not all(name in sample.extras for name in ("USED", "TOTAL", "CAP", "MAXNG", "MAXG", "SCAN", "FLAGS")):
                run.problems.append(f"{source}: missing exact USED/TOTAL/CAP/MAXNG/MAXG/SCAN/FLAGS geometry")
            elif sample.extras["SCAN"] != 1 or sample.extras["FLAGS"] != 0:
                run.problems.append(f"{source}: invalid allocator scan {sample.extras['SCAN']}/{sample.extras['FLAGS']}")
            else:
                values = sample.extras
                if (values["USED"] + values["TOTAL"] != values["CAP"] or
                        values["CAP"] > 64 * 1024 * 1024 or
                        not values["MAXNG"] <= values["MAXG"] <= values["TOTAL"] or
                        values["USED"] // 1024 != sample.used or
                        values["MAXG"] // 1024 != sample.maximum):
                    run.problems.append(f"{source}: inconsistent allocator geometry/byte vs KiB values")
            if mode == "MIDI":
                if midi_disabled or not re.search(r"\bPLAYABLE1\b", tail):
                    run.problems.append(f"{source}: MIDI point lacks PLAYABLE1/enabled backend")
            continue
        if "RECORD SOAK POINT " in line:
            run.problems.append(f"{source}: malformed/truncated RECORD SOAK POINT")
        match = END.search(line)
        if match:
            key = (int(match[1]), match[2])
            if active != key:
                run.problems.append(f"{source}: pass end differs from active pass {active}")
            if key[0] in run.ends:
                run.problems.append(f"{source}: duplicate pass end")
            run.ends.add(key[0])
            active = None
        if "RECORD SOAK COMPLETE" in line:
            if active is not None:
                run.problems.append(f"{source}: COMPLETE before active pass ended")
            if run.complete:
                run.problems.append(f"{source}: duplicate COMPLETE")
            run.complete = True
            count = re.search(r"RECORD SOAK COMPLETE PASSES(\d+)\b", line)
            if count:
                run.completed_pass_count = int(count[1])
            else:
                run.problems.append(f"{source}: COMPLETE lacks PASSES count")
        match = re.search(r"ENGINE RETURN (-?\d+)\b", line)
        if match:
            if match[1] != "0":
                run.problems.append(f"{source}: nonzero ENGINE RETURN {match[1]}")
            if not run.complete:
                run.problems.append(f"{source}: engine returned before COMPLETE")
            if run.returned:
                run.problems.append(f"{source}: duplicate ENGINE RETURN")
            run.returned = True
        # Retain older real-log evidence with its exact source, but keep it
        # noncanonical: entering a stage does not prove its recorded completion.
        legacy = re.search(r"STAGE REINIT (?:REGISTER END|RESULT) R0 NOW([1-7])\b", line)
        event = re.search(r"STAGE END SIGNAL OLD([1-7])\b", line)
        force = re.search(r"RECORD SOAK FORCE END S([1-7])\b", line)
        enter = re.search(r"SOAK STAGE ([1-7]) ENTER\b", line)
        if legacy or event or force or enter:
            stage = int((legacy or event or force or enter)[1])
            label = "EX" if stage == 7 else f"STAGE{stage}"
            name = f"{label}_{'LOAD' if legacy or enter else 'END'}"
            key = (last_pass, last_mode, name)
            sample = sample_from_line(line, source)
            sample.pass_id, sample.mode = last_pass, last_mode
            old = run.samples.get(key)
            if old is None or (not old.canonical and sample.used is not None):
                run.samples[key] = sample
            if legacy and active is None:
                run.notes.append(f"{source}: legacy stage {stage} registration R0 (incomplete trajectory evidence)")
    if active:
        run.problems.append(f"{path}: unfinished pass {active}")
    return run


def needs_memory_comparison(required_modes, baseline):
    return len(required_modes) == 2 or (baseline is not None and "MIDI" in required_modes)


def gate(runs, engine_paths, required_modes, backend, baseline=None, baseline_pass=1, selected_pass=None,
         require_midi_final=False):
    problems = [problem for run in runs for problem in run.problems]
    collected = {}
    compare_memory = needs_memory_comparison(required_modes, baseline)
    # Validate every captured pass, including the first pass that supplies
    # the matched-history warm state for a selected second-pass baseline.
    # Points remain keyed by real pass ID and mode; no log slicing/renaming.
    for run in runs:
        if require_midi_final and (run.midi_final_count != 1 or run.midi_final is None):
            problems.append(f"{run.path}: exactly one complete MIDI FINAL required for candidate boot")
        if run.pass_order != list(range(1, len(run.pass_order) + 1)):
            problems.append(f"{run.path}: pass IDs missing, repeated, or out of order: {run.pass_order}")
        for pass_id, mode in run.begins.items():
            order = run.order.get(pass_id, [])
            if tuple(order) != POINTS:
                missing = [point for point in POINTS if point not in order]
                problems.append(f"{run.path}: P{pass_id} {mode} trajectory incomplete or out of order; missing {','.join(missing) or 'none'}")
            if pass_id not in run.ends:
                problems.append(f"{run.path}: missing P{pass_id} {mode} PASS END")
        if run.begins and (not run.complete or not run.returned):
            problems.append(f"{run.path}: missing COMPLETE/ENGINE RETURN 0")
        if run.complete and run.completed_pass_count != len(run.begins):
            problems.append(f"{run.path}: COMPLETE count {run.completed_pass_count} differs from observed {len(run.begins)} passes")
        wav_ids = [pass_id for pass_id, mode in run.begins.items() if mode == "WAV"]
        midi_ids = [pass_id for pass_id, mode in run.begins.items() if mode == "MIDI"]
        if wav_ids and midi_ids and max(wav_ids) >= min(midi_ids):
            problems.append(f"{run.path}: expected WAV pass before MIDI pass")
        if run.skipped_midi and "MIDI" in required_modes:
            problems.append(f"{run.path}: MIDI unavailable/skipped")
    for mode in required_modes:
        owners = [(run, pass_id) for run in runs for pass_id, actual_mode in run.begins.items()
                  if actual_mode == mode and (selected_pass is None or pass_id == selected_pass)]
        if len(owners) != 1:
            suffix = f" P{selected_pass}" if selected_pass is not None else ""
            problems.append(f"{mode}{suffix}: expected one selected pass, found {len(owners)}")
        for run, pass_id in owners:
            if mode == "MIDI" and backend != "any" and run.backend != backend:
                problems.append(f"{run.path}: MIDI backend {run.backend!r}, expected {backend}")
            if mode == "MIDI" and run.backend == "me" and (run.runtime_me_jobs == 0 or run.runtime_me_frames == 0):
                problems.append(f"{run.path}: no game-time ME render jobs/frames during MIDI pass (READY alone is insufficient)")
            for point in POINTS:
                sample = run.samples.get((pass_id, mode, point))
                if sample:
                    key = (mode, point)
                    if key in collected:
                        problems.append(f"{sample.source}: duplicate selected measurement across boot logs {key}")
                    collected[key] = sample
    if len(engine_paths) < len(runs):
        problems.append("paired engine log.txt required (texture/init errors may be absent from boot log)")
    for path in engine_paths:
        engine_text = read_log(path)
        if not engine_text.strip():
            problems.append(f"{path}: empty engine log cannot prove error-free engine initialization")
        for number, line in enumerate(engine_text.splitlines(), 1):
            if ENGINE_FAILURE.search(line):
                problems.append(f"{path}:{number}: {line.strip()}")
    rows = []
    for point in POINTS:
        wav = baseline.samples.get((baseline_pass, "WAV", point)) if baseline else collected.get(("WAV", point))
        midi = collected.get(("MIDI", point))
        verdict = "MISSING" if compare_memory else "NOT CHECKED"
        exact = ("USED", "TOTAL", "MAXG")
        if compare_memory and wav and midi and all(name in sample.extras for sample in (wav, midi) for name in exact):
            a, b = wav.extras, midi.extras
            ok = b["TOTAL"] >= a["TOTAL"] and b["MAXG"] >= a["MAXG"]
            verdict = "OK" if ok else "OVER"
            if not ok:
                problems.append(f"{point}: MIDI exceeds WAV physical budget (total free {b['TOTAL']-a['TOTAL']:+d}, max growth-capable {b['MAXG']-a['MAXG']:+d} bytes; raw heap used delta {b['USED']-a['USED']:+d})")
            if not wav.canonical or not midi.canonical:
                verdict += " (legacy)"
        elif compare_memory:
            problems.append(f"{point}: memory comparison lacks complete WAV/MIDI exact-byte measurements")
        rows.append((point, wav, midi, verdict))
    return rows, list(dict.fromkeys(problems)), collected


def render_markdown(rows, problems, runs, baseline=None, baseline_pass=1, compare_memory=True):
    wav_label = f"Original WAV P{baseline_pass}" if baseline else "WAV"
    status = "PASS" if not problems else "FAIL"
    if not compare_memory:
        status += " (trajectory only; memory comparison NOT PERFORMED)"
    scope = ("Memory budget comparison REQUIRED: TOTAL and MAXG must both meet the WAV baseline at every point."
             if compare_memory else "Memory budget comparison NOT PERFORMED. This result checks trajectory/errors only; every comparison cell is NOT CHECKED.")
    lines = [status, "", scope, "",
             "Values are KiB displayed to 0.001; comparison uses exact bytes (TSV). TOTAL includes the reserved, uncommitted heap tail.",
             "MAXG is the analytic largest malloc request including audited SDK heap growth; MAXNG (TSV) excludes growth. SCAN1/FLAGS0 required.",
             "Comparison uses TOTAL/MAXG; raw heap USED can rise when static storage moves into the heap. Actual CAP accounts for module/static allocation. MEMSIM proves no audio or real ME behavior.", "",
             f"| Point | {wav_label} heap used | {wav_label} TOTAL | {wav_label} MAXG | MIDI heap used | MIDI TOTAL | MIDI MAXG | Comparison |",
             "|---|---:|---:|---:|---:|---:|---:|---|"]
    for point, wav, midi, verdict in rows:
        values = [sample.extras.get(name) if sample else None
                  for sample in (wav, midi) for name in ("USED", "TOTAL", "MAXG")]
        lines.append("| " + " | ".join([point, *("—" if value is None else f"{value/1024:.3f}" for value in values), verdict]) + " |")
    lines.extend(["", "Boot logs: " + ", ".join(str(run.path) for run in runs),
                  "MIDI backends: " + ", ".join(run.backend or "none" for run in runs)])
    lines.append("Game-time ME render evidence: " + ", ".join(f"{run.runtime_me_jobs} jobs/{run.runtime_me_frames} frames" for run in runs))
    lines.append("Terminal MIDI FINAL: " + ", ".join(
        (f"{run.midi_final['backend']} QD{run.midi_final['queue_drops']} "
         f"FB{run.midi_final['fallbacks']}/{run.midi_final['fallback_reason']} "
         f"SCR{run.midi_final['scrub_passes']}/{run.midi_final['scrub_bad']}/{run.midi_final['scrub_repaired']} "
         f"PIN{run.midi_final['pinned_smf']}/{run.midi_final['pinned_blob']}"
         if run.midi_final else "NOT RECORDED") for run in runs))
    if baseline:
        lines.extend([f"Original public WAV baseline P{baseline_pass}: " + str(baseline.path),
                      "Every baseline pass is validated in its original order. Selected baseline pass supplies table values; candidate WAV measurements remain in TSV."])
        midi_passes = sorted({midi.pass_id for _, _, midi, _ in rows if midi is not None})
        if midi_passes and midi_passes != [baseline_pass]:
            lines.append(f"Different preceding pass histories: baseline P{baseline_pass}, candidate MIDI {','.join('P'+str(pass_id) for pass_id in midi_passes)}. This does not exempt any budget failure.")
    if problems:
        lines.extend(["", "Failures:", "", *(f"- {problem}" for problem in problems)])
    notes = [note for run in runs for note in run.notes]
    if notes:
        lines.extend(["", "Additional evidence:", "", *(f"- {note}" for note in notes)])
    return "\n".join(lines) + "\n"


def render_tsv(rows, collected, baseline=None):
    output = io.StringIO()
    writer = csv.writer(output, delimiter="\t", lineterminator="\n")
    fields = ("pass", "mode", "used_bytes", "total_free_bytes", "capacity_bytes", "max_growth_bytes", "max_nogrow_bytes", "scan_valid", "scan_flags", "used_kib", "committed_free_kib", "max_kib", "arena_kib", "top_kib", "kernel_free_kib", "kernel_max_kib", "source", "canonical")
    modes = ("original_wav", "candidate_wav", "midi") if baseline else ("wav", "midi")
    writer.writerow(["point", *(f"{mode}_{name}" for mode in modes for name in fields), "comparison"])
    for point, wav, midi, verdict in rows:
        values = []
        samples = (wav, collected.get(("WAV", point)), midi) if baseline else (wav, midi)
        for sample in samples:
            values.extend([sample.pass_id, sample.mode,
                           *(sample.extras.get(name) for name in ("USED", "TOTAL", "CAP", "MAXG", "MAXNG", "SCAN", "FLAGS")),
                           sample.used, sample.free, sample.maximum,
                           *(sample.extras.get(name) for name in ("ARENA", "TOP", "KFREE", "KMAX")),
                           sample.source, int(sample.canonical)] if sample else [None] * len(fields))
        writer.writerow([point, *values, verdict])
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("boot_logs", nargs="+", type=Path)
    parser.add_argument("--engine-log", action="append", type=Path, default=[], help="paired log.txt; repeat for separate boots")
    parser.add_argument("--mode", choices=("WAV", "MIDI"), help="select one mode; MIDI plus --baseline-wav strictly compares memory, otherwise trajectory-only")
    parser.add_argument("--pass", dest="selected_pass", type=int, help="select a real pass ID for --mode; all captured trajectories are still validated")
    parser.add_argument("--require-midi-backend", choices=("any", "me", "sc", "memsim"), default="any")
    parser.add_argument("--require-midi-final", action="store_true",
                        help="require one complete healthy MIDI FINAL after ENGINE RETURN per candidate boot (not the original WAV baseline)")
    parser.add_argument("--baseline-wav", type=Path, help="complete original public WAV boot log; use its memory values for comparison")
    parser.add_argument("--baseline-engine-log", type=Path, help="paired original WAV log.txt (required with --baseline-wav)")
    parser.add_argument("--baseline-pass", type=int, default=1, help="real WAV baseline pass to compare (default 1); use 2 for matched warm history")
    parser.add_argument("--markdown", type=Path, help="write the full report to an existing private directory")
    parser.add_argument("--tsv", type=Path, help="write all measurements and exact source lines to a private TSV")
    args = parser.parse_args()
    if bool(args.baseline_wav) != bool(args.baseline_engine_log):
        parser.error("--baseline-wav and --baseline-engine-log must be supplied together")
    if args.selected_pass is not None and (not args.mode or args.selected_pass < 1):
        parser.error("--pass requires --mode and a positive pass ID")
    if args.baseline_pass < 1:
        parser.error("--baseline-pass must be a positive real pass ID")
    repo = Path(__file__).resolve().parents[2]
    for output in (args.markdown, args.tsv):
        if output is not None and (output.resolve() == repo or repo in output.resolve().parents):
            parser.error("report output must be outside the repository")
    try:
        runs = [parse_boot(path) for path in args.boot_logs]
        baseline = parse_boot(args.baseline_wav) if args.baseline_wav else None
        baseline_problems = []
        if baseline:
            _, baseline_problems, _ = gate([baseline], [args.baseline_engine_log], ("WAV",), "any", selected_pass=args.baseline_pass)
        required_modes = (args.mode,) if args.mode else ("WAV", "MIDI")
        rows, problems, collected = gate(runs, args.engine_log, required_modes, args.require_midi_backend,
                                         baseline, args.baseline_pass, args.selected_pass, args.require_midi_final)
        problems.extend(f"original WAV baseline: {problem}" for problem in baseline_problems)
        report = render_markdown(rows, problems, runs, baseline, args.baseline_pass, needs_memory_comparison(required_modes, baseline))
        if args.markdown:
            args.markdown.write_text(report, encoding="utf-8")
        if args.tsv:
            args.tsv.write_text(render_tsv(rows, collected, baseline), encoding="utf-8")
        print(report, end="")
        return 1 if problems else 0
    except (OSError, ValueError) as error:
        print(f"FAIL RECORD_SOAK: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
