#!/usr/bin/env python3
"""PC-only OBSERVE log replay: byte-level SCRUB and hypothetical DROP selection.

Never loads a CMU library, sends a LOCATION, estimates phone fallback, or chooses
a DROP return value. Exit codes match analyze_logs: 0 local checks, 1 violation,
2 incomplete/unsupported evidence. Even exit 0 is not permission for a live test.
"""
import argparse
from collections import Counter
import json
import math
import sys

from analyze_logs import Auditor, CLEAR_BYTES, integer

MAX_WINDOWS = 10000
MAX_REQUESTS = 100000


def project_payload(original, mode, policy):
    """Pure offline policy; None means a hypothetical omitted LOCATION payload.

    The caller must qualify type, length, request context and trace health first.
    None deliberately specifies neither the OEM return value nor its side effects.
    """
    if len(original) != 48 or type(mode) is not int or mode not in (0, 1, 2, 3):
        raise ValueError("Expected a qualified 48-byte LOCATION and mode 0..3")
    if policy not in ("OBSERVE", "SCRUB", "DROP"):
        raise ValueError("Unknown offline policy")
    if mode != 0 or policy == "OBSERVE":
        return bytes(original)
    if policy == "DROP":
        return None
    outgoing = bytearray(original)
    for offset in CLEAR_BYTES:
        outgoing[offset] = 0
    return bytes(outgoing)


class PolicyReplay(Auditor):
    def __init__(self, max_gap_ns):
        super().__init__()
        self.max_gap_ns = max_gap_ns
        self.windows = []
        self.active = None
        self.last_coordinates = None
        self.last_event_ns = None
        self.request_sends = Counter()
        self.location_results = {}
        self.policy_issues = Counter()
        self.non_location_sends = 0

    def reject(self, code):
        self.policy_issues[code] += 1
        self.end_window()

    def end_window(self):
        self.active = None
        self.last_coordinates = None

    def consume(self, row, source):
        before = sum(self.issue_counts.values())
        super().consume(row, source)
        if not isinstance(row, dict) or row.get("stream") == "collector":
            return
        kind = row.get("kind")
        if kind == "boot":
            self.end_window()
            self.last_event_ns = None
            self.request_sends.clear()
            if row.get("mode") != 1:
                self.reject("requires_observe_boot")
        if sum(self.issue_counts.values()) != before:
            self.end_window()
            return
        if kind == "health":
            if not integer(row.get("runtime_mode")) or row["runtime_mode"] != 1:
                self.reject("requires_observe_health")
            if not integer(row.get("audit_fault")) or row["audit_fault"] != 0:
                self.reject("requires_explicit_clean_audit_health")
            return
        if kind == "position":
            # The parent auditor holds correlation records; keep that bounded too.
            if len(self.positions) > MAX_REQUESTS:
                self.positions.clear()
                self.reject("request_limit")
            return
        if kind != "send":
            return
        if self.session is None or self.session["boot"] is None:
            self.reject("requires_boot_context")
            return
        now = row["mono_ns"]
        if now < self.session["boot"]["mono_ns"] or (
                self.last_event_ns is not None and now < self.last_event_ns):
            self.reject("nonmonotonic_send_time")
            return
        self.last_event_ns = now
        if row["type"] != 1:
            self.non_location_sends += 1
            # Do not bridge evidence across an unrelated send.
            self.end_window()
            return
        key = (row["call"], row["generation"])
        self.request_sends[key] += 1
        if len(self.request_sends) > MAX_REQUESTS:
            self.request_sends.clear()
            self.reject("request_limit")
            return
        if self.request_sends[key] != 1:
            self.reject("multiple_location_sends_for_request")
            return
        position = self.positions.get(key)
        if position is None or position["mono_ns"] > now or position["mono_ns"] < self.session["boot"]["mono_ns"]:
            self.reject("invalid_position_time_context")
            return
        mode = row["mode"]
        # OBSERVE's mode-0 DISABLED is expected; a fault can also produce it.
        # All-session clean health + parent audit are therefore required below.
        expected_reason = 5 if mode == 0 else 7
        if mode not in (0, 1, 2, 3) or row["choice"] != 0 or row["reason"] != expected_reason:
            self.reject("unsupported_selection_context")
            return
        if row["length"] != 48:
            self.reject("invalid_location_length")
            return
        original = bytes.fromhex(row["original_hex"])
        if len(original) != 48:  # Parent audit normally catches this first.
            self.reject("invalid_location_payload")
            return
        if original[32] not in (0, 1) or original[40] not in (0, 1):
            self.reject("invalid_optional_field_flags")
            return
        if self.active is None or self.active["mode"] != mode or (
                now - self.active["last_observed_mono_ns"] > self.max_gap_ns):
            if len(self.windows) >= MAX_WINDOWS:
                self.reject("window_limit")
                return
            self.active = dict(session_index=len(self.sessions), mode=mode,
                               first_observed_mono_ns=now, last_observed_mono_ns=now,
                               samples=0, repeated_coordinate_pairs=0,
                               zero_wire_timestamp_samples=0,
                               has_speed_samples=0, has_bearing_samples=0,
                               scrub_would_change_samples=0)
            self.windows.append(self.active)
            self.last_coordinates = None
        window = self.active
        window["last_observed_mono_ns"] = now
        window["samples"] += 1
        coordinates = original[8:16]
        window["repeated_coordinate_pairs"] += int(self.last_coordinates == coordinates)
        self.last_coordinates = coordinates
        window["zero_wire_timestamp_samples"] += int(original[:8] == bytes(8))
        window["has_speed_samples"] += original[32]
        window["has_bearing_samples"] += original[40]
        window["scrub_would_change_samples"] += int(project_payload(original, mode, "SCRUB") != original)
        self.location_results.setdefault(str(mode), Counter())[str(row["result"])] += 1

    def replay_report(self):
        audit = super().report()
        status = audit["status"]
        if self.policy_issues and status != "violation":
            status = "inconclusive"
        eligible = status == "local_checks_pass"
        windows = []
        for window in self.windows:
            item = dict(window)
            item["observed_span_seconds"] = (item["last_observed_mono_ns"] - item["first_observed_mono_ns"]) / 1e9
            n = item["samples"]
            item["counterfactual"] = None
            if eligible:
                selected = n if item["mode"] == 0 else 0
                item["counterfactual"] = dict(
                    OBSERVE=dict(forwarded=n, omitted=0, changed_payloads=0),
                    SCRUB=dict(forwarded=n, omitted=0, changed_payloads=item["scrub_would_change_samples"]),
                    DROP=dict(forwarded=n-selected, omitted=selected,
                              omitted_call_return=None, omitted_call_errno=None,
                              omitted_call_side_effects="not_established"))
            windows.append(item)
        if not eligible:
            # Unqualified raw bytes must not become an apparently usable plan.
            for item in windows:
                item.pop("scrub_would_change_samples")
        return dict(
            report_schema=1, status=status, scope="offline_location_selection_hypothesis",
            projection_available=eligible, live_activation_allowed=False,
            phone_fallback="not_established", phone_acceptance="not_established",
            assist_input_qualification="not_established",
            max_gap_ms=self.max_gap_ns / 1e6,
            window_rule="same session and mode; split on unrelated/invalid send or gap; not a GNSS outage detector",
            audit=dict(status=audit["status"], issue_counts=audit["issue_counts"],
                       issue_codes=sorted({issue["code"] for issue in audit["issues"]}),
                       omitted_issue_details=audit["omitted_issue_details"]),
            replay_issue_counts=dict(self.policy_issues),
            location_lower_send_results_by_mode={k: dict(v) for k, v in self.location_results.items()},
            non_location_sends_observed=self.non_location_sends,
            mode3_samples=sum(w["samples"] for w in windows if w["mode"] == 3),
            mode0_samples=sum(w["samples"] for w in windows if w["mode"] == 0),
            windows=windows,
            limitations=[
                "Any audit/replay issue withholds ALL projections; counts remain descriptive only.",
                "Receipt timestamps and equal coordinates do not prove GNSS outage or source staleness.",
                "Window span is last minus first observation; a single sample has zero span.",
                "DROP is a selection hypothesis, not an implemented OEM send/return contract.",
                "Observed return histograms cannot choose a safe DROP return value.",
                "Mode 3 is an observation, not native DR accuracy or phone/app adoption proof.",
                "Only logged LOCATION payloads are replayed; suppressed nested calls cannot be reconstructed.",
                "No coordinates or payload hex are emitted; timing/counters can still be private."])


def replay(paths, max_gap_ms=2500):
    if not math.isfinite(max_gap_ms) or max_gap_ms <= 0 or max_gap_ms > 3600000:
        raise ValueError("max_gap_ms must be finite and in (0, 3600000]")
    auditor = PolicyReplay(int(max_gap_ms * 1e6))
    for path in paths:
        auditor.read_path(path)
    return auditor.replay_report()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help="OBSERVE JSONL, directory or exported TAR; chronological sessions")
    parser.add_argument("--max-gap-ms", type=float, default=2500,
                        help="Heuristic window split only; not a sensor freshness threshold (default: 2500)")
    args = parser.parse_args(argv)
    try:
        report = replay(args.inputs, args.max_gap_ms)
    except ValueError as exc:
        parser.error(str(exc))
    print(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False))
    return {"local_checks_pass": 0, "violation": 1, "inconclusive": 2}[report["status"]]


if __name__ == "__main__":
    sys.exit(main())
