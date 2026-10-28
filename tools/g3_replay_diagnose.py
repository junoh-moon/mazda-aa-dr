#!/usr/bin/env python3
"""Explain archived G3 replay records without changing inputs or product gates.

Input is the existing real-replay-summary.json and on/NN.{g3,jsonl,sends}.
The output contains anonymous ordinals and allowlisted diagnostic vocabulary;
it deliberately excludes source paths, boot IDs, coordinates and headings.
Cadence fences are replay-observed input gaps, not physical sensor failures.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys


REASONS = set("missing no_heading moving_at_end unc chain_broken age not_prev_boot "
              "chain_ok chain_depart same_boot chain_unknown age_unknown position_ok "
              "moving_at_boot moved unc_grew course_ok course_error latch_unknown "
              "latch_unsure reverse wheel_stale session yaw_break heading latch "
              "off not_started journal_failed writer_lag beta_not_live adapter clock "
              "send_session source cadence_fence trusted conflict cap fault rejected "
              "none unreadable not_regular too_large empty truncated magic version_old "
              "version_future format checksum".split())
GATES = set("ACCEPTED PREVIOUS UTC UTC_MONO HDOP WHEEL REVERSE REVERSE_UNPROVEN "
            "DISPLACEMENT COURSE BAD_FIX SETTLING SPEED YAW CORE WAITING "
            "ENTRY_FRESH ENTRY_FALLBACK ENTRY_REFUSED DISABLED".split())


def safe(value, allowed):
    return value if value in allowed else "unknown"


def rows(path):
    if not path.exists():
        return None
    with path.open() as source:
        return [json.loads(line) for line in source if line.strip()]


def heading_trace(path):
    if not path.exists():
        return []
    result = []
    for line in path.read_text().splitlines():
        fields = dict(re.findall(r"(seeded|g3_breaks|resets)=(\d+)", line))
        if len(fields) != 3:
            raise ValueError("malformed heading trace")
        result.append(dict(t=float(line.split()[0]), **{k: int(v) for k, v in fields.items()}))
    return result


def raw_receipts(command):
    """Read only direct trace files and only the explicitly selected boot."""
    try:
        trip = Path(command[command.index("--trip") + 1])
        wanted = command[command.index("--boot-id") + 1]
    except (ValueError, IndexError):
        return None
    result = dict(yaw=[], wheel=[], resets=[], boundaries=[])
    files = sorted(trip.glob("trace.*.jsonl"))
    if not files:
        return None
    for path in files:
        current = None
        try:
            with path.open() as source:
                for line in source:
                    if not line.strip():
                        continue
                    row = json.loads(line)
                    if row.get("kind") == "boot":
                        current = row.get("boot_id")
                    if current != wanted:
                        continue
                    if row.get("kind") == "motion_batch":
                        for event in row.get("events", []):
                            if event[0] in (1, 2):
                                result["wheel" if event[0] == 1 else "yaw"].append(event[2] / 1e9)
                    elif row.get("kind") == "shadow_input_reset":
                        result["resets"].append(row["mono_ns"] / 1e9)
                    elif row.get("kind") in {"shadow_bus", "shadow_session"} and row.get("reset"):
                        result["boundaries"].append(row["mono_ns"] / 1e9)
        except (OSError, ValueError, KeyError, IndexError, TypeError):
            return None
    return {key: sorted(set(values)) for key, values in result.items()}


def near_break(raw, time):
    # Proximity only: receipt time and the worker's drained integration time
    # differ. No producer timestamp or physical-fault claim is manufactured.
    evidence = dict(classification="recorded_input_gap_not_physical_fault",
                    proximity_window_s=0.5)
    for stream in ("yaw", "wheel"):
        gaps = [b - a for a, b in zip(raw[stream], raw[stream][1:])
                if time - 0.5 <= b <= time + 0.05 and b - a > 0.25]
        evidence[stream + "_receipt_gap_s"] = round(max(gaps), 3) if gaps else None
    for source, name in (("resets", "recorded_input_resets"), ("boundaries", "recorded_runtime_boundaries")):
        evidence[name] = sum(time - 0.5 <= t <= time + 0.05 for t in raw[source])
    if evidence["yaw_receipt_gap_s"] is None and evidence["wheel_receipt_gap_s"] is None and \
            not evidence["recorded_input_resets"] and not evidence["recorded_runtime_boundaries"]:
        evidence["classification"] = "source_of_break_unresolved"
    return evidence


def explain_write(row, beta, trace=(), raw=None):
    # mono_ns is the replay worker's completion tick. start_ns/end_ns instead
    # measure the HOST filesystem operation and must never order replay data.
    # hv/beta/ref_s describe the REQUEST snapshot, whose replay time is absent.
    # History up to completion cannot establish what caused that snapshot.
    before = sorted((r for r in beta if r.get("mono_ns", 0) <= row["mono_ns"]),
                    key=lambda r: r.get("mono_ns", 0))
    anchors = [r for r in before if r.get("kind") == "beta_anchor"]
    accepted = [r for r in anchors if r.get("gate") == "ACCEPTED" and r.get("h") is not None]
    last = accepted[-1].get("mono_ns", 0) if accepted else 0
    later = [r for r in before if r.get("mono_ns", 0) >= last]
    fences = Counter(safe(r.get("stream"), {"position", "send"}) for r in later
                     if r.get("kind") == "beta_cadence_fence")
    result = dict(seq=row.get("seq"), state=safe(row.get("st"), {"S", "M"}),
                  heading="present" if row.get("hv") else "absent",
                  current_beta_heading=row.get("beta") is not None,
                  reference_present=row.get("ref_s") is not None,
                  prior_accepted_headings=len(accepted),
                  anchor_gates_since_last_heading=dict(Counter(
                      safe(r.get("gate"), GATES) for r in later if r.get("kind") == "beta_anchor")),
                  fence_streams=dict(fences), trace_history_at_completion="unobserved",
                  request_reference_loss="unobserved_request_time_not_recorded")
    if row.get("st") == "M" and row.get("mark"):
        origin, category = "departure_mark", "observed_record"
    elif row.get("hv"):
        origin, category = "heading_present", "observed_record"
    elif row.get("beta") is not None:
        origin, category = "heading_absent_despite_sample", "insufficient_evidence"
    elif accepted and fences:
        origin, category = "accepted_then_cadence_fence", "input_gap_evidence"
    elif anchors and not accepted:
        origin, category = "no_accepted_heading_observed", "observed_gate_history"
    else:
        origin, category = "reference_cause_unobserved", "insufficient_evidence"
    result.update(origin=origin, classification=category)
    ticks = [r for r in trace if r["t"] <= row["mono_ns"] / 1e9]
    if ticks:
        valid = [r for r in ticks if r["seeded"]]
        final = ticks[-1]
        seed = valid[-1] if valid else None
        unseeded = next((r["t"] for r in ticks if not r["seeded"] and
                         (not seed or r["t"] > seed["t"])), None)
        breaks = final["g3_breaks"] - seed["g3_breaks"] if seed else None
        result["trace"] = dict(ever_seeded=bool(seed), seeded_at_completion=bool(final["seeded"]),
                               breaks_after_seed=breaks,
                               unseeded_before_write_s=round(final["t"] - unseeded, 2)
                               if unseeded is not None else None)
        if row.get("st") == "S" and not row.get("hv") and not row.get("beta") and breaks and breaks > 0:
            result["trace_history_at_completion"] = "break_after_last_seed"
            first_break = next(r["t"] for r in ticks if r["t"] > seed["t"] and
                               r["g3_breaks"] > seed["g3_breaks"])
            if raw is not None:
                result["raw_near_first_break"] = near_break(raw, first_break)
        elif row.get("st") == "S" and not row.get("hv") and not seed:
            result["trace_history_at_completion"] = "never_seeded_in_trace"
    return result


def diagnose(archive, traces=None, commands_file=None):
    with (archive / "real-replay-summary.json").open() as source:
        summary = json.load(source)
    report = dict(schema=1, scope="archived_replay_diagnosis_not_vehicle_validation", boots=[])
    totals = Counter()
    restores = Counter()
    origins = Counter()
    commands = {}
    if commands_file:
        supplied = json.loads(commands_file.read_text())
        if len(supplied) != len(summary["runs"]):
            raise ValueError("commands and runs do not align")
        commands = {r["index"]: command for r, command in zip(summary["runs"], supplied) if r.get("mode") == "on"}
    last_write = None
    last_writer_id = None
    last_write_record = None
    skipped_since_write = False
    for run in sorted((r for r in summary["runs"] if r.get("mode") == "on"),
                      key=lambda r: r["index"]):
        index = run["index"]
        label = f"B{index:02d}"
        boot = dict(boot=label, classification="archived_replay", writes=[])
        report["boots"].append(boot)
        totals["boots"] += 1
        if run.get("rc") != 0:
            totals["not_replayable"] += 1
            boot["classification"] = "not_replayable"
            skipped_since_write = True
            continue
        totals["replayed"] += 1
        g3 = rows(archive / "on" / f"{index:02d}.g3.jsonl")
        beta = rows(archive / "on" / f"{index:02d}.jsonl")
        trace = heading_trace(traces / f"{index:02d}.heading.txt") if traces else []
        raw = raw_receipts(commands[index]) if index in commands else None
        if g3 is None:
            boot["classification"] = "insufficient_evidence"
            boot["first_send_block"] = dict(classification="insufficient_evidence", reason="missing_journal")
            totals["missing_g3_journals"] += 1
            last_write = None
            continue
        g3.sort(key=lambda r: r.get("mono_ns", 0))
        load = next((r for r in g3 if r.get("kind") == "g3_store" and
                     r.get("event") in {"missing", "rejected", "loaded", "off"}), None)
        boot["restore"] = safe(load.get("reason"), REASONS) if load else "unobserved"
        restores[boot["restore"]] += 1
        if load and last_write and load.get("seq") == last_write.get("seq") and \
                load.get("boot") == last_writer_id and load.get("st") == last_write_record.get("st") and \
                bool(load.get("hv")) == bool(last_write_record.get("hv")):
            boot["loaded_from"] = last_write.copy()
            boot["unreplayed_boot_since_write"] = skipped_since_write
        start = next((r for r in g3 if r.get("kind") == "g3_send" and r.get("event") == "start"), None)
        block = next((r for r in g3 if r.get("kind") == "g3_send" and
                      r.get("event") in {"wait", "none", "end"} and
                      (not start or r.get("mono_ns", 0) < start.get("mono_ns", 0))), None)
        boot["first_send_block"] = dict(
            classification="observed_wait" if block and block.get("event") == "wait" else
                           "observed_rejection" if block else
                           "opened_without_prior_block" if start else "insufficient_evidence",
            reason=safe(block.get("reason"), REASONS) if block else "none" if start else "unobserved")
        end = next((r for r in g3 if r.get("kind") == "g3_send" and r.get("event") == "end"), None)
        if start and end:
            boot["window_end"] = dict(reason=safe(end.get("reason"), REASONS),
                                      why=safe(end.get("why"), REASONS))
        windows = sum(r.get("kind") == "g3_send" and r.get("event") == "start" for r in g3)
        boot["windows"] = windows
        totals["send_windows"] += windows
        sends = rows(archive / "on" / f"{index:02d}.sends.jsonl")
        if sends is not None:
            totals["choice7_sends"] += sum(r.get("choice") == 7 for r in sends)
        else:
            totals["missing_send_journals"] += 1
        for row in g3:
            if row.get("kind") != "g3_store" or row.get("event") != "written":
                continue
            write = explain_write(row, beta or [], trace, raw)
            boot["writes"].append(write)
            totals["successful_record_writes"] += 1
            origins[write["origin"]] += 1
            if write["state"] == "S" and write["heading"] == "absent":
                totals["parked_heading_absent_writes"] += 1
            last_write = {k: write[k] for k in ("seq", "heading", "origin", "classification")}
            last_write["boot"] = label
            last_writer_id = str(run.get("boot", ""))[:8]
            last_write_record = row
            skipped_since_write = False
    for key in ("not_replayable", "send_windows", "choice7_sends", "parked_heading_absent_writes"):
        totals.setdefault(key, 0)
    report.update(summary=dict(totals), restore_reasons=dict(restores), write_origins=dict(origins))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--traces", type=Path, help="optional existing replay --heading-trace outputs")
    parser.add_argument("--commands", type=Path, help="optional archived argv list to locate raw inputs; never executed")
    args = parser.parse_args()
    try:
        print(json.dumps(diagnose(args.archive, args.traces, args.commands), ensure_ascii=False, indent=2, sort_keys=True))
    except (OSError, ValueError, KeyError, TypeError):
        print("diagnosis input missing or malformed; no source data printed", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
