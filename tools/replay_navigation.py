#!/usr/bin/env python3
"""PC-only production MODEL replay and paired fixed/adaptive GPS differences.

Build the local engine with `make build/replay_navigation`. Inputs use the same
JSONL/directory/tar reader and byte limits as analyze_logs.py. One complete
SHADOW boot is required. Limits: 500,000 journal rows, 250,000 replay events,
32 MiB engine stdout, 64 KiB stderr, 120 seconds. Receipt ordering is an offline
MODEL assumption: worker queue arrival/drain order and producer time are unknown.
Exit 0: paired comparison produced; 1: input invariant violation; 2: insufficient
evidence, limits, or engine failure. GPS differences are not physical accuracy.
"""
import argparse
import hashlib
import json
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time

# Also support importlib-based tests without requiring a package installation.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_logs import Auditor, bounded_int, bounded_number, decode_motion_records, finite_number, strict_object

ROOT = Path(__file__).resolve().parents[1]
MAX_INPUT_ROWS = 500_000
MAX_EVENTS = 250_000
MAX_OUTPUT_BYTES = 32 * 1024 * 1024
MAX_ERROR_BYTES = 64 * 1024
ENGINE_TIMEOUT = 120
VARIANTS = ("fixed", "adaptive")
NONBLOCKING_AUDIT_CODES = {"holdout_aborted", "holdout_unfinished_window",
                          "collector_open_session", "collector_missing_boot"}
LIMITATIONS = [
    "offline_model_receipt_order is not exact runtime worker replay or producer measurement time.",
    "Only completed windows with matching anchor and reference receipt timestamps are paired.",
    "GPS differences are not ground-truth accuracy, phone acceptance, or ASSIST qualification.",
    "Synthetic/model results do not select a winner or change any live configuration.",
]


class ReplayAuditor(Auditor):
    def __init__(self):
        super().__init__()
        self.replay_events = []
        self.replay_rows = 0
        self.replay_overflow = False
        self.replay_shadow_boots = 0
        self.replay_position_count = 0
        self.blocking_issues = 0
        self.nonblocking_issues = 0

    def issue(self, code, source, detail, violation=False):
        super().issue(code, source, detail, violation)
        if not violation and code in NONBLOCKING_AUDIT_CODES:
            self.nonblocking_issues += 1
        else:
            self.blocking_issues += 1

    def overflow(self, source):
        self.issue("replay_input_limit", source, "Replay row/event budget exceeded")
        self.replay_overflow = True
        self.replay_events.clear()

    def consume(self, row, source):
        if self.replay_overflow:
            return
        self.replay_rows += 1
        if self.replay_rows > MAX_INPUT_ROWS:
            self.overflow(source)
            return
        before = sum(self.issue_counts.values())
        super().consume(row, source)
        if not isinstance(row, dict) or row.get("stream") == "collector":
            return
        kind = row.get("kind")
        if kind == "shadow_boot":
            if row.get("active") is True and row.get("domain") == "model":
                self.replay_shadow_boots += 1
        if sum(self.issue_counts.values()) != before:
            return
        if kind == "position":
            if (not bounded_int(row.get("mono_ns"), 1, 2**64 - 1) or
                    not bounded_int(row.get("utc_s"), 0, 2**64 - 1) or
                    not bounded_int(row.get("mode"), -2**31, 2**31 - 1) or
                    any(row.get(k) is not None and not finite_number(row[k])
                        for k in ("lat", "lon", "heading", "kmh"))):
                self.issue("replay_position_fields", source, "Invalid POSITION timestamp/mode/number")
                return
            self.replay_events.append((row["mono_ns"], 0, 0, "P", row["mono_ns"],
                                       row["mode"], row["utc_s"], row["lat"], row["lon"],
                                       row["heading"], row["kmh"]))
            self.replay_position_count += 1
        elif kind in ("motion", "motion_batch"):
            for event in decode_motion_records(row):
                self.replay_events.append((event["received_ns"], 1, event["receive_seq"],
                                           "M", event["sensor"], event["epoch"],
                                           event["receive_seq"], event["received_ns"],
                                           event["source_mono_ms"], *event["raw"],
                                           event["count"], event["reverse"]))
        if len(self.replay_events) > MAX_EVENTS:
            self.overflow(source)

    def replay_report(self):
        if len(self.boots) != 1 or len(self.sessions) != 1 or self.boots[0].get("mode") != 4:
            self.issue("replay_requires_one_shadow_boot", "inputs", "Require exactly one SHADOW=4 session")
        if self.replay_shadow_boots != 1:
            self.issue("replay_requires_active_shadow", "inputs", "Require exactly one active MODEL shadow_boot")
        if set(self.motion_sensors) != {"1", "2", "3"}:
            self.issue("replay_requires_raw_sensors", "inputs", "Require wheels, yaw, and reverse raw records")
        if not self.replay_position_count:
            self.issue("replay_requires_position", "inputs", "Require complete POSITION records")
        if self.replay_events:
            first, last = min(e[0] for e in self.replay_events), max(e[0] for e in self.replay_events)
            if not self.health or max(h["mono_ns"] for h in self.health) < last:
                self.issue("replay_uncovered_input_tail", "inputs", "Health must cover final POSITION and motion receipt")
            if len(self.boots) == 1 and not bounded_int(self.boots[0].get("mono_ns"), 1, first-1):
                self.issue("replay_input_before_boot", "inputs", "Replay input must follow its boot")
        return super().report()


def write_protocol(events, stream):
    # Integers never pass through float, including uint64 receipt times/epochs.
    for event in sorted(events, key=lambda item: item[:3]):
        stream.write((" ".join("null" if v is None else str(v) for v in event[3:]) + "\n").encode("ascii"))


def run_engine(events, engine, timeout=ENGINE_TIMEOUT):
    """Drain both output pipes with hard budgets; never invoke a shell."""
    engine = Path(engine)
    if not engine.is_absolute():
        raise ValueError("Engine path must be absolute")
    with tempfile.TemporaryFile() as source:
        write_protocol(events, source)
        source.seek(0)
        with subprocess.Popen([str(engine)], stdin=source, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE) as child:
            output = bytearray()
            error_bytes = 0
            deadline = time.monotonic() + timeout
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(child.stdout, selectors.EVENT_READ, "out")
                    selector.register(child.stderr, selectors.EVENT_READ, "err")
                    while selector.get_map():
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise ValueError("Engine timeout")
                        for key, _ in selector.select(min(remaining, 0.25)):
                            chunk = key.fileobj.read1(65536)
                            if not chunk:
                                selector.unregister(key.fileobj)
                            elif key.data == "out":
                                if len(output) + len(chunk) > MAX_OUTPUT_BYTES:
                                    raise ValueError("Engine stdout budget exceeded")
                                output.extend(chunk)
                            else:
                                error_bytes += len(chunk)
                                if error_bytes > MAX_ERROR_BYTES:
                                    raise ValueError("Engine stderr budget exceeded")
                child.wait(timeout=max(0.001, deadline - time.monotonic()))
            except BaseException:
                child.kill()
                child.wait()
                raise
            if child.returncode:
                raise ValueError("Engine failed with exit code %d" % child.returncode)
    return parse_engine(output)


def stats(values):
    count, mean, low, high = 0, None, None, None
    for value in values:
        count += 1
        mean = value if mean is None else mean + (value - mean) / count
        low = value if low is None else min(low, value)
        high = value if high is None else max(high, value)
    return dict(count=count, mean=mean, min=low, max=high)


def parse_engine(output):
    windows = {variant: {} for variant in VARIANTS}
    warmup_aborts = {variant: 0 for variant in VARIANTS}
    summaries = {}
    final = None
    rows = output.splitlines()
    if not rows or len(rows) > MAX_INPUT_ROWS or not output.endswith(b"\n"):
        raise ValueError("Incomplete or oversized engine output")
    for raw in rows:
        if len(raw) > 4096:
            raise ValueError("Oversized engine record")
        row = json.loads(raw, object_pairs_hook=strict_object,
                         parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Nonfinite engine number")))
        if (not isinstance(row, dict) or final is not None or row.get("domain") != "model" or
                row.get("assist_ready") is not False):
            raise ValueError("Invalid engine record order")
        kind, variant = row.get("kind"), row.get("variant")
        if kind == "final":
            if row.get("domain") != "model" or row.get("assist_ready") is not False:
                raise ValueError("Invalid engine final qualification")
            if type(row.get("inconclusive")) is not bool or any(not bounded_int(row.get(k), 0, 2**64 - 1)
                    for k in ("records", "groups", "last_received_ns", "watermark_ns", "faults")):
                raise ValueError("Missing engine final counters")
            if row["groups"] > row["records"] or row["watermark_ns"] > row["last_received_ns"]:
                raise ValueError("Invalid engine final times/counts")
            final = {k: row[k] for k in ("records", "groups", "last_received_ns", "watermark_ns", "faults", "inconclusive")}
            continue
        if variant not in VARIANTS:
            raise ValueError("Invalid engine variant")
        if kind == "summary":
            if variant in summaries:
                raise ValueError("Duplicate engine summary")
            counters = ("frontier_ns", "events", "intervals", "resets", "rejected", "enqueue_faults",
                        "drain_faults", "holdout_faults", "holdout_aborts", "model_valid_groups",
                        "calibration_version", "wheel_scale_version")
            if (any(not bounded_int(row.get(k), 0, 2**64-1) for k in counters) or
                    not bounded_int(row.get("state"), 0, 6) or not bounded_int(row.get("holdout_phase"), 0, 2) or
                    not bounded_int(row.get("result"), 0, 2**31-1) or
                    any(type(row.get(k)) is not bool for k in ("valid", "model_valid")) or
                    any(not isinstance(row.get(k), str) for k in ("pipeline_result", "gps_anchor_gate")) or
                    not bounded_number(row.get("yaw_zero"), 0, 4093) or
                    not bounded_number(row.get("wheel_scale"), 0.95, 1.05) or
                    not bounded_number(row.get("distance_m"), 0, sys.float_info.max) or
                    not isinstance(row.get("state_counts"), list) or len(row["state_counts"]) != 7 or
                    any(not bounded_int(n, 0, 2**64-1) for n in row["state_counts"])):
                raise ValueError("Invalid engine summary fields")
            summaries[variant] = {k: row[k] for k in (
                "state", "valid", "model_valid", "frontier_ns", "events", "intervals", "resets", "rejected",
                "enqueue_faults", "drain_faults", "holdout_faults", "holdout_phase", "yaw_zero",
                "calibration_version", "wheel_scale", "wheel_scale_version", "result", "pipeline_result",
                "distance_m", "gps_anchor_gate", "model_valid_groups", "state_counts", "holdout_aborts")}
            continue
        if (kind != "holdout" or variant in summaries or row.get("domain") != "model" or
                row.get("assist_ready") is not False or row.get("time_basis") != "receipt_model"):
            raise ValueError("Invalid engine holdout envelope")
        event = row.get("event")
        anchor, reference, frontier = row.get("anchor_ns"), row.get("reference_ns"), row.get("frontier_ns")
        if event not in ("BEGIN", "COMPARED", "END", "ABORT") or any(
                not bounded_int(n, 0, 2**64 - 1) for n in (anchor, reference, frontier, row.get("window_id"),
                                                         row.get("calibration_version"), row.get("wheel_scale_version"))):
            raise ValueError("Invalid engine holdout event")
        if (not bounded_number(row.get("yaw_zero"), 0, 4093) or
                not bounded_number(row.get("wheel_scale"), 0.95, 1.05) or
                type(row.get("model_valid")) is not bool or not isinstance(row.get("reason"), str)):
            raise ValueError("Invalid engine holdout calibration")
        calibration = tuple(row[k] for k in ("yaw_zero", "calibration_version", "wheel_scale", "wheel_scale_version"))
        if event != "COMPARED" and (row.get("position_error_m") is not None or row.get("heading_error_rad") is not None):
            raise ValueError("Unexpected non-comparison metrics")
        # A warmup ABORT has no started window; count every event separately.
        if not anchor and event == "ABORT":
            if row["window_id"] or reference or frontier:
                raise ValueError("Invalid engine warmup ABORT")
            warmup_aborts[variant] += 1
            continue
        window = windows[variant].setdefault(anchor, dict(begun=False, end=False, abort=False, comparisons={},
                                                       window_id=row["window_id"], calibration=calibration,
                                                       last_reference=anchor))
        if window["window_id"] != row["window_id"] or (event != "ABORT" and window["calibration"] != calibration):
            raise ValueError("Engine window/calibration changed")
        if event == "BEGIN":
            if not anchor or not row["window_id"] or reference != anchor or frontier != anchor or window["begun"] or window["end"] or window["abort"]:
                raise ValueError("Duplicate engine window")
            window["begun"] = True
        elif event == "COMPARED":
            if (not window["begun"] or window["end"] or window["abort"] or
                    not anchor < reference == frontier or reference <= window["last_reference"]):
                raise ValueError("Invalid engine comparison sequence")
            metrics = (row.get("position_error_m"), row.get("heading_error_rad"))
            if (row.get("model_valid") is not True or not finite_number(metrics[0]) or metrics[0] < 0 or
                    (metrics[1] is not None and (not finite_number(metrics[1]) or abs(metrics[1]) > 3.141592653589793))):
                raise ValueError("Invalid engine comparison metrics")
            window["comparisons"][reference] = metrics
            window["last_reference"] = reference
        elif event == "END":
            if not window["begun"] or window["end"] or window["abort"] or frontier < window["last_reference"]:
                raise ValueError("Invalid engine END")
            window["end"] = True
        else:
            window["abort"] = True
    if final is None or set(summaries) != set(VARIANTS):
        raise ValueError("Missing engine summary/final")
    if any(sum(s["state_counts"]) != final["groups"] or s["model_valid_groups"] > final["groups"] or
           s["frontier_ns"] > final["last_received_ns"] for s in summaries.values()):
        raise ValueError("Engine summary/final disagreement")
    completed = {v: {a: w for a, w in windows[v].items() if w["end"] and not w["abort"]}
                 for v in VARIANTS}
    pairs = []
    paired_windows = 0
    for anchor in sorted(completed["fixed"].keys() & completed["adaptive"].keys()):
        fixed = completed["fixed"][anchor]["comparisons"]
        adaptive = completed["adaptive"][anchor]["comparisons"]
        references = sorted(fixed.keys() & adaptive.keys())
        paired_windows += bool(references)
        pairs.extend((fixed[r], adaptive[r]) for r in references)
    coverage = {}
    for variant in VARIANTS:
        total = sum(len(w["comparisons"]) for w in windows[variant].values())
        complete = sum(len(w["comparisons"]) for w in completed[variant].values())
        coverage[variant] = dict(windows=len(windows[variant]), completed_windows=len(completed[variant]),
            incomplete_windows=len(windows[variant]) - len(completed[variant]),
            warmup_aborts=warmup_aborts[variant],
            aborted_windows=sum(w["abort"] for w in windows[variant].values()),
            compared_events=total, incomplete_compared_events=total-complete,
            completed_compared_events=complete, unmatched_completed_compared_events=complete-len(pairs))
    metrics = {}
    for index, name in enumerate(("position_difference_m", "signed_heading_difference_rad")):
        eligible = [p for p in pairs if p[0][index] is not None and p[1][index] is not None]
        metrics[name] = dict(fixed=stats(p[0][index] for p in eligible),
                             adaptive=stats(p[1][index] for p in eligible),
                             adaptive_minus_fixed=stats(p[1][index] - p[0][index] for p in eligible))
    heading = [p for p in pairs if p[0][1] is not None and p[1][1] is not None]
    metrics["absolute_heading_difference_rad"] = dict(
        fixed=stats(abs(p[0][1]) for p in heading), adaptive=stats(abs(p[1][1]) for p in heading),
        adaptive_minus_fixed=stats(abs(p[1][1])-abs(p[0][1]) for p in heading))
    return dict(paired_samples=len(pairs), paired_windows=paired_windows, coverage=coverage,
                paired_metrics=metrics, variants=summaries, engine=final)


def replay(paths, engine=None):
    auditor = ReplayAuditor()
    for path in paths:
        auditor.read_path(path)
    audit = auditor.replay_report()
    report = dict(report_schema=1, status=audit["status"], time_basis="offline_model_receipt_order",
                  producer_time="unknown", gps_is_ground_truth=False, dr_accuracy="not_established",
                  phone_acceptance="not_established", assist_ready=False, limitations=LIMITATIONS,
                  input_events=len(auditor.replay_events),
                  nonblocking_audit_codes=sorted(NONBLOCKING_AUDIT_CODES),
                  nonblocking_audit_issues=auditor.nonblocking_issues,
                  audit={k: audit[k] for k in ("status", "record_counts", "issue_counts", "omitted_issue_details")},
                  issues=[{k: issue[k] for k in ("severity", "code", "source")} for issue in audit["issues"]])
    if auditor.blocking_issues:
        return report
    try:
        executable = Path(engine or ROOT / "build" / "replay_navigation")
        hasher = hashlib.sha256()
        with executable.open("rb") as binary:
            for chunk in iter(lambda: binary.read(65536), b""):
                hasher.update(chunk)
        result = run_engine(auditor.replay_events, executable)
        report["engine_executable"] = dict(name=executable.name, sha256=hasher.hexdigest())
    except (OSError, ValueError, KeyError, TypeError, OverflowError, subprocess.TimeoutExpired) as exc:
        report["status"] = "inconclusive"
        # Do not echo arbitrary engine output or potentially sensitive input.
        report["issues"].append(dict(severity="inconclusive", code="replay_engine_error", source=type(exc).__name__))
        return report
    report["comparison"] = result
    report["status"] = "comparison_produced" if result["paired_samples"] and not result["engine"]["inconclusive"] and not result["engine"]["faults"] else "inconclusive"
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help="One complete SHADOW session: JSONL, directory, or tar")
    parser.add_argument("--engine", type=Path, help="Trusted local replay executable (absolute path)")
    parser.add_argument("--json", action="store_true", help="Emit a coordinate-free JSON comparison report")
    args = parser.parse_args(argv)
    report = replay(args.inputs, args.engine)
    if args.json:
        print(json.dumps(report, indent=2, allow_nan=False))
    else:
        print("%s: %d paired MODEL/GPS samples" % (report["status"], report.get("comparison", {}).get("paired_samples", 0)))
        for name, values in report.get("comparison", {}).get("paired_metrics", {}).items():
            print("%s: %s" % (name, values))
        for issue in report["issues"][:10]:
            print("%s: %s" % (issue["code"], issue["source"]))
        print("Receipt-time MODEL only; GPS differences do not establish physical accuracy or ASSIST readiness.")
    return {"comparison_produced": 0, "violation": 1, "inconclusive": 2}[report["status"]]


if __name__ == "__main__":
    sys.exit(main())
