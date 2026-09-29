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
CLEAR_BYTES = (32, 36, 37, 38, 39, 40, 44, 45, 46, 47)
CHOICES = {0: "ORIGINAL", 1: "SCRUBBED", 2: "DR_REPLACEMENT"}
REASONS = ("PASS", "NO_CONTEXT", "NESTED_CALL", "EXTRA_LOCATION", "BAD_LENGTH",
           "DISABLED", "LOCK_BUSY", "NOT_UNKNOWN", "NOT_READY", "EPOCH_MISMATCH",
           "EXPIRED", "BAD_ENCODING", "BAD_PROVENANCE")
LIMITATIONS = [
    "Only recorded local byte invariants are checked; no complete vehicle-session proof.",
    "Lower send result is not phone receipt, app adoption, or navigation success.",
    "SMDB/owner/receiver polls do not establish source freshness or exact-request provenance.",
    "SHADOW model diagnostics do not establish DR accuracy, ground truth, or ASSIST readiness.",
    "Motion counts cover channel-accepted records; source measurement timing remains unknown.",
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
        self.collector_pids = set()
        self.motion_samples = 0
        self.motion_sensors = Counter()
        self.motion_batches = 0
        self.shadow_pipelines = Counter()
        self.shadow_results = Counter()
        self.shadow_states = Counter()
        self.shadow_valid = Counter()
        self.shadow_resets_max = 0
        self.shadow_rejected_max = 0

    def issue(self, code, source, detail, violation=False):
        severity = "violation" if violation else "inconclusive"
        self.issue_counts[severity] += 1
        if len(self.issues) < 100:
            self.issues.append(dict(severity=severity, code=code, source=source, detail=detail))

    def new_session(self, boot=None):
        self.session = dict(boot=boot, last_send_ns=-1, health_ns=-1, sends=0,
                            dropped_max=0, health_records=0, motion_epoch=None,
                            motion_seq=0, motion_ns=0, last_diagnostic_ns=-1,
                            shadow_resets=0, shadow_rejected=0, shadow_pipeline=None)
        self.sessions.append(self.session)
        self.positions = {}

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

    def consume(self, row, source):
        if not isinstance(row, dict) or not isinstance(row.get("kind"), str):
            self.issue("partial_record", source, "Expected object with kind")
            return
        kind = row["kind"]
        self.counts[kind] += 1
        collector = row.get("stream") == "collector"
        if collector:
            self.collector_counts[kind] += 1
            if not self.validate(row, source, ("collector_pid", "observed_at_mono_ns"),
                                 ("producer_time_status",)):
                return
            self.collector_pids.add(row["collector_pid"])
            if "producer_mono_ns" not in row or row["producer_mono_ns"] is not None or row["producer_time_status"] != "unknown":
                self.issue("unexpected_poll_qualification", source, "Collector cannot establish producer measurement time", True)
            if kind == "collector_boot":
                self.validate(row, source, ("schema", "sample_ms", "session_seconds"), ("boot_id",))
                self.collector_boots.append(row)
                return
            if kind == "collector_stop":
                self.validate(row, source, ("samples",), ("reason",))
                self.collector_stops.append(row)
                return
            if kind not in ("poll", "position_poll", "position_poll_error", "owner_poll", "receiver_poll"):
                self.issue("unexpected_collector_record", source, "Collector cannot emit AA hook/health records", True)
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
        if kind == "position":
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
        elif kind in ("shadow_boot", "shadow", "shadow_input_reset", "shadow_disabled"):
            self.shadow(row, source)
        else:
            self.issue("unknown_record_kind", source, kind)

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

    def shadow(self, row, source):
        if not self.validate(row, source, bools=("assist_ready",)):
            return
        if row["assist_ready"]:
            self.issue("impossible_live_capability", source, "SHADOW cannot authorize ASSIST", True)
        kind = row["kind"]
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
            return
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
        self.session["last_diagnostic_ns"] = max(self.session["last_diagnostic_ns"], row["mono_ns"])
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
        for key in ("lat", "lon", "heading_rad", "speed_mps", "error_model_m"):
            if key not in row or (row[key] is not None and type(row[key]) not in (int, float)):
                self.issue("partial_record", source, "Invalid SHADOW numeric field: " + key)
        preview = row["location_preview_hex"]
        if (row["preview_encoded"] and (not row['model_valid'] or
                not re.fullmatch(r"[0-9a-fA-F]{96}", preview))) or (
                not row["preview_encoded"] and preview != ""):
            self.issue("invalid_shadow_preview", source, "Preview must match encoded flag and 48-byte shape")

    def send(self, row, source):
        if not self.validate(row, source, ("call", "generation", "mono_ns", "mode", "type", "length",
                                          "choice", "reason", "result"), ("original_hex", "outgoing_hex")):
            return
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
                files = sorted(path.rglob("*.jsonl"), key=lambda p: rotation_key(str(p)))
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
                if member.isfile() and name.endswith(".jsonl"):
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
        if not self.checked:
            self.issue("no_location_samples", "inputs", "No complete type1 length48 payload pair checked")
        if self.collector_counts and not self.collector_boots:
            self.issue("collector_missing_boot", "collector", "Collector rotation/start boundary is missing")
        if len(self.collector_stops) < len(self.collector_boots):
            self.issue("collector_open_session", "collector", "Collector shutdown is not recorded; observation coverage is incomplete")
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
                    shadow=dict(pipelines=dict(self.shadow_pipelines), results=dict(self.shadow_results),
                                states=dict(self.shadow_states), model_valid=dict(self.shadow_valid),
                                resets_max=self.shadow_resets_max, rejected_max=self.shadow_rejected_max,
                                scope="reported_model_diagnostics_not_accuracy_validation"),
                    position_poll_modes=dict(self.poll_modes), send_choices=dict(self.choices),
                    send_reasons=dict(self.reasons), lower_send_results=dict(self.results),
                    checked_location_payload_pairs=self.checked,
                    drop_health=dict(records=len(self.health), max_dropped=max([h["dropped"] for h in self.health] or [0]),
                                     per_session_max=[s["dropped_max"] for s in self.sessions],
                                     audit_fault_counts=dict(self.audit_faults)),
                    actual_runtime_modes=dict(self.runtime_modes),
                    stream_correlation=dict(aa_boot_ids=aa_boot_ids, collector_boot_ids=collector_boot_ids,
                                            shared_kernel_boot_ids=sorted(set(aa_boot_ids) & set(collector_boot_ids)),
                                            meaning="same_kernel_boot_only_not_request_or_producer_provenance"),
                    collector=dict(record_counts=dict(self.collector_counts),
                                   pids=sorted(self.collector_pids), boots=self.collector_boots,
                                   stops=self.collector_stops, producer_time="unknown",
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
        for issue in report["issues"][:10]:
            print("%s %s: %s" % (issue["code"], issue["source"], issue["detail"]))
        print("Phone acceptance and DR accuracy: not established. Polling does not prove source provenance.")
    return {"local_checks_pass": 0, "violation": 1, "inconclusive": 2}[report["status"]]


if __name__ == "__main__":
    sys.exit(main())
