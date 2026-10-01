#!/usr/bin/env python3
"""Offline trace auditor (Python 3, PC only; never extracts tar members).

Exit 0: local checks passed for the recorded window; 1: invariant violation;
2: insufficient/malformed evidence. No exit status establishes phone acceptance,
DR accuracy, or a complete vehicle session. Run --help for input syntax.
"""
import argparse
from collections import Counter
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys
import tarfile

MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024
MAX_LINE_BYTES = 8192
MAX_MEMBERS = 4096
STORAGE_FILES = ('trace.storage.json', 'collector.storage.json')
CLEAR_BYTES = (32, 36, 37, 38, 39, 40, 44, 45, 46, 47)
CHOICES = {0: "ORIGINAL", 1: "SCRUBBED", 2: "DR_REPLACEMENT"}
REASONS = ("PASS", "NO_CONTEXT", "NESTED_CALL", "EXTRA_LOCATION", "BAD_LENGTH",
           "DISABLED", "LOCK_BUSY", "NOT_UNKNOWN", "NOT_READY", "EPOCH_MISMATCH",
           "EXPIRED", "BAD_ENCODING", "BAD_PROVENANCE")
# get_snapshot/Pipeline::diagnostic return these query results, not step()
# results such as DUPLICATE. Pipeline status is a separate last-operation value.
SHADOW_RESULTS = ("OK", "E_CONFIG", "E_NO_SEED", "E_CONTEXT", "E_QUALITY",
                  "E_TIME", "E_LIMIT", "E_STALE")
SHADOW_PIPELINES = ("OK", "WAITING", "BAD_INPUT", "LATE", "CLOCK_RESET",
                    "SOURCE_RESET", "OVERFLOW", "MISSING_SENSOR", "CORE_REJECTED", "NO_ANCHOR")
LIMITATIONS = [
    "Only recorded local byte invariants are checked; no complete vehicle-session proof.",
    "Lower send result is not phone receipt, app adoption, or navigation success.",
    "SMDB/owner/receiver polls do not establish source freshness or exact-request provenance.",
    "Connection lifetimes are process-local observed API boundaries, not daemon GUIDs or provider qualification.",
    "An issue-time unique live session is ambient context, not request ownership or phone acceptance.",
    "SHADOW model diagnostics do not establish DR accuracy, ground truth, or ASSIST readiness.",
    "Yaw/wheel calibration and GPS holdout differences are receipt-time MODEL hypotheses only.",
    "Holdout journal structure cannot prove that GPS references were excluded from prediction inputs.",
    "Motion counts cover channel-accepted records; source measurement timing remains unknown.",
    "Collector stops carry no boot ID; matching uses ordered boot boundaries, PID, and monotonic receipt time.",
]


def rotation_key(name):
    p = PurePosixPath(name)
    m = re.fullmatch(r"(trace|collector)\.(\d+)\.jsonl", p.name)
    return (str(p.parent), m[1] if m else p.name, -int(m[2]) if m else 0, p.name)


def strict_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result


def integer(value):
    return type(value) is int


def finite_float(value):
    result = float(value)
    if not math.isfinite(result):
        raise ValueError("Nonfinite JSON number")
    return result


def bounded_int(value, low, high):
    return integer(value) and low <= value <= high


def bus_snapshot(value, source=False):
    if (not isinstance(value, dict) or value.get("result") not in
            (("connected", "unobserved", "transition", "observation_fault", "no_live_connection", "ambiguous")
             if source else ("connected", "disconnected", "unobserved", "transition", "observation_fault")) or
            any(k not in value for k in ("object", "lifetime"))):
        return False
    if value["result"] in ("connected", "disconnected"):
        return (bounded_int(value["object"], 1, 2**32-1) and
                (bounded_int(value["lifetime"], 1, 2**64-1) if value["result"] == "connected"
                 else value["lifetime"] is None))
    return value["object"] is None and value["lifetime"] is None


def session_snapshot(value, basis):
    if (not isinstance(value, dict) or value.get("basis") != basis or
            value.get("result") not in ("observed", "unobserved", "no_live_session",
                                        "transition", "ambiguous", "observation_fault") or
            any(k not in value for k in ("lifetime", "event", "state"))):
        return False
    if "revision" in value:
        if value["result"] in ("observed", "no_live_session", "ambiguous"):
            if not bounded_int(value["revision"], 1 if value["result"] == "observed" else 0, 2**64-1):
                return False
        elif value["revision"] is not None:
            return False
    if value["result"] != "observed":
        return all(value[k] is None for k in ("lifetime", "event", "state"))
    return (bounded_int(value["lifetime"], 1, 2**32-1) and
            ((value["event"] is None and value["state"] is None) or
             (bounded_int(value["event"], 1, 2**32-1) and
              bounded_int(value["state"], -2**31, 2**31-1))) and
            ("revision" not in value or
             value["revision"] >= value["lifetime"] + (value["event"] or 0)))


def finite_number(value):
    return type(value) in (int, float) and (type(value) is int or math.isfinite(value))


def bounded_number(value, low, high):
    return finite_number(value) and low <= value <= high


def add_difference(stats, value):
    """Bounded-memory summary; online mean avoids an overflowing error sum."""
    stats['count'] += 1
    stats['min'] = value if stats['min'] is None else min(stats['min'], value)
    stats['max'] = value if stats['max'] is None else max(stats['max'], value)
    stats['mean'] = value if stats['mean'] is None else stats['mean'] + (value - stats['mean']) / stats['count']


def decode_motion_records(row):
    """Expand journal v1 without promoting receipt timestamps to producer time.

    Validate the entire batch before returning any records. Integers stay Python
    integers, including uint64 values larger than a JSON/JavaScript double.
    """
    if row.get("producer_time_status") != "unknown":
        raise ValueError("motion producer_time_status must remain unknown")
    if not bounded_int(row.get("epoch"), 1, 2**64 - 1):
        raise ValueError("invalid motion epoch")
    if row.get("kind") == "motion_batch":
        if not integer(row.get("schema")) or row["schema"] != 1:
            raise ValueError("unsupported motion_batch schema")
        events = row.get("events")
        if not isinstance(events, list) or not 1 <= len(events) <= 32:
            raise ValueError("motion_batch must contain 1..32 events")
    elif row.get("kind") == "motion":
        raw = row.get("raw")
        if not isinstance(raw, list) or len(raw) != 4:
            raise ValueError("motion raw must contain four integers")
        events = [[row.get("sensor"), row.get("receive_seq"), row.get("received_ns"),
                   row.get("source_mono_ms"), *raw, row.get("count"), row.get("reverse")]]
    else:
        raise ValueError("expected motion or motion_batch")
    bounds = [(1, 3), (1, 2**64 - 1), (1, 2**64 - 1), (-2**63, 2**63 - 1)]
    bounds += [(0, 65535)] * 6
    result = []
    for event in events:
        if not isinstance(event, list) or len(event) != 10 or any(
                not bounded_int(v, lo, hi) for v, (lo, hi) in zip(event, bounds)):
            raise ValueError("invalid motion row shape, integer type, or range")
        result.append(dict(kind="motion", sensor=event[0], epoch=row["epoch"],
                           receive_seq=event[1], received_ns=event[2], source_mono_ms=event[3],
                           producer_time_status="unknown", raw=event[4:8],
                           count=event[8], reverse=event[9]))
    return result


class Auditor:
    def __init__(self):
        self.counts = Counter()
        self.modes = Counter()
        self.poll_modes = Counter()
        self.choices = Counter()
        self.reasons = Counter()
        self.results = Counter()
        self.request_results = Counter()
        self.request_reply_types = Counter()
        self.request_errors = Counter()
        self.owners = Counter()
        self.receivers = Counter()
        self.runtime_modes = Counter()
        self.audit_faults = Counter()
        self.installs = Counter()
        self.boots = []
        self.health = []
        self.issues = []
        self.issue_counts = Counter()
        self.files = []
        self.ignored = []
        self.total_bytes = 0
        self.session = None
        self.sessions = []
        self.positions = {}
        self.checked = 0
        self.collector_counts = Counter()
        self.collector_boots = []
        self.collector_stops = []
        self.storage_stops = []
        self.collector_pids = set()
        self.collector_session = None
        self.collector_sessions = []
        self.collector_orphan_pid = None
        self.motion_samples = 0
        self.motion_sensors = Counter()
        self.motion_batches = 0
        self.shadow_pipelines = Counter()
        self.shadow_results = Counter()
        self.shadow_states = Counter()
        self.shadow_valid = Counter()
        self.shadow_resets_max = 0
        self.shadow_rejected_max = 0
        self.calibration_states = Counter()
        self.calibration_enabled = Counter()
        self.calibration_candidates = Counter()
        self.calibration_versions = Counter()
        self.calibration_samples_max = 0
        self.wheel_scales = dict(count=0, min=None, max=None, mean=None)
        self.wheel_versions = Counter()
        self.gps_anchor_gates = Counter()
        self.holdout_events = Counter()
        self.holdout_reasons = Counter()
        self.holdout_completed = 0
        self.holdout_aborted = 0
        self.motion_rejected_reasons = Counter()
        self.motion_rejected_sensors = Counter()
        self.capture_ends = 0
        self.holdout_position = dict(count=0, min=None, max=None, mean=None)
        self.holdout_heading = dict(count=0, min=None, max=None, mean=None)

    def issue(self, code, source, detail, violation=False):
        severity = "violation" if violation else "inconclusive"
        self.issue_counts[severity] += 1
        if len(self.issues) < 100:
            self.issues.append(dict(severity=severity, code=code, source=source, detail=detail))

    def new_session(self, boot=None):
        self.session = dict(boot=boot, last_send_ns=-1, health_ns=-1, sends=0,
                            dropped_max=0, health_records=0, motion_epoch=None,
                            motion_seq=0, motion_ns=0, last_diagnostic_ns=-1,
                            shadow_resets=0, shadow_rejected=0, shadow_pipeline=None,
                            holdout_window=None, capture_end_ns=None, model_session=None, model_bus=None)
        self.sessions.append(self.session)
        self.positions = {}
        self.bus_lifetimes = {}
        self.bus_objects = {}
        self.bus_health_counts = {}
        self.bus_capacity = None
        self.bus_contexts_max = 0
        self.model_bus_coherent = None
        self.model_bus_lifetimes = {}
        self.model_bus_seen_source = False

    def validate(self, row, source, ints=(), strings=(), bools=()):
        bad = [k for k in ints if not integer(row.get(k))]
        bad += [k for k in strings if not isinstance(row.get(k), str)]
        bad += [k for k in bools if type(row.get(k)) is not bool]
        if bad:
            self.issue("partial_record", source, "Missing/invalid fields: " + ", ".join(bad))
            return False
        return True

    def position_numbers(self, row, source):
        fields = ("lat", "lon", "heading", "kmh")
        if any(k not in row or (row[k] is not None and type(row[k]) not in (int, float)) for k in fields):
            self.issue("partial_record", source, "Position numeric fields absent or invalid")

    def collector_continuation(self, row, source):
        """Only the current explicit boot boundary can own a subsequent row."""
        s = self.collector_session
        pid, observed = row["collector_pid"], row["observed_at_mono_ns"]
        if s is None:
            if self.collector_orphan_pid != pid:
                self.issue("collector_missing_boot", source,
                           "Collector rotation/start boundary is missing for PID %d" % pid)
                self.collector_orphan_pid = pid
            return False
        if pid != s["pid"]:
            self.issue("collector_session_mismatch", source,
                       "Record PID does not match the current collector boot")
            return False
        if s["stop_ns"] is not None:
            self.issue("collector_record_after_stop", source,
                       "Collector record follows this session's terminal record")
            return False
        if observed < s["last_ns"]:
            self.issue("collector_clock_regressed", source,
                       "Collector receipt time regressed within a session")
            return False
        s["last_ns"] = observed
        return True

    def collector_record(self, row, source):
        """Return whether a polling row still needs its ordinary field checks."""
        kind = row["kind"]
        self.collector_counts[kind] += 1
        if not self.validate(row, source, ("collector_pid", "observed_at_mono_ns"),
                             ("producer_time_status",)):
            return False
        if (not bounded_int(row["collector_pid"], 1, 2**31 - 1) or
                not bounded_int(row["observed_at_mono_ns"], 1, 2**64 - 1)):
            self.issue("partial_record", source, "Invalid collector PID or receipt time")
            return False
        self.collector_pids.add(row["collector_pid"])
        if ("producer_mono_ns" not in row or row["producer_mono_ns"] is not None or
                row["producer_time_status"] != "unknown"):
            self.issue("unexpected_poll_qualification", source,
                       "Collector cannot establish producer measurement time", True)
        if kind == "collector_boot":
            previous = self.collector_session
            self.collector_session = None
            self.collector_orphan_pid = None
            self.collector_boots.append(row)
            if not self.validate(row, source, ("schema", "sample_ms", "session_seconds"), ("boot_id",)):
                return False
            if (not bounded_int(row["schema"], 1, 1) or
                    not bounded_int(row["sample_ms"], 1, 2**32 - 1) or
                    not bounded_int(row["session_seconds"], 1, 86400)):
                self.issue("partial_record", source, "Invalid collector boot schema or timing")
                return False
            if not re.fullmatch(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", row["boot_id"]):
                self.issue("collector_boot_identity_unavailable", source, "Collector boot ID is not available")
                return False
            observed = row["observed_at_mono_ns"]
            if previous and previous["boot_id"] == row["boot_id"] and observed < previous["last_ns"]:
                self.issue("collector_clock_regressed", source,
                           "Collector receipt time regressed within the same kernel boot")
            self.collector_session = dict(boot_id=row["boot_id"], pid=row["collector_pid"],
                                          start_ns=observed, last_ns=observed, stop_ns=None,
                                          source=source)
            self.collector_sessions.append(self.collector_session)
            return False
        if kind == "collector_stop":
            self.collector_stops.append(row)
            if not self.validate(row, source, ("samples",), ("reason",)):
                return False
            if not bounded_int(row["samples"], 0, 2**64 - 1) or not row["reason"]:
                self.issue("partial_record", source, "Invalid collector stop counter or reason")
                return False
            if self.collector_continuation(row, source):
                self.collector_session["stop_ns"] = row["observed_at_mono_ns"]
            return False
        if kind not in ("poll", "position_poll", "position_poll_error", "owner_poll", "receiver_poll"):
            self.issue("unexpected_collector_record", source, "Collector cannot emit AA hook/health records", True)
            return False
        self.collector_continuation(row, source)
        return True

    def consume(self, row, source):
        if not isinstance(row, dict) or not isinstance(row.get("kind"), str):
            self.issue("partial_record", source, "Expected object with kind")
            return
        kind = row["kind"]
        self.counts[kind] += 1
        if kind == 'storage_stop':
            # These fixed diagnostics are separate from the failed journal,
            # including the collector's usual envelope. Never infer a complete
            # session from the last healthy row preceding a storage stop.
            if not self.validate(row, source,
                                 ('pid', 'mono_ns', 'reserve_bytes', 'margin_bytes', 'syscall_errno'),
                                 ('stream', 'boot_id', 'reason')):
                return
            if (row['stream'] not in ('trace', 'collector') or
                    row['reason'] not in ('low_space', 'space_query_failed', 'space_info_invalid') or
                    any(not bounded_int(row[key], 0, 2**64-1) for key in
                        ('pid', 'mono_ns', 'reserve_bytes', 'margin_bytes', 'syscall_errno')) or
                    'available_bytes' not in row or
                    (row['available_bytes'] is not None and
                     not bounded_int(row['available_bytes'], 0, 2**64-1))):
                self.issue('partial_record', source, 'Invalid storage stop diagnostic')
                return
            self.storage_stops.append(row)
            self.issue('storage_stopped', source, row['stream'] + ': ' + row['reason'])
            return
        collector = row.get("stream") == "collector"
        if collector and not self.collector_record(row, source):
            return
        if kind == "boot":
            self.new_session(row)
            self.boots.append(row)
            if not self.validate(row, source, ("schema", "pid", "mono_ns", "mode"),
                                 ("install", "assist_block"), ("assist_ready", "wire_timestamp_modified")):
                return
            self.installs[row["install"]] += 1
            if row["schema"] != 1:
                self.issue("unsupported_schema", source, str(row["schema"]))
            if row["mode"] not in (1, 2, 4):
                self.issue("unexpected_boot_mode", source, "Live worker requires OBSERVE=1, SCRUB=2, or SHADOW=4", True)
            if row["install"] != "ok":
                self.issue("install_not_ok", source, row["install"])
            if row["assist_ready"] or row["wire_timestamp_modified"]:
                self.issue("impossible_live_capability", source, "Boot claims unsupported live capability", True)
            return
        if self.session is None and not collector:
            self.new_session()
        if (not collector and self.session.get("capture_end_ns") is not None and
                kind not in ("health", "capture_end")):
            self.issue("record_after_capture_end", source, kind)
        if kind == "position":
            self.request_record(row, source, count=True)
            if not self.validate(row, source, ("call", "generation", "mono_ns", "mode", "utc_s")):
                return
            self.position_numbers(row, source)
            self.modes[str(row["mode"])] += 1
            key = (row["call"], row["generation"])
            if key in self.positions:
                self.issue("duplicate_position", source, "Ambiguous call/generation correlation")
            self.positions[key] = row
        elif kind == "send":
            self.send(row, source)
        elif kind == "health":
            if not self.validate(row, source, ("mono_ns", "dropped"), bools=("hook_installed", "assist_ready")):
                return
            self.health.append(row)
            s = self.session
            s["health_records"] += 1
            s["health_ns"] = max(s["health_ns"], row["mono_ns"])
            if row["dropped"] < s["dropped_max"] or row["dropped"] < 0:
                self.issue("drop_counter_regressed", source, "Cumulative drop count decreased")
            s["dropped_max"] = max(s["dropped_max"], row["dropped"])
            if row["dropped"]:
                self.issue("dropped_observations", source, str(row["dropped"]))
            if not row["hook_installed"]:
                self.issue("hook_not_installed", source, "Health reports no installed hook")
            if row["assist_ready"]:
                self.issue("impossible_live_capability", source, "Health claims unsupported ASSIST readiness", True)
            for key, counter in (("runtime_mode", self.runtime_modes), ("audit_fault", self.audit_faults)):
                if key in row:
                    if not integer(row[key]):
                        self.issue("partial_record", source, key + " must be an integer")
                    else:
                        counter[str(row[key])] += 1
            if integer(row.get("audit_fault")) and row["audit_fault"] != 0:
                self.issue("audit_fault", source, "Runtime disabled mutation because audit logging failed")
            if "request_observer" in row:
                observer = row["request_observer"]
                if (not isinstance(observer, dict) or
                        any(not isinstance(observer.get(k), bool) for k in ("prepared", "abi_fault", "exhausted")) or
                        any(not integer(observer.get(k)) or observer[k] < 0
                            for k in ("loss_epoch", "requests", "workers", "loss_reasons")) or
                        not isinstance(observer.get("result"), str)):
                    self.issue("request_observer_malformed", source, "Invalid request observation health")
                elif (not observer["prepared"] or observer["result"] != "observed"):
                    self.issue("request_observer_unavailable", source, "No current request observation health")
                elif observer["abi_fault"] or observer["exhausted"] or observer["loss_reasons"]:
                    self.issue("request_observer_loss", source, "Request association has incomplete lifetime evidence")
            if "session_observer" in row:
                observer = row["session_observer"]
                if (not isinstance(observer, dict) or not isinstance(observer.get("prepared"), bool) or
                        not bounded_int(observer.get("capacity"), 1, 2**32-1) or
                        not bounded_int(observer.get("contexts"), 0, observer["capacity"]) or
                        not bounded_int(observer.get("faults"), 0, 2**32-1)):
                    self.issue("session_observer_malformed", source, "Invalid session observation health")
                elif not observer["prepared"] or observer["faults"]:
                    self.issue("session_observer_unavailable", source, "Session observation is unavailable or incomplete")
            if "bus_observer" in row:
                observer = row["bus_observer"]
                if (not isinstance(observer, dict) or not isinstance(observer.get("prepared"), bool) or
                        not bounded_int(observer.get("capacity"), 1, 2**32-1) or
                        not bounded_int(observer.get("contexts"), 0, observer["capacity"]) or
                        not bounded_int(observer.get("faults"), 0, 2**32-1)):
                    self.issue("bus_observer_malformed", source, "Invalid bus observation health")
                else:
                    self.bus_health_record(observer, row["mono_ns"], source)
                    if not observer["prepared"] or observer["faults"]:
                        self.issue("bus_observer_unavailable", source, "Bus observation is unavailable or incomplete")
        elif kind == "owner_poll":
            if self.validate(row, source, ("receipt_ns", "pid"), ("owner", "comm"), ("request_provenance",)):
                self.owners[(row["owner"], row["pid"], row["comm"])] += 1
                if row["request_provenance"]:
                    self.issue("unsupported_provenance", source, "Poll cannot prove exact request", True)
        elif kind == "receiver_poll":
            if self.validate(row, source, ("receipt_ns", "receiver")):
                self.receivers[str(row["receiver"])] += 1
        elif kind == "position_poll":
            if self.validate(row, source, ("receipt_ns", "mode", "utc_s"), bools=("request_provenance",)):
                self.position_numbers(row, source)
                self.poll_modes[str(row["mode"])] += 1
                if row["request_provenance"]:
                    self.issue("unsupported_provenance", source, "Poll cannot prove exact request", True)
        elif kind == "poll":
            if self.validate(row, source, ("seq", "begin_ns", "end_ns"),
                             ("speed_raw", "yaw_raw", "gear_raw", "freshness", "quality"), ("assist_ready",)):
                if row["assist_ready"] or row["freshness"] != "unproven_poll" or row["quality"] != "unknown":
                    self.issue("unexpected_poll_qualification", source,
                               "Polling cannot establish ASSIST readiness, freshness, or quality", True)
        elif kind == "position_poll_error":
            self.validate(row, source, strings=("reason",))
        elif kind == "capture_end":
            self.capture_end(row, source)
        elif kind == "motion_rejected":
            self.rejected_motion(row, source)
        elif kind in ("motion", "motion_batch"):
            try:
                events = decode_motion_records(row)
            except ValueError as exc:
                self.issue("malformed_motion", source, str(exc))
                return
            if kind == "motion_batch":
                self.motion_batches += 1
            for event in events:
                self.motion(event, source)
        elif kind in ("shadow_boot", "shadow", "shadow_input_reset", "shadow_disabled", "shadow_pipeline_reset"):
            self.shadow(row, source)
        elif kind == "shadow_bus":
            self.model_bus(row, source)
        elif kind in ("shadow_session", "shadow_position_rejected", "shadow_motion_excluded"):
            self.model_session(row, source)
        elif kind == "shadow_calibration":
            self.calibration(row, source)
        elif kind == "shadow_holdout":
            self.holdout(row, source)
        else:
            self.issue("unknown_record_kind", source, kind)

    def capture_end(self, row, source):
        if row.get("domain") != "model" or row.get("assist_ready") is not False:
            self.issue("unexpected_capture_qualification", source,
                       "Capture completion is not navigation qualification", True)
            return
        boot = self.session.get("boot") or {}
        if (not bounded_int(row.get("schema"), 1, 1) or
                not bounded_int(row.get("mono_ns"), 1, 2**64-1) or
                not bounded_int(row.get("cutoff_ns"), 1, row["mono_ns"]) or
                row.get("bounded_final_drain") is not True or
                row.get("reason") != "requested" or
                not isinstance(row.get("boot_id"), str) or
                not re.fullmatch(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", row["boot_id"]) or
                row["boot_id"] != boot.get("boot_id")):
            self.issue("malformed_capture_end", source, "Missing/current-boot completion metadata")
            return
        s = self.session
        if s["capture_end_ns"] is not None or row["mono_ns"] < s["last_diagnostic_ns"]:
            self.issue("invalid_capture_end_order", source, "Duplicate or regressed completion")
            return
        s["capture_end_ns"] = row["mono_ns"]
        s["last_diagnostic_ns"] = max(s["last_diagnostic_ns"], row["mono_ns"])
        self.capture_ends += 1

    def rejected_motion(self, row, source):
        # Preserve diagnostics without healing accepted sequence gaps or feeding
        # the replay engine. A decoded payload is not an accepted sensor sample.
        if (row.get("domain") != "model" or row.get("assist_ready") is not False or
                row.get("producer_time_status") != "unknown" or
                row.get("authenticated_decoded") is not True):
            self.issue("unexpected_rejected_motion_qualification", source,
                       "Rejected input cannot authorize navigation", True)
            return
        if (not bounded_int(row.get("schema"), 1, 1) or
                row.get("reason") not in ("clock_unavailable", "future", "stale",
                                          "source_changed", "sequence_discontinuity") or
                not bounded_int(row.get("checked_ns"), 0, 2**64-1) or
                not bounded_int(row.get("sender_pid"), 1, 2**31-1) or
                not bounded_int(row.get("sender_uid"), 0, 2**32-1)):
            self.issue("malformed_rejected_motion", source, "Invalid rejection metadata")
            return
        try:
            accepted_shape = dict(row, kind="motion")
            event = decode_motion_records(accepted_shape)[0]
        except ValueError as exc:
            self.issue("malformed_rejected_motion", source, str(exc))
            return
        self.motion_rejected_reasons[row["reason"]] += 1
        self.motion_rejected_sensors[str(event["sensor"])] += 1
        # A future raw timestamp must not become the journal's time frontier.
        s = self.session
        s["last_diagnostic_ns"] = max(s["last_diagnostic_ns"], row["checked_ns"])
        self.issue("motion_channel_rejected", source, row["reason"])

    def motion(self, row, source):
        self.motion_samples += 1
        self.motion_sensors[str(row["sensor"])] += 1
        s = self.session
        s["last_diagnostic_ns"] = max(s["last_diagnostic_ns"], row["received_ns"])
        if s["motion_epoch"] != row["epoch"]:
            if s["motion_epoch"] is not None:
                self.issue("motion_source_restart", source, "Observed source epoch changed")
            s["motion_epoch"] = row["epoch"]
            s["motion_seq"] = row["receive_seq"]
            s["motion_ns"] = row["received_ns"]
            return  # First sequence need not be 1: the receiver can start late.
        if row["receive_seq"] <= s["motion_seq"]:
            self.issue("motion_sequence_replayed", source, "Duplicate/backward observer sequence")
        elif row["receive_seq"] != s["motion_seq"] + 1:
            self.issue("motion_sequence_gap", source, "Gap in channel-accepted observer records")
        if row["received_ns"] < s["motion_ns"]:
            self.issue("motion_clock_regressed", source, "Receipt clock regressed within source epoch")
        s["motion_seq"] = max(s["motion_seq"], row["receive_seq"])
        s["motion_ns"] = max(s["motion_ns"], row["received_ns"])

    def model_bus(self, row, source):
        if not self.model_diagnostic(row, source):
            return
        observed = row.get('connection')
        previous = self.session['model_bus']
        if (not bus_snapshot(observed, source=True) or
                type(row.get('reset')) is not bool or type(row.get('input_available')) is not bool or
                row['input_available'] != (observed['result'] == 'connected') or
                not bounded_int(row.get('model_bus_epoch'), 1, 2**64-1) or
                not bounded_int(row.get('bus_revision'), 1 if row['input_available'] else 0, 2**64-1) or
                (observed['result'] in ('transition', 'observation_fault') and row['bus_revision'] != 0) or
                not bounded_int(row.get('raw_since_ns'), 1, row['mono_ns']) or
                row['raw_since_ns'] != row['mono_ns'] or
                (previous is None and (row['reset'] or row['model_bus_epoch'] != 1)) or
                (previous is not None and (not row['reset'] or
                    row['model_bus_epoch'] != previous['model_bus_epoch'] + 1 or
                    row['raw_since_ns'] < previous['raw_since_ns']))):
            self.issue('model_bus_malformed', source, 'Invalid MODEL bus boundary')
            return
        result, revision = observed['result'], row['bus_revision']
        # A coherent marked source includes completed create/connect calls and
        # at least one source-marking mutation. Additional/failed/concurrent
        # calls may increase the revision; these are lower bounds, not equality.
        minimum = (observed['object'] + observed['lifetime'] + 1 if result == 'connected'
                   else 4 if result == 'no_live_connection' else 6 if result == 'ambiguous' else 0)
        if revision < minimum:
            self.issue('model_bus_malformed', source, 'Bus revision cannot contain its observed lifecycle')
            return
        coherent = self.model_bus_coherent
        stable = result not in ('transition', 'observation_fault')
        unchanged = previous is not None and (revision, observed) == (
            previous['bus_revision'], previous['connection'])
        if (unchanged or
                (previous is not None and previous['connection']['result'] == 'observation_fault') or
                (self.model_bus_seen_source and result == 'unobserved') or
                (stable and coherent is not None and (
                    revision < coherent['bus_revision'] or
                    (revision == coherent['bus_revision'] and (
                        observed != coherent['connection'] or
                        (previous is not None and previous['connection']['result'] == 'transition'))))) or
                (result == 'connected' and observed['lifetime'] <
                 self.model_bus_lifetimes.get(observed['object'], 0))):
            self.issue('model_bus_history_inconsistent', source,
                       'Worker-ordered MODEL bus boundary contradicts prior observation', True)
            return
        if stable:
            self.model_bus_coherent = row
        if result in ('connected', 'no_live_connection', 'ambiguous'):
            self.model_bus_seen_source = True
        if result == 'connected':
            self.model_bus_lifetimes[observed['object']] = observed['lifetime']
        # The worker clock follows its snapshot; use it only as an existence
        # upper bound, preserving delayed request snapshot semantics.
        self.bus_connection_record(observed, row['mono_ns'], row['mono_ns'], source)
        if self.session['holdout_window'] is not None:
            self.issue('holdout_crosses_bus_boundary', source,
                       'MODEL bus boundary did not terminate the open holdout window', True)
            self.session['holdout_window'] = None
        self.session['model_bus'] = row
        if row['reset']:
            self.issue('shadow_bus_reset', source, observed['result'])
        elif not row['input_available']:
            self.issue('bus_observation_unavailable', source, observed['result'])

    def model_session(self, row, source):
        if not self.model_diagnostic(row, source):
            return
        if row['kind'] == 'shadow_motion_excluded':
            if (not all(bounded_int(row.get(k), 1, high) for k, high in
                    (('raw_since_ns', row['mono_ns']), ('sensor', 3), ('epoch', 2**64-1),
                     ('receive_seq', 2**64-1), ('received_ns', row['mono_ns']))) or
                    not bounded_int(row.get('source_mono_ms'), -2**63, 2**63-1) or
                    row.get('reason') not in ('receipt_before_session', 'transport_before_session',
                                              'receipt_before_bus', 'transport_before_bus')):
                self.issue('model_session_malformed', source, 'Invalid excluded MODEL motion')
                return
            boundary = self.session['model_bus' if row['reason'].endswith('_bus') else 'model_session']
            session, bus = self.session['model_session'], self.session['model_bus']
            if session is not None and bus is not None:
                selected = bus if bus['raw_since_ns'] > session['raw_since_ns'] else session
                if not session['input_available'] or not bus['input_available'] or boundary is not selected:
                    self.issue('model_session_malformed', source,
                               'Motion exclusion does not use the available, later MODEL boundary')
                    return
            transport = row['source_mono_ms'] * 1000000
            if (boundary is None or row['raw_since_ns'] != boundary['raw_since_ns'] or
                    not boundary['input_available'] or
                    (row['reason'].startswith('receipt_before_') and
                     row['received_ns'] >= row['raw_since_ns']) or
                    (row['reason'].startswith('transport_before_') and not
                     (row['received_ns'] >= row['raw_since_ns'] and
                      0 < transport < row['raw_since_ns']))):
                self.issue('model_session_malformed', source, 'Motion exclusion contradicts its boundary')
                return
            self.issue('shadow_motion_excluded', source, row['reason'])
            return
        if row["kind"] == "shadow_position_rejected":
            if (not all(bounded_int(row.get(k), 0, high) for k, high in
                    (("call", 2**32-1), ("generation", 2**32-1), ("session_revision", 2**64-1))) or
                    row.get("reason") not in ("session_unavailable", "request_unobserved",
                                             "session_changed_since_issue", "request_time_order",
                                             "bus_unavailable", "request_bus_unobserved",
                                             "bus_changed_since_issue", "request_before_bus_boundary")):
                self.issue("model_session_malformed", source, "Invalid rejected MODEL position")
                return
            if (self.session['model_bus'] is not None or 'bus' in row['reason'] or
                    'model_bus_epoch' in row or 'bus_revision' in row):
                boundary = self.session['model_bus']
                if (boundary is None or not bounded_int(row.get('model_bus_epoch'), 1, 2**64-1) or
                        not bounded_int(row.get('bus_revision'), 0, 2**64-1) or
                        row['model_bus_epoch'] != boundary['model_bus_epoch'] or
                        row['bus_revision'] != boundary['bus_revision'] or row['mono_ns'] < boundary['mono_ns']):
                    self.issue('model_bus_malformed', source, 'Rejected position has inconsistent bus boundary')
                    return
                if ('bus' in row['reason'] and
                        (row['reason'] == 'bus_unavailable') == boundary['input_available']):
                    self.issue('model_bus_malformed', source, 'Rejection reason contradicts bus availability')
                    return
            boundary = self.session['model_session']
            if boundary is not None and (
                    row['session_revision'] != (boundary['session']['revision'] or 0) or
                    row['mono_ns'] < boundary['mono_ns'] or
                    ('bus' in row['reason'] and not boundary['input_available'])):
                self.issue('model_session_malformed', source, 'Rejected position has inconsistent session boundary')
                return
            self.issue("shadow_position_rejected", source, row["reason"])
            return
        previous = self.session["model_session"]
        observed = row.get("session")
        if (not session_snapshot(observed, "unique_live_context") or "revision" not in observed or
                type(row.get("reset")) is not bool or type(row.get("input_available")) is not bool or
                row["input_available"] != (observed["result"] == "observed") or
                not bounded_int(row.get("model_session_epoch"), 1, 2**64-1) or
                not bounded_int(row.get("raw_since_ns"), 1, row["mono_ns"]) or
                row['raw_since_ns'] != row['mono_ns'] or
                (previous is None and (row["reset"] or row["model_session_epoch"] != 1)) or
                (previous is not None and (not row["reset"] or
                    row["model_session_epoch"] != previous["model_session_epoch"] + 1 or
                    row["raw_since_ns"] < previous["raw_since_ns"]))):
            self.issue("model_session_malformed", source, "Invalid MODEL lifecycle boundary")
            return
        self.session["model_session"] = row
        if row["reset"]:
            self.issue("shadow_session_reset", source, observed["result"])
        elif not row["input_available"]:
            self.issue("session_observation_unavailable", source, observed["result"])

    def shadow(self, row, source):
        if not self.validate(row, source, bools=("assist_ready",)):
            return
        if row["assist_ready"]:
            self.issue("impossible_live_capability", source, "SHADOW cannot authorize ASSIST", True)
        kind = row["kind"]
        if kind == 'shadow_pipeline_reset':
            if (not self.validate(row, source,
                    ints=('mono_ns', 'input_ns', 'receive_seq', 'sensor', 'call', 'resets'),
                    strings=('reason', 'operation'))):
                return
            faults = ('BAD_INPUT', 'LATE', 'CLOCK_RESET', 'SOURCE_RESET', 'OVERFLOW',
                      'MISSING_SENSOR', 'CORE_REJECTED')
            if (row.get('domain') != 'model' or row['reason'] not in faults or
                    row['operation'] not in ('raw', 'position', 'drain') or
                    any(not bounded_int(row[key], 0, 2**64-1) for key in
                        ('mono_ns', 'input_ns', 'receive_seq', 'sensor', 'call', 'resets')) or
                    not row['mono_ns'] or not row['resets'] or
                    (row['operation'] == 'raw' and
                     (row['sensor'] not in (1, 2, 3) or not row['receive_seq']))):
                self.issue('partial_record', source, 'Invalid primary MODEL reset diagnostic')
                return
            self.issue(kind, source, '%s: %s receive_seq=%d' %
                       (row['operation'], row['reason'], row['receive_seq']))
            return
        if kind in ("shadow_input_reset", "shadow_disabled"):
            self.validate(row, source, strings=("reason",))
            self.issue(kind, source, str(row.get("reason")))
            return  # A reset marker does not heal a missing observer sequence.
        if row.get("domain") != "model":
            self.issue("unexpected_shadow_domain", source, "SHADOW must remain MODEL", True)
        if kind == "shadow_boot":
            if self.validate(row, source, strings=("source",), bools=("active",)) and not row["active"]:
                self.issue("shadow_inactive", source, "Sensor SHADOW did not start")
            if "motion_sampling" in row and row["motion_sampling"] is not False:
                self.issue("motion_sampling", source, "Lossless motion logging not established")
            if "capture_active" in row:
                if type(row["capture_active"]) is not bool:
                    self.issue("partial_record", source, "capture_active must be boolean")
                elif not row["capture_active"]:
                    self.issue("motion_capture_inactive", source, "Raw receiver did not start")
            return
        boundary = self.session["model_session"]
        if boundary is not None or "model_session_epoch" in row or "session_revision" in row:
            if (boundary is None or
                    not bounded_int(row.get("model_session_epoch"), 1, 2**64-1) or
                    not bounded_int(row.get("session_revision"), 0, 2**64-1) or
                    row["model_session_epoch"] != boundary["model_session_epoch"] or
                    row["session_revision"] != (boundary["session"]["revision"] or 0) or
                    (row.get("model_valid") is True and not boundary["input_available"])):
                self.issue("shadow_session_mismatch", source, "MODEL snapshot does not match its observed boundary")
        bus = self.session['model_bus']
        if bus is not None or 'model_bus_epoch' in row or 'bus_revision' in row:
            if (bus is None or not bounded_int(row.get('model_bus_epoch'), 1, 2**64-1) or
                    not bounded_int(row.get('bus_revision'), 0, 2**64-1) or
                    row['model_bus_epoch'] != bus['model_bus_epoch'] or
                    row['bus_revision'] != bus['bus_revision'] or
                    (row.get('model_valid') is True and not bus['input_available'])):
                self.issue('shadow_bus_mismatch', source, 'MODEL snapshot does not match its bus boundary')
        if not self.validate(row, source,
                ints=("mono_ns", "state", "uncertainties", "events", "intervals", "resets", "rejected", "frontier_ns"),
                strings=("result", "pipeline", "location_preview_hex"),
                bools=("model_valid", "stopped", "preview_encoded")):
            return
        if (not bounded_int(row['state'], 0, 6) or not bounded_int(row['uncertainties'], 0, 2**32-1) or
                not bounded_int(row['mono_ns'], 1, 2**64-1) or any(
                    not bounded_int(row[key], 0, 2**64-1)
                    for key in ('events', 'intervals', 'resets', 'rejected', 'frontier_ns'))):
            self.issue('partial_record', source, 'SHADOW state/counter/time outside integer range')
            return
        if boundary is not None and (
                row['mono_ns'] < boundary['mono_ns'] or row['frontier_ns'] > row['mono_ns'] or
                (row['model_valid'] and row['frontier_ns'] < boundary['raw_since_ns'])):
            self.issue('shadow_session_time_inconsistent', source,
                       'MODEL time contradicts its observed session boundary or capture time', True)
        if bus is not None and (
                row['mono_ns'] < bus['mono_ns'] or row['frontier_ns'] > row['mono_ns'] or
                (row['model_valid'] and row['frontier_ns'] < bus['raw_since_ns'])):
            self.issue('shadow_bus_time_inconsistent', source,
                       'MODEL time contradicts its observed bus boundary or capture time', True)
        self.session["last_diagnostic_ns"] = max(self.session["last_diagnostic_ns"], row["mono_ns"])
        if not self.wheel_scale_pair(row, source):
            return
        if not self.shadow_result(row, source):
            return
        self.shadow_pipelines[row['pipeline']] += 1
        self.shadow_results[row['result']] += 1
        self.shadow_states[str(row['state'])] += 1
        self.shadow_valid[str(row['model_valid']).lower()] += 1
        self.shadow_resets_max = max(self.shadow_resets_max, row['resets'])
        self.shadow_rejected_max = max(self.shadow_rejected_max, row['rejected'])
        normal = ('OK', 'WAITING', 'MISSING_SENSOR', 'NO_ANCHOR')
        if row['pipeline'] not in normal and self.session['shadow_pipeline'] != row['pipeline']:
            self.issue('shadow_pipeline_fault', source, row['pipeline'])
        self.session['shadow_pipeline'] = row['pipeline']
        for counter in ('resets', 'rejected'):
            if row[counter] > self.session['shadow_' + counter]:
                self.issue('shadow_' + counter, source, 'Observed model input fault counter increased')
            self.session['shadow_' + counter] = row[counter]
        preview = row["location_preview_hex"]
        if (row["preview_encoded"] and (not row['model_valid'] or
                not re.fullmatch(r"[0-9a-fA-F]{96}", preview))) or (
                not row["preview_encoded"] and preview != ""):
                self.issue("invalid_shadow_preview", source, "Preview must match encoded flag and 48-byte shape")

    def shadow_result(self, row, source):
        """Check producer implications without treating an ACTIVE state as valid."""
        if row['result'] not in SHADOW_RESULTS or row['pipeline'] not in SHADOW_PIPELINES:
            self.issue('partial_record', source, 'Unknown MODEL query result or pipeline status')
            return False
        numeric = ('lat', 'lon', 'heading_rad', 'speed_mps', 'error_model_m')
        for key in numeric:
            # json_number emits finite binary64 numbers or null. In particular,
            # a Python integer is not automatically representable as a double.
            if key not in row or (row[key] is not None and not bounded_number(
                    row[key], -sys.float_info.max, sys.float_info.max)):
                self.issue('partial_record', source, 'Invalid SHADOW numeric field: ' + key)
                return False
        contradiction = row['model_valid'] != (row['result'] == 'OK')
        if row['result'] in ('E_TIME', 'E_STALE', 'E_LIMIT'):
            contradiction |= row['state'] != 2 or not row['frontier_ns']
        if row['result'] == 'E_STALE':
            contradiction |= row['frontier_ns'] > row['mono_ns']
        if row['result'] == 'E_CONFIG':
            contradiction |= row['state'] != 0
        # All supported core configurations cap query age at 150 ms. E_LIMIT
        # also follows the stale check; E_STALE may instead be an expired lease.
        if row['result'] in ('OK', 'E_LIMIT'):
            contradiction |= not 0 <= row['mono_ns'] - row['frontier_ns'] <= 150000000
        if row['model_valid']:
            contradiction |= (row['state'] != 2 or not 0 < row['frontier_ns'] <= row['mono_ns'] or
                              not row['events'] or not row['intervals'])
            # Invalid snapshots may retain earlier values, or an error estimate
            # above the configured limit. These solution bounds apply only to OK.
            contradiction |= (not bounded_number(row['lat'], -85, 85) or
                              row['lat'] in (-85, 85) or
                              not bounded_number(row['lon'], -180, 180) or
                              not bounded_number(row['heading_rad'], 0, 2*math.pi) or
                              not bounded_number(row['speed_mps'], 0, sys.float_info.max) or
                              not bounded_number(row['error_model_m'], 0, 100) or
                              (row['stopped'] and row['speed_mps'] != 0))
            # wrap(-tiny) and lon_wrap can round to exactly +2*pi / +180;
            # both are actual finite producer outputs, not malformed JSON.
        if contradiction:
            self.issue('shadow_result_inconsistent', source,
                       'MODEL validity contradicts its query result, state, time or numeric solution', True)
            return False
        return True

    def model_diagnostic(self, row, source):
        """Check the shared envelope without hiding qualification violations."""
        valid = self.validate(row, source, ints=("mono_ns",),
                              strings=("domain",), bools=("assist_ready",))
        if row.get("domain") != "model":
            self.issue("unexpected_shadow_domain", source, "SHADOW must remain MODEL", True)
            valid = False
        if row.get("assist_ready") is True:
            self.issue("impossible_live_capability", source, "SHADOW cannot authorize ASSIST", True)
            valid = False
        if not bounded_int(row.get("mono_ns"), 1, 2**64-1):
            self.issue("partial_record", source, "SHADOW mono_ns outside uint64 range")
            return False
        self.session["last_diagnostic_ns"] = max(self.session["last_diagnostic_ns"], row["mono_ns"])
        return valid

    def wheel_scale_pair(self, row, source):
        # Both fields are absent in older journals; partial additions are invalid.
        if "wheel_scale" not in row and "wheel_scale_version" not in row:
            return True
        if (not bounded_number(row.get("wheel_scale"), 0.95, 1.05) or
                not bounded_int(row.get("wheel_scale_version"), 0, 2**64-1)):
            self.issue("partial_record", source, "Invalid or incomplete wheel scale/version pair")
            return False
        return True

    def calibration(self, row, source):
        envelope = self.model_diagnostic(row, source)
        if not self.validate(row, source,
                ints=("samples", "evidence_start_ns", "evidence_end_ns", "calibration_version"),
                strings=("state",), bools=("enabled", "candidate_ready")):
            return
        if (any(not bounded_int(row[key], 0, 2**64-1) for key in
                ("samples", "evidence_start_ns", "evidence_end_ns", "calibration_version")) or
                any(not bounded_number(row.get(key), 0, 4093) for key in ("active_zero", "candidate_zero")) or
                not bounded_number(row.get("variance_counts2"), 0, sys.float_info.max) or
                not row["state"]):
            self.issue("partial_record", source, "Invalid calibration state, counter, time, or yaw zero")
            return
        if not envelope:
            return
        start, end = row["evidence_start_ns"], row["evidence_end_ns"]
        if ((start == 0) != (end == 0) or start > end or end > row["mono_ns"] or
                (row["candidate_ready"] and (not row["enabled"] or not row["samples"] or not start or start == end))):
            self.issue("invalid_calibration_evidence", source, "Calibration evidence window/candidate is inconsistent")
            return
        wheel_fields = ("wheel_enabled", "wheel_candidate_ready", "wheel_scale", "wheel_candidate_scale",
                        "wheel_scale_version", "wheel_segments", "wheel_gps_distance_m", "wheel_distance_m",
                        "wheel_evidence_end_ns", "gps_anchor_gate")
        if any(key in row for key in wheel_fields):
            if not self.validate(row, source,
                    ints=("wheel_scale_version", "wheel_segments", "wheel_evidence_end_ns"),
                    strings=("gps_anchor_gate",), bools=("wheel_enabled", "wheel_candidate_ready")):
                return
            if not self.wheel_scale_pair(row, source):
                return
            if (not bounded_number(row.get("wheel_candidate_scale"), 0.95, 1.05) or
                    any(not bounded_int(row[key], 0, 2**64-1)
                        for key in ("wheel_segments", "wheel_evidence_end_ns")) or
                    any(not bounded_number(row.get(key), 0, sys.float_info.max)
                        for key in ("wheel_gps_distance_m", "wheel_distance_m")) or
                    not row["gps_anchor_gate"]):
                self.issue("partial_record", source, "Invalid wheel calibration evidence or GPS anchor gate")
                return
            if (row["wheel_evidence_end_ns"] > row["mono_ns"] or
                    (row["wheel_candidate_ready"] and (not row["wheel_enabled"] or
                        row["wheel_segments"] < 3 or row["wheel_gps_distance_m"] < 100 or
                        row["wheel_distance_m"] <= 0 or row["wheel_evidence_end_ns"] == 0))):
                self.issue("invalid_calibration_evidence", source, "Wheel calibration evidence/candidate is inconsistent")
                return
            add_difference(self.wheel_scales, row["wheel_scale"])
            self.wheel_versions[str(row["wheel_scale_version"])] += 1
            self.gps_anchor_gates[row["gps_anchor_gate"]] += 1
        self.calibration_states[row["state"]] += 1
        self.calibration_enabled[str(row["enabled"]).lower()] += 1
        self.calibration_candidates[str(row["candidate_ready"]).lower()] += 1
        self.calibration_versions[str(row["calibration_version"])] += 1
        self.calibration_samples_max = max(self.calibration_samples_max, row["samples"])

    def holdout(self, row, source):
        envelope = self.model_diagnostic(row, source)
        if row.get("time_basis") != "receipt_model":
            self.issue("unexpected_holdout_time_basis", source, "Holdout timestamps must remain receipt-time MODEL", True)
            envelope = False
        if not self.validate(row, source,
                ints=("window_id", "anchor_ns", "reference_ns", "frontier_ns", "calibration_version"),
                strings=("event", "reason"), bools=("model_valid",)):
            return
        event = row["event"]
        if (event not in ("BEGIN", "COMPARED", "END", "ABORT") or
                not bounded_int(row["window_id"], 0 if event == "ABORT" else 1, 2**64-1) or
                any(not bounded_int(row[key], 0, 2**64-1) for key in
                    ("anchor_ns", "reference_ns", "frontier_ns", "calibration_version")) or
                not bounded_number(row.get("yaw_zero"), 0, 4093)):
            self.issue("partial_record", source, "Invalid holdout event, counter, time, or yaw zero")
            return
        if not self.wheel_scale_pair(row, source):
            return
        self.holdout_events[event] += 1
        self.holdout_reasons[row["reason"]] += 1
        numeric_bounds = (("lat", -90, 90), ("lon", -180, 180),
                          ("ref_lat", -90, 90), ("ref_lon", -180, 180),
                          ("position_error_m", 0, sys.float_info.max),
                          ("heading_error_rad", -math.pi, math.pi))
        if any(key not in row or (row[key] is not None and not bounded_number(row[key], low, high))
               for key, low, high in numeric_bounds):
            self.issue("partial_record", source, "Invalid nullable holdout coordinate or difference")
            return
        if not envelope:
            return
        anchor, reference, frontier = row["anchor_ns"], row["reference_ns"], row["frontier_ns"]
        warmup_abort = event == "ABORT" and row["window_id"] == 0
        if ((not anchor and not warmup_abort) or max(anchor, reference, frontier) > row["mono_ns"] or
                (warmup_abort and (anchor or reference or frontier))):
            self.issue("invalid_holdout_time", source, "Holdout timestamp exceeds diagnostic time or anchor is absent")
            return
        if row['reason'] == 'bus_reset' and event != 'ABORT':
            self.issue('invalid_holdout_boundary', source, 'Bus reset can only abort a holdout window')
            return
        for boundary in (s for s in (self.session['model_bus'], self.session['model_session']) if s is not None):
            if (row['mono_ns'] < boundary['mono_ns'] or (event != 'ABORT' and (
                    not boundary['input_available'] or anchor < boundary['raw_since_ns']))):
                self.issue('invalid_holdout_boundary', source,
                           'Holdout output contradicts its available MODEL input boundary', True)
                return
        if event == "COMPARED":
            if not (row["model_valid"] and anchor < reference == frontier):
                self.issue("invalid_holdout_comparison", source, "Comparison requires MODEL output at the later GPS receipt time")
                return
            if any(row[key] is None for key in ("lat", "lon", "ref_lat", "ref_lon", "position_error_m")):
                self.issue("invalid_holdout_comparison", source, "Comparison requires finite coordinates and position difference")
                return
        elif row["position_error_m"] is not None or row["heading_error_rad"] is not None:
            self.issue("invalid_holdout_comparison", source, "Only COMPARED records may report differences")
            return
        s = self.session
        window = s["holdout_window"]
        if event == "BEGIN":
            if reference != anchor or frontier != anchor:
                self.issue("invalid_holdout_time", source, "Holdout BEGIN must align reference and frontier with its anchor")
                return
            if window is not None:
                self.issue("holdout_unfinished_window", source, "New BEGIN precedes prior window END/ABORT")
            s["holdout_window"] = dict(window_id=row["window_id"], anchor_ns=anchor,
                                       last_reference_ns=anchor, mono_ns=row["mono_ns"],
                                       calibration_version=row["calibration_version"], yaw_zero=row["yaw_zero"],
                                       wheel_scale=row.get("wheel_scale"),
                                       wheel_scale_version=row.get("wheel_scale_version"))
            return
        if event == "ABORT":
            if not warmup_abort:
                self.holdout_aborted += 1
            self.issue("holdout_aborted", source, row["reason"])
            if warmup_abort:
                if window is not None:
                    self.issue("holdout_missing_begin", source, "Warmup ABORT cannot terminate an open holdout window")
                return
        if window is None or window["window_id"] != row["window_id"]:
            self.issue("holdout_missing_begin", source, "No matching holdout BEGIN in this recorded session")
            return
        if (anchor != window["anchor_ns"] or (event != "ABORT" and (
                row["calibration_version"] != window["calibration_version"] or row["yaw_zero"] != window["yaw_zero"] or
                row.get("wheel_scale") != window["wheel_scale"] or
                row.get("wheel_scale_version") != window["wheel_scale_version"]))):
            self.issue("holdout_window_changed", source, "Anchor/calibration changed within holdout window")
            return
        if row["mono_ns"] < window["mono_ns"]:
            self.issue("invalid_holdout_time", source, "Holdout diagnostic clock regressed within window")
            return
        window["mono_ns"] = row["mono_ns"]
        if event == "COMPARED":
            if reference <= window["last_reference_ns"]:
                self.issue("holdout_reference_replayed", source, "GPS comparison receipt time did not advance")
                return
            window["last_reference_ns"] = reference
            add_difference(self.holdout_position, row["position_error_m"])
            if row["heading_error_rad"] is not None:
                add_difference(self.holdout_heading, row["heading_error_rad"])
        else:
            if event == "END":
                if frontier <= anchor or frontier < window["last_reference_ns"]:
                    self.issue("invalid_holdout_time", source, "Holdout END must advance beyond anchor and cover every comparison")
                    return
                self.holdout_completed += 1
            s["holdout_window"] = None

    def bus_health_record(self, observer, now, source):
        capacity, contexts = observer["capacity"], observer["contexts"]
        if self.bus_capacity is not None and capacity != self.bus_capacity:
            self.issue("bus_capacity_changed", source, "Fixed callback capacity changed within one boot", True)
        self.bus_capacity = capacity
        # Health rows are written by one worker. Contexts are reserved forever,
        # even after failed creation/free; neither the counter nor capacity can
        # shrink. A previously emitted request already proves object existence.
        if contexts < self.bus_contexts_max:
            self.issue("bus_context_count_regressed", source, "Reserved context count decreased", True)
        self.bus_contexts_max = max(self.bus_contexts_max, contexts)
        if any(obj > contexts for obj in self.bus_objects):
            self.issue("bus_object_outside_contexts", source, "Observed object exceeds later reserved context count", True)
        if any(obj > capacity for obj in self.bus_objects):
            self.issue("bus_object_outside_capacity", source, "Observed object exceeds fixed callback capacity", True)
        # Store one timestamp per distinct count, not every 1 Hz health row.
        self.bus_health_counts[contexts] = max(self.bus_health_counts.get(contexts, -1), now)

    def bus_connection_record(self, snapshot, observed, row_ns, source):
        obj, lifetime = snapshot["object"], snapshot["lifetime"]
        if obj is None:
            return
        if lifetime is not None:
            owner = self.bus_lifetimes.setdefault(lifetime, obj)
            if owner != obj:
                self.issue("bus_lifetime_owner_changed", source, "One global connect lifetime belongs to two objects", True)
        # The clock is sampled AFTER the snapshot. It gives an upper bound on
        # object creation, never an ordering of snapshots across requests. An
        # old snapshot can be timestamped or drained after a newer connection.
        bounds = [n for n in (observed, row_ns) if integer(n) and n > 0]
        upper_bound = min(bounds) if bounds else None
        previous = self.bus_objects.get(obj)
        if previous is not None:
            upper_bound = min(previous, upper_bound) if upper_bound is not None else previous
        self.bus_objects[obj] = upper_bound
        if self.bus_capacity is not None and obj > self.bus_capacity:
            self.issue("bus_object_outside_capacity", source, "Observed object exceeds fixed callback capacity", True)
        # A later timestamp covers the earlier snapshot; equality cannot order
        # clock calls. Unknown clocks must not turn an earlier health row into
        # proof that an object created afterwards is impossible.
        if upper_bound is not None and any(count < obj and now > upper_bound
                                           for count, now in self.bus_health_counts.items()):
            self.issue("bus_object_outside_contexts", source, "Reserved context count cannot cover an earlier object", True)

    def request_record(self, row, source, count=False):
        # Older journals predate request observation. Presence opts into this
        # schema; absence never proves a qualified request or receiver.
        if "request" not in row:
            return
        t = row["request"]
        results = {"observed", "not_observed", "reply_not_observed", "observation_gap",
                   "observation_busy", "observation_capacity", "identity_conflict",
                   "different_position_pointer", "scope_consumed", "invalid_observation_input",
                   "observation_ids_exhausted"}
        ids = ("request_id", "request_epoch", "worker_id", "worker_epoch")
        optional = ("issue_observed_ns", "reply_observed_ns", "bus_lifetime",
                    "session_lifetime", "session_event")
        def unsigned(value, bits=64):
            return integer(value) and 0 <= value < 2**bits
        def text(value):
            return (isinstance(value, dict) and "value" in value and isinstance(value.get("complete"), bool) and
                    ((value.get("value") is None and not value["complete"]) or
                     (isinstance(value.get("value"), str) and len(value["value"]) <= 64 and
                      all(0 < ord(c) <= 255 for c in value["value"]) and
                      (not value["complete"] or len(value["value"]) < 64))))
        valid = (isinstance(t, dict) and t.get("association_only") is True and
                 isinstance(t.get("result"), str) and t["result"] in results and
                 all(unsigned(t.get(k)) for k in ids) and
                 all(k in t and (t[k] is None or unsigned(t[k])) for k in optional) and
                 "session_state" in t and (t["session_state"] is None or
                    (integer(t["session_state"]) and -2**31 <= t["session_state"] < 2**31)) and
                 "reply_type" in t and (t["reply_type"] is None or
                    (integer(t["reply_type"]) and t["reply_type"] in (1, 2))) and
                 "wire_serial" in t and (t["wire_serial"] is None or
                    (unsigned(t["wire_serial"], 32) and t["wire_serial"] > 0)) and
                 all(text(t.get(k)) for k in ("sender", "error")))
        if valid and "route" in t:
            route = t["route"]
            fields = ("destination", "path", "interface", "member")
            valid = isinstance(route, dict) and all(text(route.get(k)) for k in fields)
            if valid and t["result"] != "observed":
                valid = all(route[k]["value"] is None for k in fields)
        if valid:
            if "session_context" in t:
                valid = session_snapshot(t["session_context"], "unique_live_context")
                if t["result"] != "observed":
                    valid = valid and t["session_context"]["result"] == "unobserved"
        connection_fields = ("issue_connection", "reply_connection")
        if valid and any(k in t for k in connection_fields):
            valid = all(bus_snapshot(t.get(k)) for k in connection_fields)
            if valid:
                valid = t["bus_lifetime"] == t["issue_connection"]["lifetime"]
                if t["result"] != "observed":
                    valid = valid and all(t[k]["result"] == "unobserved" for k in connection_fields)
        if valid:
            if t["result"] == "observed":
                valid = all(t[k] > 0 for k in ids) and t["request_epoch"] == t["worker_epoch"]
            else:
                valid = (all(t[k] == 0 for k in ids) and
                         all(t[k] is None for k in optional + ("session_state", "reply_type", "wire_serial")) and
                         all(t[k]["value"] is None for k in ("sender", "error")))
        if not valid:
            self.issue("request_record_malformed", source, "Invalid request association record")
            return
        if count:
            self.request_results[t["result"]] += 1
            self.request_reply_types[str(t["reply_type"])] += 1
            if t["error"]["complete"]:
                self.request_errors[t["error"]["value"]] += 1
        if t["result"] not in ("observed", "not_observed", "reply_not_observed"):
            self.issue("request_observation_failed", source, t["result"])
        if ("session_context" in t and
                t["session_context"]["result"] in ("transition", "ambiguous", "observation_fault")):
            self.issue("session_observation_unavailable", source, t["session_context"]["result"])

        if t["result"] == "observed" and "issue_connection" in t:
            issue, reply = t["issue_connection"], t["reply_connection"]
            self.bus_connection_record(issue, t["issue_observed_ns"], row.get("mono_ns"), source)
            self.bus_connection_record(reply, t["reply_observed_ns"], row.get("mono_ns"), source)
            if issue["result"] != "connected" or reply["result"] != "connected":
                self.issue("bus_observation_unavailable", source, "Issue/reply connection continuity is unknown")
            elif issue != reply:
                self.issue("bus_changed_since_issue", source, "Reply connection differs from the issue-time connection")
                if issue["object"] == reply["object"] and reply["lifetime"] < issue["lifetime"]:
                    self.issue("bus_lifetime_regressed", source, "Same request's reply precedes its issue connection lifetime", True)

    def send(self, row, source):
        if not self.validate(row, source, ("call", "generation", "mono_ns", "mode", "type", "length",
                                          "choice", "reason", "result"), ("original_hex", "outgoing_hex")):
            return
        self.request_record(row, source)
        if "send_session" in row:
            target = row["send_session"]
            if not session_snapshot(target, "send_storage"):
                self.issue("send_session_malformed", source, "Invalid send storage observation")
            else:
                request = row.get("request")
                ambient = request.get("session_context") if isinstance(request, dict) else None
                if (session_snapshot(ambient, "unique_live_context") and
                        ("revision" in ambient) != ("revision" in target)):
                    self.issue("session_revision_partial", source,
                               "Issue and send snapshots mix revision schemas")
                if (session_snapshot(ambient, "unique_live_context") and
                        ambient["result"] == target["result"] == "observed"):
                    if (ambient["lifetime"] != target["lifetime"] or
                            ("revision" in ambient and "revision" in target and
                             ambient["revision"] != target["revision"])):
                        self.issue("session_changed_since_issue", source,
                                   "Send target differs from issue-time ambient context; ownership is unproved")
                    if ambient["lifetime"] == target["lifetime"] and ambient["event"] is not None and (
                            target["event"] is None or target["event"] < ambient["event"] or
                            (target["event"] == ambient["event"] and target["state"] != ambient["state"])):
                        self.issue("session_state_inconsistent", source,
                                   "Same-lifetime callback history contradicts its issue-time snapshot", True)
                    if "revision" in ambient and "revision" in target:
                        delta = target["revision"] - ambient["revision"]
                        if (delta < 0 or (ambient["lifetime"] == target["lifetime"] and
                                (target["event"] or 0) - (ambient["event"] or 0) > delta)):
                            self.issue("session_revision_inconsistent", source,
                                       "Completed callback history contradicts lifecycle revisions", True)
                if target["result"] in ("transition", "ambiguous", "observation_fault"):
                    self.issue("session_observation_unavailable", source, target["result"])
        position = self.positions.get((row["call"], row["generation"]))
        if position and ("request" in position or "request" in row) and position.get("request") != row.get("request"):
            self.issue("request_copy_mismatch", source, "Position/send request metadata differ", True)
        s = self.session
        s["sends"] += 1
        s["last_send_ns"] = max(s["last_send_ns"], row["mono_ns"])
        choice = row["choice"]
        self.choices[CHOICES.get(choice, "UNKNOWN:" + str(choice))] += 1
        reason = row["reason"]
        self.reasons[REASONS[reason] if 0 <= reason < len(REASONS) else "UNKNOWN:" + str(reason)] += 1
        self.results[str(row["result"])] += 1
        if choice == 2:
            self.issue("dr_replacement_impossible", source, "DR_REPLACEMENT is disabled in the live runtime", True)
        elif choice not in CHOICES:
            self.issue("unknown_send_choice", source, str(choice), True)
        if row["type"] != 1 or row["length"] != 48:
            if choice != 0:
                self.issue("non_location_mutation", source, "Only type1 length48 LOCATION may be scrubbed", True)
            return
        p = self.positions.get((row["call"], row["generation"]))
        if p is None:
            self.issue("missing_position_context", source, "No matching position call/generation")
        elif p["mode"] != row["mode"]:
            self.issue("context_mode_mismatch", source, "Position/send original modes differ", True)
        payloads = []
        for key in ("original_hex", "outgoing_hex"):
            value = row[key]
            if not re.fullmatch(r"[0-9a-fA-F]{96}", value):
                self.issue("invalid_payload_hex", source, key + " must encode exactly 48 bytes")
                return
            payloads.append(bytes.fromhex(value))
        original, outgoing = payloads
        self.checked += 1
        if row["mode"] in (1, 2, 3) and (original != outgoing or choice != 0):
            self.issue("native_mode_mutation", source, "Native mode1/2/3 must remain ORIGINAL and identical", True)
        if choice == 0 and original != outgoing:
            self.issue("original_payload_changed", source, "ORIGINAL payload differs", True)
        if choice == 1:
            if s["boot"] is not None and s["boot"].get("mode") != 2:
                self.issue("scrub_without_scrub_config", source, "SCRUBBED requires boot configuration SCRUB=2", True)
            expected = bytearray(original)
            for i in CLEAR_BYTES:
                expected[i] = 0
            if row["mode"] != 0:
                self.issue("scrub_wrong_mode", source, "SCRUBBED requires original mode0", True)
            if row["reason"] != 0:
                self.issue("scrub_nonpass_reason", source, "SCRUBBED must have reason PASS", True)
            if outgoing != bytes(expected):
                self.issue("scrub_payload_mismatch", source, "Only bytes32,36..39,40,44..47 must be zeroed", True)

    def read_stream(self, stream, name, size):
        if size > MAX_FILE_BYTES or self.total_bytes + size > MAX_TOTAL_BYTES:
            self.issue("input_limit", name, "File/total byte budget exceeded")
            return
        self.total_bytes += size
        self.files.append(name)
        number = 0
        while True:
            raw = stream.readline(MAX_LINE_BYTES + 1)
            if not raw:
                break
            number += 1
            source = "%s:%d" % (name, number)
            if len(raw) > MAX_LINE_BYTES:
                self.issue("line_limit", source, "Oversized record; remainder of file not read")
                break
            if not raw.endswith(b"\n"):
                self.issue("partial_final_line", source, "Final record lacks newline; completeness unproven")
            if not raw.strip():
                continue
            try:
                row = json.loads(raw.decode("utf-8"), object_pairs_hook=strict_object, parse_float=finite_float,
                                 parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
            except (ValueError, UnicodeError, RecursionError) as exc:
                self.issue("malformed_json", source, str(exc))
                continue
            self.consume(row, source)

    def read_path(self, path):
        path = Path(path)
        try:
            if path.is_symlink():
                self.issue("unsafe_input", str(path), "Symlink inputs are refused")
            elif path.is_dir():
                files = list(path.rglob("*.jsonl"))
                files += [p for p in path.rglob('*.storage.json') if p.name in STORAGE_FILES]
                files.sort(key=lambda p: rotation_key(str(p)))
                if not files:
                    self.issue("no_jsonl_files", str(path), "No JSONL files found")
                for file in files:
                    # Refuse traversal through a symlinked parent as well.
                    if any(p.is_symlink() for p in (file, *file.parents)):
                        self.issue("unsafe_input", str(file), "Symlink inputs are refused")
                    else:
                        self.read_path(file)
            elif path.name.endswith((".tar", ".tar.gz", ".tgz")):
                self.read_tar(path)
            elif path.is_file():
                with path.open("rb") as stream:
                    self.read_stream(stream, str(path), path.stat().st_size)
            else:
                self.issue("input_unavailable", str(path), "Not a regular file/directory")
        except (OSError, tarfile.TarError, EOFError) as exc:
            self.issue("input_error", str(path), str(exc))

    def read_tar(self, path):
        with tarfile.open(path, "r:*") as archive:
            members = []
            names = set()
            for count, member in enumerate(archive, 1):
                name = member.name
                parts = PurePosixPath(name).parts
                if count > MAX_MEMBERS:
                    self.issue("archive_limit", str(path), "Too many members")
                    break
                if name.startswith("/") or ".." in parts or "\\" in name or not (member.isfile() or member.isdir()):
                    self.issue("unsafe_archive_member", str(path), name)
                    continue
                if name in names:
                    self.issue("duplicate_archive_member", str(path), name)
                    continue
                names.add(name)
                if member.isfile() and (name.endswith(".jsonl") or PurePosixPath(name).name in STORAGE_FILES):
                    members.append(member)
                elif member.isfile():
                    self.ignored.append(name)
            for member in sorted(members, key=lambda m: rotation_key(m.name)):
                with archive.extractfile(member) as stream:
                    self.read_stream(stream, str(path) + "!" + member.name, member.size)

    def report(self):
        for index, session in enumerate(self.sessions):
            source = "session:%d" % (index + 1)
            if session["boot"] is None:
                self.issue("missing_boot", source, "Rotated/partial trace has no boot record")
            if not session["health_records"]:
                self.issue("missing_health", source, "No health record")
            elif session["health_ns"] < max(session["last_send_ns"], session["last_diagnostic_ns"]):
                self.issue("uncovered_trace_tail", source, "No health record at/after final send or diagnostic")
            if session["holdout_window"] is not None:
                self.issue("holdout_unfinished_window", source, "Recorded holdout window has no END/ABORT")
        if not self.checked:
            self.issue("no_location_samples", "inputs", "No complete type1 length48 payload pair checked")
        for session in self.collector_sessions:
            if session["stop_ns"] is None:
                self.issue("collector_open_session", session["source"],
                           "No matching shutdown after this collector boot; observation coverage is incomplete")
        status = ("violation" if self.issue_counts["violation"] else
                  "inconclusive" if self.issue_counts["inconclusive"] else "local_checks_pass")
        def boot_ids(rows):
            return sorted({row["boot_id"] for row in rows
                           if isinstance(row.get("boot_id"), str) and
                           re.fullmatch(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", row["boot_id"])})
        aa_boot_ids = boot_ids(self.boots)
        collector_boot_ids = boot_ids(self.collector_boots)
        return dict(report_schema=1, runtime_release="not_inferred_from_log_schema", status=status,
                    scope="recorded_window_local_invariants_only", phone_acceptance="not_established",
                    dr_accuracy="not_established", limitations=LIMITATIONS,
                    files=self.files, ignored_archive_members=self.ignored,
                    record_counts=dict(self.counts), input_modes=dict(self.modes),
                    motion=dict(samples=self.motion_samples, batches=self.motion_batches,
                                sensors=dict(self.motion_sensors), producer_time="unknown",
                                scope="channel_accepted_records_only"),
                    motion_rejected=dict(reasons=dict(self.motion_rejected_reasons),
                                         sensors=dict(self.motion_rejected_sensors),
                                         scope="diagnostic_only_excluded_from_accepted_motion"),
                    capture=dict(completion_records=self.capture_ends,
                                 scope="recorded_cutoff_not_proof_of_storage_or_vehicle_safety"),
                    shadow=dict(pipelines=dict(self.shadow_pipelines), results=dict(self.shadow_results),
                                states=dict(self.shadow_states), model_valid=dict(self.shadow_valid),
                                resets_max=self.shadow_resets_max, rejected_max=self.shadow_rejected_max,
                                scope="reported_model_diagnostics_not_accuracy_validation"),
                    shadow_calibration=dict(states=dict(self.calibration_states),
                                            enabled=dict(self.calibration_enabled),
                                            candidate_ready=dict(self.calibration_candidates),
                                            versions=dict(self.calibration_versions),
                                            samples_max=self.calibration_samples_max,
                                            wheel_scale=dict(self.wheel_scales),
                                            wheel_versions=dict(self.wheel_versions),
                                            gps_anchor_gates=dict(self.gps_anchor_gates),
                                            scope="stationary_receipt_model_hypothesis_not_verified_calibration"),
                    shadow_holdout=dict(events=dict(self.holdout_events), reasons=dict(self.holdout_reasons),
                                        completed_windows=self.holdout_completed, aborted_windows=self.holdout_aborted,
                                        position_difference_m=dict(self.holdout_position),
                                        heading_difference_rad=dict(self.holdout_heading),
                                        time_basis="receipt_model", gps_is_ground_truth=False,
                                        reference_exclusion="not_provable_from_journal",
                                        comparison_scope="recorded_compared_events_including_later_aborted_windows",
                                        scope="model_to_gps_differences_not_physical_accuracy"),
                    storage_stops=self.storage_stops,
                    position_poll_modes=dict(self.poll_modes), send_choices=dict(self.choices),
                    send_reasons=dict(self.reasons), lower_send_results=dict(self.results),
                    checked_location_payload_pairs=self.checked,
                    drop_health=dict(records=len(self.health), max_dropped=max([h["dropped"] for h in self.health] or [0]),
                                     per_session_max=[s["dropped_max"] for s in self.sessions],
                                     audit_fault_counts=dict(self.audit_faults)),
                    actual_runtime_modes=dict(self.runtime_modes),
                    request_observation=dict(position_results=dict(self.request_results),
                                             reply_types=dict(self.request_reply_types),
                                             complete_error_names=dict(self.request_errors),
                                             qualification="not_established"),
                    stream_correlation=dict(aa_boot_ids=aa_boot_ids, collector_boot_ids=collector_boot_ids,
                                            shared_kernel_boot_ids=sorted(set(aa_boot_ids) & set(collector_boot_ids)),
                                            meaning="same_kernel_boot_only_not_request_or_producer_provenance"),
                    collector=dict(record_counts=dict(self.collector_counts),
                                   pids=sorted(self.collector_pids), boots=self.collector_boots,
                                   stops=self.collector_stops, producer_time="unknown",
                                   sessions=[dict(boot_id=s["boot_id"], pid=s["pid"],
                                                  start_ns=s["start_ns"], stop_ns=s["stop_ns"])
                                             for s in self.collector_sessions],
                                   request_provenance="not_established"),
                    owner_pid_comm=[dict(owner=k[0], pid=k[1], comm=k[2], count=v) for k, v in sorted(self.owners.items())],
                    receiver_counts=dict(self.receivers), install_counts=dict(self.installs), boots=self.boots,
                    issue_counts=dict(self.issue_counts), issues=self.issues,
                    omitted_issue_details=max(0, sum(self.issue_counts.values()) - len(self.issues)))


def analyze(paths):
    auditor = Auditor()
    for path in paths:
        auditor.read_path(path)
    return auditor.report()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help="JSONL files, directories, or exported tar files; separate sessions in chronological order")
    parser.add_argument("--json", action="store_true", help="Emit a machine-readable JSON report")
    args = parser.parse_args(argv)
    report = analyze(args.inputs)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False))
    else:
        print("%s: %d LOCATION payload pairs checked" % (report["status"], report["checked_location_payload_pairs"]))
        print("Input modes: %s; choices: %s; max dropped: %s" %
              (report["input_modes"], report["send_choices"], report["drop_health"]["max_dropped"]))
        if report["record_counts"].get("shadow_calibration"):
            print("MODEL calibration states: %s; versions: %s" %
                  (report["shadow_calibration"]["states"], report["shadow_calibration"]["versions"]))
        if report["record_counts"].get("shadow_holdout"):
            holdout = report["shadow_holdout"]
            print("Receipt-time MODEL holdout events: %s; GPS position differences (m): %s" %
                  (holdout["events"], holdout["position_difference_m"]))
        if report['motion_rejected']['reasons']:
            print("Rejected sensor diagnostics (not accepted input): %s" %
                  report['motion_rejected']['reasons'])
        for issue in report["issues"][:10]:
            print("%s %s: %s" % (issue["code"], issue["source"], issue["detail"]))
        print("Phone acceptance and DR accuracy: not established. Polling does not prove source provenance.")
    return {"local_checks_pass": 0, "violation": 1, "inconclusive": 2}[report["status"]]


if __name__ == "__main__":
    sys.exit(main())
