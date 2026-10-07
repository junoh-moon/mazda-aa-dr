#!/usr/bin/env python3
"""Offline trace auditor (Python 3, PC only; never extracts tar members).

Exit 0: local checks passed for the recorded window; 1: invariant violation;
2: insufficient/malformed evidence. No exit status establishes phone acceptance,
DR accuracy, or a complete vehicle session. Run --help for input syntax.
"""
import argparse
import bisect
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
# src/adapter/adapter.h Choice/Reason (exact enum order).
CHOICES = {0: "ORIGINAL", 1: "SCRUBBED", 2: "DR_REPLACEMENT", 3: "BETA_REPLACEMENT",
           4: "BETA_SPEED_OVERLAY"}
REASONS = ("PASS", "NO_CONTEXT", "NESTED_CALL", "EXTRA_LOCATION", "BAD_LENGTH",
           "DISABLED", "LOCK_BUSY", "NOT_UNKNOWN", "NOT_READY", "EPOCH_MISMATCH",
           "EXPIRED", "BAD_ENCODING", "BAD_PROVENANCE", "CONTEXT_UNAVAILABLE", "HELD",
           "OVERLAY_MISMATCH", "OVERLAY_NOT_NEEDED")
# BETA (config mode 5): MODEL-domain LOCATION replacement while the original
# reports mode 0 (class LOST), and the wheel speed overlay while it reports
# the stored no-fix position (class NO_FIX: mode 1/2, utc_s 0);
# validation/ASSIST_BETA_DESIGN_2026-10-05.md, BETA_DECISIONS_2026-10-05.md.
BETA_STATES = ("DISABLED", "ARMED", "GPS_LOST", "ENGAGED", "WITHDRAWN", "FAULT",
               "NO_FIX", "SPEED_ENGAGED")
BETA_LIVE_STATES = ("GPS_LOST", "ENGAGED")
BETA_SPEED_STATES = ("NO_FIX", "SPEED_ENGAGED")
BETA_MAX_ACCURACY_E3 = 40000
# encode_speed_overlay: only hasSpeed (32) and speed_e3 (36..39) may change.
BETA_OVERLAY_BYTES = (32, 36, 37, 38, 39)
BETA_MAX_SPEED_E3 = 100000
# BetaProfile.lease_ns / Options.max_snapshot_age_ns: an overlay speed comes
# from a wheel event received at most this long before the send.
BETA_LEASE_NS = 500000000
# Row kinds whose time is not the worker's clock at the moment the row was
# queued: POSITION/SEND carry the OEM hook time, motion rows the producer
# receipt time (both can precede rows the journal writer dropped before
# them). They, and rows written from a persistent-profile RAW window, never
# close a journal_dropped gap (2026-10-07).
GAP_NON_CLOSING_KINDS = frozenset(("position", "send", "motion", "motion_batch"))
# Report values that can only under-count (2026-10-07): the rows that carry
# them are diagnostic class, so the journal writer may drop them under a
# backlog, and a reset or process death loses the final digest.
LOWER_BOUNDS = {
    "persistent_profile.suppressed": "log_digest rows are diagnostic class (droppable); the final digest is lost "
                                     "on a reset or process death",
    "motion_rejected.suppressed_by_profile": "from log_digest suppressed counters (see persistent_profile.suppressed)",
    "motion_rejected.total_including_suppressed": "written rejection rows plus digest counters; both can be dropped",
    "journal_lag.not_durable": "the journal_not_durable row is diagnostic class and is written while the journal "
                               "is already behind",
}
# Adapter PositionClass numbers carried as the send/position "class" field.
POSITION_CLASS_NO_FIX, POSITION_CLASS_LOST = 1, 3
BETA_KINDS = ("beta_state", "beta_summary", "beta_hold", "beta_session_storage", "beta_anchor")
# beta_reverse_latch is a MODEL-domain row (also written in SHADOW mode 4).
WHEEL_PROFILE = (0.01, -100.0)  # research_model_profile(): km/h per count, zero
EARTH_RADIUS_M = 6371008.8
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
    "Endpoint server GUID is transport-specific, not the GetId bus ID; complete request keys do not establish LDS lineage or ASSIST qualification.",
    "An issue-time unique live session is ambient context, not request ownership or phone acceptance.",
    "SHADOW model diagnostics do not establish DR accuracy, ground truth, or ASSIST readiness.",
    "Yaw/wheel calibration and GPS holdout differences are receipt-time MODEL hypotheses only.",
    "Holdout journal structure cannot prove that GPS references were excluded from prediction inputs.",
    "Holdout references match earlier raw rows within one trace group and recorded session; missing boot still leaves process identity unproven.",
    "Motion counts cover channel-accepted records; source measurement timing remains unknown.",
    "LDS attachments use six exact wire identifiers within one trace group and recorded session; body and clocks never substitute for identity.",
    "LDS field origins are observed cache assignments, not producer measurements, receiver quality, or ASSIST qualification; UID 0 is only a local account boundary.",
    "LDS assignment patterns cover only matched wire/payload rows and count retained known field origins, not all cache writes or physical fixes.",
    "A recorded LDS match may change with later rows; file end does not certify a complete source or receiver session.",
    "Collector stops carry no boot ID; matching uses ordered boot boundaries, PID, and monotonic receipt time.",
    "BETA GPS-return distances compare the last replaced LOCATION with the first later original fix; GPS is a reference, not ground truth, and a zero send result is not phone adoption.",
]


def rotation_key(name):
    p = PurePosixPath(name)
    m = re.fullmatch(r"(trace|collector)\.(\d+)\.jsonl", p.name)
    return (str(p.parent), m[1] if m else p.name, -int(m[2]) if m else 0, p.name)


def trace_group(name):
    """Only the actual trace.N.jsonl rotation family shares AA row identity."""
    p = PurePosixPath(name)
    return (str(p.parent), "trace" if re.fullmatch(r"trace\.\d+\.jsonl", p.name) else p.name)


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


def distance_m(lat1, lon1, lat2, lon2):
    """Great-circle (haversine) distance on a mean-radius sphere."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * EARTH_RADIUS_M * math.asin(min(1.0, math.sqrt(a)))


def advance(lat, lon, bearing_deg, metres):
    """Destination after a constant-bearing great-circle step."""
    d = metres / EARTH_RADIUS_M
    p1, l1, b = math.radians(lat), math.radians(lon), math.radians(bearing_deg)
    p2 = math.asin(max(-1.0, min(1.0, math.sin(p1) * math.cos(d) +
                                 math.cos(p1) * math.sin(d) * math.cos(b))))
    l2 = l1 + math.atan2(math.sin(b) * math.sin(d) * math.cos(p1),
                         math.cos(d) - math.sin(p1) * math.sin(p2))
    return math.degrees(p2), (math.degrees(l2) + 540.0) % 360.0 - 180.0


def beta_location(payload):
    """Fields the BETA encoder writes into the 48-byte LOCATION payload."""
    def signed(lo):
        return int.from_bytes(payload[lo:lo + 4], 'little', signed=True)

    def unsigned(lo):
        return int.from_bytes(payload[lo:lo + 4], 'little')
    return dict(lat=signed(8) / 1e7, lon=signed(12) / 1e7, has_accuracy=payload[16],
                accuracy_e3=unsigned(20), speed_mps=unsigned(36) / 1000.0,
                moving=payload[40] == 1, bearing_deg=unsigned(44) / 1e6)


def wheel_speed_e3(raw, profile):
    """Overlay speed (mm/s) the BETA pipeline derives from one WHEELS event:
    mean of four wheels (count*k + zero km/h), 0 while every wheel reads at
    most 0.05 m/s, rounded like the adapter encoder. None: not a speed event."""
    k, zero = profile
    if len(raw) != 4 or any(r > 40000 for r in raw):
        return None
    wheels = [r * k + zero for r in raw]
    if any(w < 0 for w in wheels):
        return None
    if max(wheels) / 3.6 <= 0.05:
        return 0
    return int(math.floor(sum(w * 0.25 for w in wheels) / 3.6 * 1000 + 0.5))


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


class LdsLinks:
    """Bounded diagnostic join; late ambiguity withdraws earlier attachments.

    Exhaustion disables only this session's offline association, without
    evicting keys and accidentally accepting reused identities later. Raw rows
    and all existing MODEL/send analysis continue independently.
    """
    POSITION_FIELDS = ('mode', 'utc_s', 'lat', 'lon', 'altitude_m', 'heading',
                       'kmh', 'horizontal', 'vertical')
    capacity = 4096
    attachment_limit = 256

    def __init__(self, issue):
        self.issue = issue
        self.finished = Counter()
        self.finished_sideband = Counter()
        self.finished_assignment_patterns = Counter()
        self.finished_heading_presence = Counter()
        self.finished_heading_rmc_status = Counter()
        self.attachments = []
        self.attachment_total = 0
        self.records = Counter()
        self.statuses = Counter()
        self.session_index = 0
        self.reset()

    def begin(self):
        if self.session_index:
            counts, sideband, attachments, patterns, presence, rmc_status = self.current()
            self.finished.update(counts)
            self.finished_sideband.update(sideband)
            self.finished_assignment_patterns.update(patterns)
            self.finished_heading_presence.update(presence)
            self.finished_heading_rmc_status.update(rmc_status)
            self.attachment_total += len(attachments)
            self.attachments.extend(attachments[:max(0, self.attachment_limit-len(self.attachments))])
        self.session_index += 1
        self.reset()

    def reset(self):
        self.entries = {}
        self.ids = {}
        self.sources = {}
        self.unkeyed = Counter()
        self.exhausted = False
        self.observation_fault = False

    def invalidate_observation(self):
        # The live worker permanently retires its source after adapter loss.
        # Keep raw rows, but withdraw every diagnostic attachment in this
        # recorded session, including an earlier provisional match.
        self.observation_fault = True

    @staticmethod
    def text(value):
        return (isinstance(value, dict) and type(value.get('complete')) is bool and
                'value' in value and ((value['value'] is None and not value['complete']) or
                (isinstance(value['value'], str) and len(value['value']) < 64 and
                 all(0 < ord(c) <= 255 for c in value['value']))))

    @classmethod
    def complete(cls, value):
        return cls.text(value) and value['complete'] and bool(value['value'])

    @staticmethod
    def uint(value, bits=64, minimum=0):
        return bounded_int(value, minimum, 2**bits-1)

    @classmethod
    def sideband_key(cls, wire):
        if (not isinstance(wire, dict) or
                not all(cls.complete(wire.get(k)) for k in
                        ('server_guid', 'client_unique', 'server_unique')) or
                not all(cls.uint(wire.get(k), 32, 1) for k in
                        ('request_serial', 'response_serial', 'reply_serial'))):
            return None
        return (wire['server_guid']['value'], wire['client_unique']['value'], wire['request_serial'],
                wire['server_unique']['value'], wire['response_serial'], wire['reply_serial'])

    @classmethod
    def position_key(cls, row):
        request = row.get('request', {})
        endpoint, wire = request.get('endpoint', {}), request.get('wire', {})
        issue, reply = wire.get('issue', {}), wire.get('reply', {})
        if (request.get('result') != 'observed' or issue.get('known') is not True or
                issue.get('endpoint_matched') is not True or issue.get('conflict') is not False or
                reply.get('known') is not True or reply.get('type') != 2 or
                issue.get('serial') != reply.get('reply_serial')):
            return None
        return cls.sideband_key(dict(server_guid=endpoint.get('server_guid'),
            client_unique=endpoint.get('unique_name'), server_unique=reply.get('sender'),
            request_serial=issue.get('serial'), response_serial=reply.get('serial'),
            reply_serial=reply.get('reply_serial')))

    @classmethod
    def payload(cls, position):
        return (isinstance(position, dict) and all(k in position for k in cls.POSITION_FIELDS) and
                all(bounded_int(position[k], -2**31, 2**31-1) for k in ('mode', 'altitude_m')) and
                cls.uint(position['utc_s']) and
                all(position[k] is None or
                    (type(position[k]) in (int, float) and
                     -sys.float_info.max <= position[k] <= sys.float_info.max)
                    for k in ('lat', 'lon', 'heading', 'kmh', 'horizontal', 'vertical')))

    @classmethod
    def valid_record(cls, row):
        if (row.get('schema') != 1 or type(row.get('schema')) is not int or
                row.get('association_only') is not True or row.get('assist_ready') is not False or
                row.get('producer_time_status') != 'unknown' or
                not bounded_int(row.get('sender_pid'), 1, 2**31-1) or row.get('sender_uid') != 0 or
                type(row.get('sender_uid')) is not int or
                not all(cls.uint(row.get(k), minimum=1) for k in ('source_instance', 'sequence')) or
                not all(cls.uint(row.get(k)) for k in ('mono_ns', 'observed_ns', 'dropped_before')) or
                not cls.uint(row.get('flags'), 8) or
                not all(bounded_int(row.get(k), -2**31, 2**31-1) for k in
                        ('path_result', 'send_result', 'reply_type'))):
            return False
        wire, lineage, position = row.get('wire'), row.get('field_lineage'), row.get('position')
        if (not isinstance(wire, dict) or
                not all(cls.text(wire.get(k)) for k in
                        ('server_guid', 'client_unique', 'server_unique', 'destination')) or
                not all(cls.uint(wire.get(k), 32) for k in
                        ('request_serial', 'response_serial', 'reply_serial')) or
                not isinstance(lineage, dict) or lineage.get('association_only') is not True or
                not all(cls.uint(lineage.get(k)) for k in ('lifetime', 'write_sequence')) or
                not isinstance(position, dict) or
                type(position.get('snapshot_known')) is not bool or
                position['snapshot_known'] != bool(row['flags'] & 1) or
                not all(k in position for k in cls.POSITION_FIELDS)):
            return False
        sequences, clocks = lineage.get('field_write_sequences'), lineage.get('field_observed_ns')
        presence = lineage.get('heading_presence', 'unknown')
        if type(presence) is not str or presence not in ('unknown', 'empty', 'present'):
            return False
        rmc_status = lineage.get('heading_rmc_status', 'unknown')
        if type(rmc_status) is not str or rmc_status not in ('unknown', 'empty', 'a', 'v', 'other'):
            return False
        if (not isinstance(sequences, list) or not isinstance(clocks, list) or
                len(sequences) != 9 or len(clocks) != 9 or
                not all(cls.uint(v) and v <= lineage['write_sequence'] for v in sequences) or
                not all(cls.uint(v) for v in clocks) or
                any(s == 0 and c != 0 for s, c in zip(sequences, clocks)) or
                (lineage['lifetime'] == 0 and lineage['write_sequence'] != 0)):
            return False
        if (presence != 'unknown' or rmc_status != 'unknown') and not sequences[5]:
            return False
        return cls.payload(position)

    def entry(self, key, source):
        if self.exhausted:
            return None
        if key not in self.entries:
            if len(self.entries) >= self.capacity:
                self.capacity_fault(source)
                return None
            self.entries[key] = dict(positions=0, position=None, record=None, conflict=False)
        return self.entries[key]

    def capacity_fault(self, source):
        if not self.exhausted:
            self.issue('lds_sideband_capacity', source,
                       'Bounded association state exhausted; this session cannot establish unique links')
        self.exhausted = True

    def position(self, row, valid_request, source):
        key = self.position_key(row) if valid_request else None
        if key is None:
            self.unkeyed['identity_unavailable'] += 1
            return
        entry = self.entry(key, source)
        if entry is None:
            self.unkeyed['state_capacity'] += 1
            return
        entry['positions'] += 1
        if entry['position'] is None:
            entry['position'] = row

    def record(self, row, source):
        self.records['rows'] += 1
        if not self.valid_record(row):
            self.records['malformed'] += 1
            self.issue('lds_sideband_malformed', source, 'Invalid diagnostic sideband record')
            return
        self.records['valid'] += 1
        if row['dropped_before'] or row['flags'] & 128:
            self.issue('lds_sideband_loss', source, 'Sender reports missing or uncountable observations')
        key = self.sideband_key(row['wire'])
        if key is None:
            self.records['identity_unavailable'] += 1
            return
        entry = self.entry(key, source)
        if entry is None:
            return
        identity = (row['sender_uid'], row['sender_pid'], row['source_instance'], row['sequence'])
        canonical = json.dumps({k: v for k, v in row.items() if k != 'mono_ns'}, sort_keys=True)
        if identity in self.ids:
            old_key, old_record = self.ids[identity]
            if old_key == key and old_record == canonical:
                self.records['duplicates'] += 1
                return
            entry['conflict'] = self.entries[old_key]['conflict'] = True
            self.issue('lds_sideband_record_reused', source, 'One sender record identity has conflicting observations')
        else:
            if len(self.ids) >= self.capacity:
                self.capacity_fault(source)
                return
            self.ids[identity] = (key, canonical)
            source_key = identity[:3]
            previous = self.sources.get(source_key)
            if previous is not None and (row['sequence'] <= previous[0] or row['dropped_before'] < previous[1]):
                self.issue('lds_sideband_sequence', source, 'Sender order or cumulative loss counter regressed')
            self.sources[source_key] = (row['sequence'], row['dropped_before'])
        if entry['record'] is not None:
            entry['conflict'] = True
        else:
            entry['record'] = row
        wire = row['wire']
        if (row['flags'] & 16 or wire['reply_serial'] != wire['request_serial'] or
                (self.complete(wire['destination']) and wire['destination']['value'] != wire['client_unique']['value'])):
            entry['conflict'] = True

    def status(self, row, source):
        if (row.get('schema') != 1 or type(row.get('schema')) is not int or
                row.get('association_only') is not True or row.get('assist_ready') is not False or
                row.get('status') not in ('opened', 'unavailable', 'rejected', 'drain_limit', 'closed') or
                row.get('reason') not in ('none', 'syscall_failed', 'truncated', 'credentials_missing',
                                         'credentials_mismatch', 'bad_record') or
                not all(self.uint(row.get(k)) for k in ('mono_ns', 'count', 'sender_uid')) or
                not bounded_int(row.get('sender_pid'), 0, 2**31-1) or
                not bounded_int(row.get('syscall_errno'), 0, 2**31-1)):
            self.issue('lds_sideband_malformed', source, 'Invalid sideband receiver status')
            return
        self.statuses[row['status']] += 1
        if row['status'] in ('unavailable', 'rejected'):
            self.issue('lds_sideband_transport', source, row['status'] + ': ' + row['reason'])

    @staticmethod
    def assignment_pattern(lineage):
        # A known field origin identifies an observed assignment during a
        # cache write. Not every write establishes one. Count distinct origins
        # retained on these fields, never producer fixes or measurement epochs.
        # Equal observer clocks cannot merge write identities.
        sequences = lineage['field_write_sequences']
        known = {sequence for sequence in sequences if sequence}
        coverage = 'all_fields' if all(sequences) else (
            'some_fields' if known else 'no_fields')
        origins = 'multiple_distinct_known_assignments' if len(known) > 1 else (
            'one_distinct_known_assignment' if known else
            'no_known_field_assignment' if lineage['write_sequence'] else
            'no_tracked_cache_write_in_lifetime' if lineage['lifetime'] else
            'observation_lifetime_unavailable')
        return coverage, origins

    def current(self):
        counts, sideband, attachments = self.unkeyed.copy(), Counter(), []
        patterns, presence_counts, rmc_status_counts = Counter(), Counter(), Counter()
        for key, entry in self.entries.items():
            count, row, record = entry['positions'], entry['position'], entry['record']
            if not count:
                if record is not None:
                    sideband['observation_fault' if self.observation_fault else
                             'state_capacity' if self.exhausted else
                             'conflict' if entry['conflict'] else 'missing_position'] += 1
                continue
            if self.observation_fault:
                state = 'observation_fault'
            elif self.exhausted:
                state = 'state_capacity'
            elif entry['conflict']:
                state = 'conflict'
            elif count > 1:
                state = 'ambiguous_position'
            elif (record is None or record['flags'] & 111 != 111 or record['reply_type'] != 2 or
                  record['send_result'] == 0 or
                  not self.complete(record['wire']['destination'])):
                state = 'missing_sideband'
            elif any(k not in row for k in self.POSITION_FIELDS):
                state = 'payload_incomplete'
            elif not self.payload(row):
                state = 'payload_malformed'
            elif any(row[k] != record['position'][k] for k in self.POSITION_FIELDS):
                state = 'payload_mismatch'
            else:
                state = 'matched'
                coverage, origins = self.assignment_pattern(record['field_lineage'])
                patterns[coverage + '/' + origins] += 1
                # Legacy absence carries no lexical evidence. A numeric zero
                # or a new assignment identity cannot fill this information.
                presence = record['field_lineage'].get('heading_presence', 'unknown')
                presence_counts[presence] += 1
                # Status belongs to the HEADING assignment. A newer numeric
                # MODE may have been written by GGA/GSA; it cannot recover the
                # earlier RMC token or qualify the current receiver.
                rmc_status = record['field_lineage'].get('heading_rmc_status', 'unknown')
                rmc_status_counts[rmc_status] += 1
                attachments.append(dict(recorded_session=self.session_index, call=row['call'],
                    generation=row['generation'], source_instance=record['source_instance'],
                    sequence=record['sequence'], sender_pid=record['sender_pid'], sender_uid=record['sender_uid'],
                    path_result=record['path_result'], send_result=record['send_result'], flags=record['flags'],
                    wire_key=list(key), field_lineage=record['field_lineage'],
                    assignment_coverage=coverage, known_field_assignments=origins,
                    heading_presence=presence, heading_rmc_status=rmc_status))
            counts[state] += count
            if record is not None:
                sideband[state] += 1
        return counts, sideband, attachments, patterns, presence_counts, rmc_status_counts

    def report(self):
        counts, sideband, attachments, patterns, presence, rmc_status = self.current()
        counts.update(self.finished)
        sideband.update(self.finished_sideband)
        patterns.update(self.finished_assignment_patterns)
        presence.update(self.finished_heading_presence)
        rmc_status.update(self.finished_heading_rmc_status)
        total = self.attachment_total + len(attachments)
        shown = self.attachments + attachments[:max(0, self.attachment_limit-len(self.attachments))]
        return dict(position_links=dict(counts), sideband_links=dict(sideband),
                    records=dict(self.records), statuses=dict(self.statuses),
                    duplicates=self.records['duplicates'], attachments=shown, omitted_attachments=total-len(shown),
                    state_capacity=self.capacity, scope='same_trace_group_and_recorded_session',
                    assignment_patterns=dict(patterns),
                    heading_presence=dict(presence),
                    heading_presence_scope='lexical_course_token_only_not_numeric_or_quality',
                    heading_rmc_status=dict(rmc_status),
                    heading_rmc_status_scope='lexical_status_of_heading_assignment_not_receiver_quality',
                    assignment_scope='matched_wire_payload_retained_field_origins_not_producer_or_assist',
                    producer_time_status='unknown', association_only=True, assist_ready=False)


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
        self.request_wire_headers = Counter()
        self.request_wire_errors = Counter()
        self.request_endpoints = Counter()
        self.owners = Counter()
        self.receivers = Counter()
        self.runtime_modes = Counter()
        self.audit_faults = Counter()
        self.installs = Counter()
        self.boots = []
        self.health = []
        self.issues = []
        self.issue_counts = Counter()
        self.lds = LdsLinks(self.issue)
        self.files = []
        self.ignored = []
        self.total_bytes = 0
        self.session = None
        self.trace_group = None
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
        self.holdout_reference_links = Counter()
        self.holdout_completed = 0
        self.holdout_aborted = 0
        self.motion_rejected_reasons = Counter()
        self.motion_rejected_sensors = Counter()
        self.motion_late = dict(bursts=0, events=0, max_late_ms=0)
        # log_profile=persistent (validation/PERSISTENT_LOGGING_2026-10-06.md).
        self.log_profiles = Counter()
        self.persistent = dict(digests=0, digest_kinds=Counter(), raw_windows=0, raw_window_rows=0,
                               raw_window_triggers=Counter(), digested_motion_events=0,
                               digested_sends=0, unpaired_original_sends=0, suppressed=Counter())
        self.journal_dropped = dict(counter_rows=0, rows=0)
        self.journal_lag = Counter()
        self.capture_ends = 0
        self.holdout_position = dict(count=0, min=None, max=None, mean=None)
        self.holdout_heading = dict(count=0, min=None, max=None, mean=None)
        self.beta_boots = []
        self.beta_states = Counter()
        self.beta_transition_reasons = Counter()
        self.beta_engaged_periods = 0
        self.beta_engaged_seconds = dict(count=0, min=None, max=None, mean=None)
        self.beta_exit_reasons = Counter()
        self.beta_withdraw_reasons = Counter()
        self.beta_open_periods = 0
        self.beta_replacements = 0
        self.beta_replaced_nonzero = 0
        self.beta_accuracy_m = dict(count=0, min=None, max=None, mean=None)
        self.beta_hold_events = Counter()
        self.beta_storage_changes = 0
        self.beta_last_state = None
        self.beta_last_summary = None
        self.beta_returns = []
        self.beta_returns_total = 0
        self.beta_position_classes = Counter()
        self.beta_state_seconds = Counter()
        self.beta_no_fix_seconds = dict(count=0, min=None, max=None, mean=None)
        self.beta_speed_engaged_periods = 0
        self.beta_speed_engaged_seconds = dict(count=0, min=None, max=None, mean=None)
        self.beta_speed_overlays = 0
        self.beta_speed_overlay_nonzero = 0
        self.beta_speed_overlay_mps = dict(count=0, min=None, max=None, mean=None)
        self.beta_speed_overlay_wheel = Counter()  # checked / unverified (no wheel row in the lease)
        self.beta_speed_overlay_error_mps = dict(count=0, min=None, max=None, mean=None)
        self.beta_anchor_gates = Counter()
        self.beta_anchor_dropped = 0
        self.beta_reverse_latch = Counter()

    def issue(self, code, source, detail, violation=False):
        severity = "violation" if violation else "inconclusive"
        self.issue_counts[severity] += 1
        if len(self.issues) < 100:
            self.issues.append(dict(severity=severity, code=code, source=source, detail=detail))

    def new_session(self, boot=None):
        self.close_beta_session()
        self.lds.begin()
        profile = boot.get("log_profile", "full") if isinstance(boot, dict) else "full"
        self.session = dict(boot=boot, log_profile=profile if profile in ("full", "persistent") else "full",
                            last_send_ns=-1, health_ns=-1, sends=0,
                            dropped_max=0, health_records=0, motion_epoch=None,
                            motion_seq=0, motion_ns=0, last_diagnostic_ns=-1,
                            shadow_resets=0, shadow_rejected=0, shadow_pipeline=None,
                            holdout_window=None, capture_end_ns=None, model_session=None, model_bus=None,
                            # state: last beta_state "to"; left_ns: when it last left a
                            # live state; pending: last replaced LOCATION awaiting a GPS fix.
                            beta=dict(state=None, live_seen=False, left_ns=None,
                                      gps_returned=False, engaged_ns=None, engaged_source=None,
                                      pending=None, hold_seen=0, closed=False,
                                      # NO_FIX overlay family (NO_FIX/SPEED_ENGAGED).
                                      speed_live_seen=False, speed_left_ns=None,
                                      state_ns=None, no_fix_ns=None, speed_engaged_ns=None,
                                      # Overlay sends and wheel receipts, checked at close
                                      # (motion batches can be journaled after the send).
                                      overlays=[], wheels=[], wheel_profile=WHEEL_PROFILE,
                                      # Intervals with rows lost by the journal writer
                                      # (journal_dropped): [last time before, first after].
                                      journal_gaps=[], gap_open=None, last_time_ns=None))
        self.sessions.append(self.session)
        self.positions = {}
        self.invalid_positions = set()
        self.ambiguous_positions = set()
        self.matched_holdout_references = Counter()
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

    def consume(self, row, source, group=None):
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
        if not collector and group is not None and group != self.trace_group:
            # Keep one current group, not a cache of independent exports. A
            # missing boot starts a partial session and cannot borrow raw rows
            # or health/window state from the previous directory/archive.
            self.trace_group = group
            self.session = None
        if kind == "boot":
            self.new_session(row)
            self.boots.append(row)
            if not self.validate(row, source, ("schema", "pid", "mono_ns", "mode"),
                                 ("install", "assist_block"), ("assist_ready", "wire_timestamp_modified")):
                return
            self.installs[row["install"]] += 1
            if "log_profile" in row:
                if row["log_profile"] not in ("full", "persistent"):
                    self.issue("partial_record", source, "Unknown boot log_profile")
                else:
                    self.log_profiles[row["log_profile"]] += 1
            if row["schema"] != 1:
                self.issue("unsupported_schema", source, str(row["schema"]))
            if row["mode"] not in (1, 2, 4, 5):
                self.issue("unexpected_boot_mode", source,
                           "Live worker requires OBSERVE=1, SCRUB=2, SHADOW=4, or BETA=5", True)
            if row["install"] != "ok":
                self.issue("install_not_ok", source, row["install"])
            if row["assist_ready"] or row["wire_timestamp_modified"]:
                self.issue("impossible_live_capability", source, "Boot claims unsupported live capability", True)
            self.beta_boot(row, source)
            return
        if self.session is None and not collector:
            self.new_session()
        if not collector and kind != "journal_dropped" and integer(row.get("mono_ns")):
            # Rows written from a RAW window (count from its marker) carry
            # their older buffered times: they never close a gap.
            window_row = self.session.get("raw_flush_left", 0) > 0
            if window_row:
                self.session["raw_flush_left"] -= 1
            self.note_time(row["mono_ns"], closes=not window_row and kind not in GAP_NON_CLOSING_KINDS)
        elif not collector and kind != "journal_dropped" and self.session.get("raw_flush_left", 0) > 0:
            self.session["raw_flush_left"] -= 1
        if (not collector and self.session.get("capture_end_ns") is not None and
                kind not in ("health", "capture_end")):
            self.issue("record_after_capture_end", source, kind)
        if kind == "position":
            request_valid = self.request_record(row, source, count=True)
            if not self.validate(row, source, ("call", "generation", "mono_ns", "mode", "utc_s")):
                return
            reason = row.get("reason", 0)  # Earlier journals have no POSITION reason.
            if not bounded_int(reason, 0, len(REASONS)-1) or reason not in (0, 13):
                self.issue("partial_record", source, "Invalid POSITION reason")
                return
            self.position_numbers(row, source)
            self.modes[str(row["mode"])] += 1
            key = (row["call"], row["generation"])
            if key in self.positions:
                self.issue("duplicate_position", source, "Ambiguous call/generation correlation")
                self.ambiguous_positions.add(key)
                matched = self.matched_holdout_references.pop(key, 0)
                if matched:
                    # A later duplicate also invalidates earlier unique joins.
                    self.holdout_reference_links["matched"] -= matched
                    if not self.holdout_reference_links["matched"]:
                        del self.holdout_reference_links["matched"]
                    self.holdout_reference_links["ambiguous"] += matched
                    self.issue("holdout_reference_ambiguous", source,
                               "Later duplicate invalidates %d prior reference matches" % matched)
            self.positions[key] = row
            if reason == REASONS.index("CONTEXT_UNAVAILABLE"):
                self.invalid_positions.add(key)
                self.lds.invalidate_observation()
                self.issue("adapter_context_unavailable", source,
                           "POSITION context unavailable; pool capacity or nesting depth exceeded")
            else:
                self.lds.position(row, request_valid, source)
            self.beta_position(row, source)
        elif kind in BETA_KINDS:
            self.beta(row, source)
        elif kind == 'lds_sideband':
            self.lds.record(row, source)
        elif kind == 'lds_sideband_status':
            self.lds.status(row, source)
        elif kind == "send":
            self.send(row, source)
        elif kind == "capture_incomplete":
            if not self.validate(row, source, strings=("reason",), bools=("assist_ready",)):
                return
            if row["assist_ready"] is not False or row["reason"] not in (
                    "observation_pending", "adapter_context_unavailable", "adapter_fault"):
                self.issue("malformed_capture_incomplete", source,
                           "Unknown capture failure or unsupported capability", True)
            else:
                self.lds.invalidate_observation()
                self.issue("capture_incomplete", source, row["reason"])
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
                self.lds.invalidate_observation()
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
                self.lds.invalidate_observation()
                self.issue("audit_fault", source, "Runtime disabled mutation after an observation or logging fault")
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
        elif kind == "motion_late_accepted":
            self.late_motion(row, source)
        elif kind == "log_digest" or kind.endswith("_digest"):
            self.digest(row, source)
        elif kind == "raw_window":
            self.raw_window(row, source)
        elif kind == "journal_dropped":
            self.dropped_rows(row, source)
        elif kind == "beta_journal_lag":
            self.journal_lag_row(row, source)
        elif kind == "journal_not_durable":
            self.journal_lag["not_durable"] += 1
            self.issue("journal_not_durable", source,
                       "Boot row did not reach the journal file in time; SCRUB/BETA stayed off "
                       "(occurrence count is a lower bound)")
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
        elif kind in ("shadow_boot", "shadow", "shadow_input_reset", "shadow_disabled",
                      "shadow_pipeline_reset"):
            if (kind == "shadow_boot" and bounded_number(row.get("wheel_kmh_per_count"), 0.0001, 1) and
                    bounded_number(row.get("wheel_zero_kmh"), -1000, 1000)):
                self.session["beta"]["wheel_profile"] = (row["wheel_kmh_per_count"], row["wheel_zero_kmh"])
            self.shadow(row, source)
        elif kind == "beta_reverse_latch":
            self.reverse_latch(row, source)
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

    def digest(self, row, source):
        """Persistent-profile summary of the rows it replaced (raw motion,
        ORIGINAL sends, rate-limited diagnostics). Counted only; a digest is
        never accepted motion, a send or a MODEL result. Unknown digest kinds
        and fields are tolerated."""
        if (not bounded_int(row.get("schema"), 1, 1) or not bounded_int(row.get("mono_ns"), 0, 2**64-1) or
                ("assist_ready" in row and row["assist_ready"] is not False)):
            self.issue("malformed_digest", source, "Invalid digest envelope")
            return
        p = self.persistent
        p["digests"] += 1
        p["digest_kinds"][row["kind"] + ":" + str(row.get("digest", "unspecified"))] += 1
        if bounded_int(row.get("motion_events"), 0, 2**64-1):
            p["digested_motion_events"] += row["motion_events"]
        if bounded_int(row.get("sends"), 0, 2**64-1):
            p["digested_sends"] += row["sends"]
        # Rate-limited rows the digest counted instead of writing: keep them
        # in the totals so a verdict does not under-report rejections.
        suppressed = row.get("suppressed")
        if isinstance(suppressed, dict):
            for key, value in suppressed.items():
                if isinstance(key, str) and bounded_int(value, 0, 2**64-1):
                    p["suppressed"][key] += value

    def raw_window(self, row, source):
        """Start of a persistent-profile raw period: the in-memory window of
        older raw rows follows, then raw rows are written directly for a
        while. Raw rows before it are absent by design, so accepted-motion
        sequence continuity restarts here (not healed, not a gap issue)."""
        if (not bounded_int(row.get("schema"), 1, 1) or not bounded_int(row.get("mono_ns"), 0, 2**64-1) or
                not bounded_int(row.get("rows"), 0, 2**64-1) or not isinstance(row.get("trigger"), str)):
            self.issue("malformed_raw_window", source, "Invalid raw window marker")
            return
        p = self.persistent
        p["raw_windows"] += 1
        p["raw_window_rows"] += row["rows"]
        p["raw_window_triggers"][row["trigger"]] += 1
        self.session["motion_epoch"] = None
        self.session["raw_flush_left"] = row["rows"]

    def journal_lag_row(self, row, source):
        """The worker withheld BETA provenance because the journal writer was
        more than limit_ms behind (lagging) and restored it after catch-up
        (current). A replacement decided well after 'lagging' contradicts it."""
        if (row.get("domain") != "beta" or row.get("assist_ready") is not False or
                row.get("event") not in ("lagging", "current") or
                not bounded_int(row.get("mono_ns"), 0, 2**64-1)):
            self.issue("partial_record", source, "Invalid beta_journal_lag row")
            return
        self.journal_lag[row["event"]] += 1
        beta = self.session["beta"]
        if row["event"] == "lagging":
            beta["journal_lag_ns"] = row["mono_ns"]
            self.issue("beta_journal_lag", source, "Journal writer %s ms behind; BETA withheld" % row.get("lag_ms"))
        else:
            beta["journal_lag_ns"] = None

    def lag_violation(self, row, source, what):
        lag = self.session["beta"].get("journal_lag_ns")
        # One POSITION call may already hold provenance when the flag drops.
        if lag is not None and row["mono_ns"] > lag + 100000000:
            self.issue("beta_change_during_journal_lag", source,
                       "%s after the journal writer fell behind (BETA must be withheld)" % what, True)

    def note_time(self, ns, closes=True):
        """Row times (one monotonic clock) bound journal_dropped gaps: a gap
        runs from the last time before the counter row to the first later
        row that was timed by the worker when it was queued (closes) and is
        not earlier than the gap's start. Hook-time POSITION/SEND rows,
        producer-time motion rows and RAW-window rows keep the gap open:
        their times can precede rows that were dropped."""
        beta = self.session["beta"]
        if beta["gap_open"] is not None and closes and ns >= beta["gap_open"]:
            beta["journal_gaps"].append((beta["gap_open"], ns))
            beta["gap_open"] = None
        if beta["last_time_ns"] is None or ns > beta["last_time_ns"]:
            beta["last_time_ns"] = ns

    def dropped_rows(self, row, source):
        """The journal writer dropped the oldest DIAGNOSTIC rows under a write
        backlog (evidence rows are never dropped). The loss is real: count it
        and restart motion continuity at this point."""
        if not bounded_int(row.get("rows"), 1, 2**64-1) or row.get("class") != "diagnostic":
            self.issue("malformed_journal_dropped", source, "Invalid dropped-row counter")
            return
        self.journal_dropped["counter_rows"] += 1
        self.journal_dropped["rows"] += row["rows"]
        beta = self.session["beta"]
        if beta["gap_open"] is None:
            beta["gap_open"] = beta["last_time_ns"] if beta["last_time_ns"] is not None else 0
        self.session["motion_epoch"] = None
        self.issue("journal_rows_dropped", source, "%d diagnostic rows dropped under a write backlog" % row["rows"])

    def late_motion(self, row, source):
        # Accepted records that waited > fresh_limit_ms in the socket queue
        # while the worker was stalled (same producer, contiguous, monotonic,
        # at most late_limit_ms). Diagnostic only: the records themselves are
        # in the motion rows with their unchanged producer receipt times.
        if (row.get("domain") != "model" or row.get("assist_ready") is not False or
                not bounded_int(row.get("schema"), 1, 1) or
                any(not bounded_int(row.get(k), 0, 2**64-1) for k in
                    ("mono_ns", "epoch", "first_seq", "last_seq", "events", "max_late_ms",
                     "fresh_limit_ms", "late_limit_ms")) or
                row["last_seq"] < row["first_seq"] or not row["events"]):
            self.issue("malformed_late_motion", source, "Invalid late-arrival diagnostic")
            return
        # The exact age is in ns (newer rows); the ms field is truncated, so
        # 250 ms + 1 ns reads as 250 ms and only the legacy check uses <=.
        if bounded_int(row.get("max_late_ns"), 0, 2**64-1):
            late_ok = row["fresh_limit_ms"] * 1000000 < row["max_late_ns"] <= row["late_limit_ms"] * 1000000
            shown = "%d ns" % row["max_late_ns"]
        else:
            late_ok = row["fresh_limit_ms"] <= row["max_late_ms"] <= row["late_limit_ms"]
            shown = "%d ms" % row["max_late_ms"]
        if not late_ok:
            self.issue("late_motion_out_of_bounds", source,
                       "Late arrival %s outside (%d, %d] ms" % (
                           shown, row["fresh_limit_ms"], row["late_limit_ms"]), True)
        m = self.motion_late
        m["bursts"] += 1
        m["events"] += row["events"]
        m["max_late_ms"] = max(m["max_late_ms"], row["max_late_ms"])
        s = self.session
        s["last_diagnostic_ns"] = max(s["last_diagnostic_ns"], row["mono_ns"])

    def motion(self, row, source):
        self.motion_samples += 1
        self.motion_sensors[str(row["sensor"])] += 1
        s = self.session
        if row["sensor"] == 1 and len(s["beta"]["wheels"]) < 1000000:
            speed = wheel_speed_e3(row["raw"], s["beta"]["wheel_profile"])
            if speed is not None:
                s["beta"]["wheels"].append((row["received_ns"], speed))
        s["last_diagnostic_ns"] = max(s["last_diagnostic_ns"], row["received_ns"])
        self.note_time(row["received_ns"], closes=False)
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
        # BETA design decision 6: only a boot that journaled the KNOWN session
        # hook decline admits an unobserved session as MODEL input; the
        # send-time storage counter then replaces the session fence.
        boot = self.session["boot"] or {}
        declined = (isinstance(boot.get("beta"), dict) and boot.get("mode") == 5 and
                    boot["beta"].get("session_fence") == "declined_send_storage_counter")
        admitted = (isinstance(observed, dict) and
                    (observed.get("result") == "observed" or
                     (declined and observed.get("result") == "unobserved")))
        if (not session_snapshot(observed, "unique_live_context") or "revision" not in observed or
                type(row.get("reset")) is not bool or type(row.get("input_available")) is not bool or
                row["input_available"] != admitted or
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
        if kind == "shadow_pipeline_reset":
            if not self.validate(row, source,
                    ints=("mono_ns", "input_ns", "receive_seq", "sensor", "call", "resets"),
                    strings=("reason", "operation")):
                return
            reasons = ("BAD_INPUT", "LATE", "CLOCK_RESET", "SOURCE_RESET",
                       "OVERFLOW", "MISSING_SENSOR", "CORE_REJECTED")
            if (row.get("domain") != "model" or row["reason"] not in reasons or
                    row["operation"] not in ("raw", "position", "drain") or
                    any(not bounded_int(row[key], 0, 2**64-1) for key in
                        ("mono_ns", "input_ns", "receive_seq", "sensor", "call", "resets")) or
                    not row["mono_ns"] or not row["resets"] or
                    (row["operation"] == "raw" and
                     (row["sensor"] not in (1, 2, 3) or not row["receive_seq"]))):
                self.issue("partial_record", source, "Invalid primary MODEL reset diagnostic")
                return
            self.issue("shadow_pipeline_reset", source,
                       "%s: %s receive_seq=%d" %
                       (row["operation"], row["reason"], row["receive_seq"]))
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
        if ('drain_calls_total' in row and
                not bounded_int(row['drain_calls_total'], 0, 2**64-1)):
            self.issue('partial_record', source, 'Invalid MODEL drain invocation counter')
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

    def holdout_reference(self, row, source):
        """Add diagnostics without removing otherwise valid MODEL comparisons."""
        fields = ("reference_call", "reference_generation")
        present = [key in row for key in fields]
        counts = self.holdout_reference_links
        if not any(present):
            counts["legacy_without_identity"] += 1
            return
        values = tuple(row.get(key) for key in fields)
        if all(present) and values == (None, None) and row["event"] not in ("BEGIN", "COMPARED", "SKIPPED"):
            counts["no_reference"] += 1
            return
        terminal_without_reference = (row["event"] == "END" or
                                      (row["event"] == "ABORT" and row["reason"] != "output_overflow"))
        if (not all(present) or terminal_without_reference or
                any(not bounded_int(value, 0, 2**32-1) for value in values)):
            counts["malformed"] += 1
            self.issue("partial_record", source, "Holdout reference requires two uint32 identifiers or an absent reference")
            return
        if values in self.ambiguous_positions:
            counts["ambiguous"] += 1
            self.issue("holdout_reference_ambiguous", source, "Reference call/generation has multiple raw rows")
            return
        if values in self.invalid_positions:
            counts["raw_unavailable"] += 1
            self.issue("holdout_reference_unavailable", source,
                       "Adapter could not retain the referenced POSITION context")
            return
        position = self.positions.get(values)
        if position is None:
            counts["raw_missing"] += 1
            self.issue("holdout_reference_missing", source, "No prior raw position for this reference in the recorded session")
            return
        # Time and coordinates check the already selected row; they are never
        # fallback keys. No producer timing or held-out-input proof is implied.
        if (position["mono_ns"] != row["reference_ns"] or
                (row["event"] == "COMPARED" and any(
                    not finite_number(position.get(raw)) or position[raw] != row[reference]
                    for raw, reference in (("lat", "ref_lat"), ("lon", "ref_lon"))))):
            counts["mismatch"] += 1
            self.issue("holdout_reference_mismatch", source, "Reference identity disagrees with raw receipt time or coordinates", True)
            return
        counts["matched"] += 1
        self.matched_holdout_references[values] += 1

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
        if (event not in ("BEGIN", "COMPARED", "END", "ABORT", "SKIPPED") or
                not bounded_int(row["window_id"], 0 if event in ("ABORT", "SKIPPED") else 1, 2**64-1) or
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
        self.holdout_reference(row, source)
        anchor, reference, frontier = row["anchor_ns"], row["reference_ns"], row["frontier_ns"]
        if event == "SKIPPED":
            if (row["reason"] != "stale_reference" or row["window_id"] != 0 or
                    anchor or frontier or not reference or reference >= row["mono_ns"] or
                    row["model_valid"] or row["position_error_m"] is not None or
                    row["heading_error_rad"] is not None or
                    any(row[key] is not None for key in ("lat", "lon", "ref_lat", "ref_lon"))):
                self.issue("invalid_holdout_time", source, "SKIPPED must identify one stale pre-window reference")
                return
            if self.session["holdout_window"] is not None:
                self.issue("invalid_holdout_boundary", source, "SKIPPED cannot belong to an open holdout window")
                return
            if any(row["mono_ns"] < boundary["mono_ns"] for boundary in
                   (s for s in (self.session["model_bus"], self.session["model_session"]) if s is not None)):
                self.issue("invalid_holdout_boundary", source, "SKIPPED predates the current MODEL boundary", True)
                return
            self.issue("holdout_reference_stale", source, "GPS reference expired before holdout submission")
            return
        warmup_abort = event == "ABORT" and row["window_id"] == 0
        # A full result queue can rewrite a pre-window SKIPPED observation to
        # ABORT/output_overflow. Its actual reference survives; it is still an
        # inconclusive abort with no active window or prediction.
        reference_keys = ("reference_call", "reference_generation")
        overflow_reference_shape = (not any(key in row for key in reference_keys) or
            all(bounded_int(row.get(key), 0, 2**32-1) for key in reference_keys))
        warmup_reference_overflow = (warmup_abort and row["reason"] == "output_overflow" and
            overflow_reference_shape and
            not row["model_valid"] and all(row[key] is None for key in ("lat", "lon", "ref_lat", "ref_lon")))
        if ((not anchor and not warmup_abort) or max(anchor, reference, frontier) > row["mono_ns"] or
                (warmup_abort and (anchor or frontier or (reference and not warmup_reference_overflow)))):
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
        def wire_record(value):
            if not isinstance(value, dict):
                return False
            issue, reply = value.get("issue"), value.get("reply")
            if not (isinstance(issue, dict) and isinstance(reply, dict) and
                    isinstance(issue.get("known"), bool) and isinstance(issue.get("conflict"), bool) and
                    isinstance(reply.get("known"), bool) and
                    all(k in issue for k in ("observed_ns", "serial")) and
                    all(k in reply for k in ("observed_ns", "serial", "reply_serial", "type")) and
                    all(text(reply.get(k)) for k in ("sender", "error"))):
                return False
            if "endpoint_matched" in issue and (
                    not isinstance(issue["endpoint_matched"], bool) or
                    (issue["endpoint_matched"] and (not issue["known"] or issue["conflict"]))):
                return False
            for header in (issue, reply):
                clock = header["observed_ns"]
                if clock is not None and not (unsigned(clock) and clock > 0):
                    return False
            if issue["known"]:
                if not (unsigned(issue["serial"], 32) and issue["serial"] > 0):
                    return False
            elif issue["observed_ns"] is not None or issue["serial"] is not None:
                return False
            if reply["known"]:
                # These are raw DBus types (not the older JCIDBUS enum). A local
                # NoReply header may have serial/reply_serial zero and no sender.
                return (unsigned(reply["serial"], 32) and unsigned(reply["reply_serial"], 32) and
                        integer(reply["type"]) and reply["type"] in (1, 2, 3, 4))
            return (all(reply[k] is None for k in ("observed_ns", "serial", "reply_serial", "type")) and
                    all(reply[k]["value"] is None for k in ("sender", "error")))
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
        if valid and "endpoint" in t:
            endpoint = t["endpoint"]
            fields = ("server_guid", "unique_name")
            valid = isinstance(endpoint, dict) and all(text(endpoint.get(k)) for k in fields)
            if valid and t["result"] != "observed":
                valid = all(endpoint[k]["value"] is None for k in fields)
        if valid and "wire" in t:
            valid = wire_record(t["wire"])
            if valid and t["result"] != "observed":
                valid = (not t["wire"]["issue"]["known"] and not t["wire"]["issue"]["conflict"] and
                         not t["wire"]["reply"]["known"])
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
                if valid and "endpoint" in t and t["issue_connection"]["result"] != "connected":
                    valid = all(t["endpoint"][k]["value"] is None for k in ("server_guid", "unique_name"))
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
            if "endpoint" in t:
                endpoint = t["endpoint"]
                fields = ("server_guid", "unique_name")
                complete = all(endpoint[k]["complete"] and endpoint[k]["value"] for k in fields)
                self.request_endpoints["records"] += 1
                state = ("complete" if complete else
                         "unavailable" if all(endpoint[k]["value"] is None for k in fields)
                         else "incomplete")
                self.request_endpoints[state] += 1
                wire_issue = t.get("wire", {}).get("issue", {})
                # A validated known issue has a nonzero uint32 serial. Count
                # availability only. The separate LDS diagnostic join needs
                # the reply key too; no reply/receipt clock proves freshness.
                self.request_endpoints["exact_request_key_records"] += int(
                    complete and wire_issue.get("known", False) and
                    wire_issue.get("endpoint_matched", False) and not wire_issue["conflict"])
            else:
                self.request_endpoints["legacy_without_endpoint"] += 1
            if "wire" in t:
                wire_issue, wire_reply = t["wire"]["issue"], t["wire"]["reply"]
                self.request_wire_headers["records"] += 1
                self.request_wire_headers["issue_known"] += int(wire_issue["known"])
                self.request_wire_headers["reply_known"] += int(wire_reply["known"])
                self.request_wire_headers["issue_conflicts"] += int(wire_issue["conflict"])
                if "endpoint_matched" in wire_issue:
                    self.request_wire_headers["issue_endpoint_match_records"] += 1
                    self.request_wire_headers["issue_endpoint_matched"] += int(wire_issue["endpoint_matched"])
                self.request_wire_headers["reply_without_remote_serial"] += int(
                    wire_reply["known"] and wire_reply["serial"] == 0)
                if wire_reply["known"] and wire_reply["error"]["complete"]:
                    self.request_wire_errors[wire_reply["error"]["value"]] += 1
        if "wire" in t:
            wire_issue, wire_reply = t["wire"]["issue"], t["wire"]["reply"]
            if wire_issue["conflict"]:
                self.issue("request_wire_issue_conflict", source, "One request has conflicting raw submission observations")
            if (wire_issue["known"] and not wire_issue["conflict"] and wire_reply["known"] and
                    wire_reply["reply_serial"] > 0 and wire_reply["reply_serial"] != wire_issue["serial"]):
                self.issue("request_wire_reply_mismatch", source,
                           "Raw reply_serial differs from the associated request's submitted serial", True)
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
        return True

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
        if (position and position.get("reason", 0) != REASONS.index("CONTEXT_UNAVAILABLE") and
                row["reason"] != REASONS.index("CONTEXT_UNAVAILABLE") and
                ("request" in position or "request" in row) and
                position.get("request") != row.get("request")):
            self.issue("request_copy_mismatch", source, "Position/send request metadata differ", True)
        s = self.session
        s["sends"] += 1
        s["last_send_ns"] = max(s["last_send_ns"], row["mono_ns"])
        choice = row["choice"]
        self.choices[CHOICES.get(choice, "UNKNOWN:" + str(choice))] += 1
        reason = row["reason"]
        self.reasons[REASONS[reason] if 0 <= reason < len(REASONS) else "UNKNOWN:" + str(reason)] += 1
        if ((row["call"], row["generation"]) in self.invalid_positions and
                (choice != 0 or reason == REASONS.index("PASS"))):
            self.issue("context_unavailable_send_inconsistent", source,
                       "A failed POSITION context cannot authorize a PASS or mutated SEND", True)
        if reason == REASONS.index("CONTEXT_UNAVAILABLE"):
            self.lds.invalidate_observation()
            self.issue("adapter_context_unavailable", source,
                       "SEND forwarded without a POSITION context")
        if reason == REASONS.index("EXTRA_LOCATION"):
            self.lds.invalidate_observation()
            self.issue("extra_location", source,
                       "Multiple LOCATION sends in one POSITION call disabled mutation")
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
        if p is None and choice == 0 and s["log_profile"] == "persistent":
            # A RAW window starts at an arbitrary row: its first ORIGINAL
            # send can precede the window. Changed sends keep this check.
            self.persistent["unpaired_original_sends"] += 1
        elif p is None:
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
        # A choice-4 overlay is checked (and rejected outside NO_FIX) below.
        if row["mode"] in (1, 2, 3) and choice != 4 and (original != outgoing or choice != 0):
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
        if choice == 3:
            self.beta_send(row, original, outgoing, source)
        elif choice == 4:
            self.beta_overlay(row, original, outgoing, source)

    def beta_boot(self, row, source):
        beta = row.get("beta")
        if beta is None:
            if row["mode"] == 5:
                self.issue("partial_record", source, "BETA boot lacks its beta object")
            return
        if (not isinstance(beta, dict) or beta.get("mode") not in ("off", "BETA") or
                type(beta.get("enabled")) is not bool or
                any(not isinstance(beta.get(k), str) for k in ("reason", "session_fence"))):
            self.issue("partial_record", source, "Invalid boot BETA object")
            return
        self.beta_boots.append(dict(beta, boot_mode=row["mode"], install=row["install"],
                                    session_hooks=row.get("session_hooks")))
        if (beta["mode"] == "BETA") != (row["mode"] == 5):
            self.issue("beta_boot_mode_mismatch", source, "BETA object disagrees with boot mode", True)
        elif beta["enabled"] and row["install"] != "ok":
            self.issue("beta_enabled_without_hook", source, "BETA opt-in claims an uninstalled hook", True)
        elif row["mode"] == 5 and not beta["enabled"]:
            self.issue("beta_not_enabled", source, beta["reason"])

    def beta_live_ok(self, beta, mono_ns):
        """A replacement needs a journaled GPS_LOST/ENGAGED state. The worker
        writes a withdrawal row after revoking; a send decided before that row
        (by its own monotonic time) can still be drained after it."""
        if beta["state"] in BETA_LIVE_STATES:
            return True
        return beta["live_seen"] and beta["left_ns"] is not None and mono_ns <= beta["left_ns"]

    def beta(self, row, source):
        kind = row["kind"]
        if not self.validate(row, source, ("mono_ns",)):
            return
        if row.get("domain") != "beta":
            self.issue("unexpected_beta_domain", source, "BETA rows must stay in the beta domain", True)
        if ("assist_ready" in row or kind in ("beta_state", "beta_summary")) and row.get("assist_ready") is not False:
            self.issue("impossible_live_capability", source, "BETA cannot authorize qualified ASSIST", True)
        boot = self.session["boot"]
        if boot is not None and boot.get("mode") != 5:
            self.issue("beta_row_without_beta_config", source, kind + " in a boot without mode BETA=5", True)
        for key in ("accuracy_m",):
            if key in row and row[key] is not None and not bounded_number(row[key], 0, 40):
                self.issue("beta_accuracy_out_of_range", source, "%s=%r outside 0..40 m" % (key, row[key]), True)
        beta = self.session["beta"]
        if kind == "beta_state":
            if not self.validate(row, source, strings=("from", "to", "reason")):
                return
            old, new, reason, now = row["from"], row["to"], row["reason"], row["mono_ns"]
            if old not in BETA_STATES or new not in BETA_STATES:
                self.issue("partial_record", source, "Unknown BETA state")
                return
            if beta["state"] is not None and beta["state"] != old:
                self.issue("beta_state_discontinuity", source,
                           "Transition from %s follows state %s" % (old, beta["state"]))
            if old == "FAULT":
                self.issue("beta_fault_not_sticky", source, "FAULT must not be left in one boot", True)
            self.beta_states[new] += 1
            # Optional newer field (LOST/NO_FIX/FIX); tolerated, never required.
            if isinstance(row.get("position_class"), str):
                self.beta_position_classes[row["position_class"]] += 1
            self.beta_transition_reasons[new + ":" + reason] += 1
            self.beta_last_state = dict(state=new, reason=reason, source=source)
            if new == "WITHDRAWN":
                self.beta_withdraw_reasons[reason] += 1
            if old == "ENGAGED" and new != "ENGAGED" and beta["engaged_ns"] is not None:
                if now >= beta["engaged_ns"]:
                    add_difference(self.beta_engaged_seconds, (now - beta["engaged_ns"]) / 1e9)
                else:
                    self.issue("beta_clock_regressed", source, "ENGAGED exit precedes its entry")
                self.beta_exit_reasons[reason] += 1
                beta["engaged_ns"] = None
            if new == "ENGAGED" and old != "ENGAGED":
                self.beta_engaged_periods += 1
                beta["engaged_ns"], beta["engaged_source"] = now, source
            if new == "GPS_LOST":
                beta["gps_returned"] = False
            if new in BETA_LIVE_STATES:
                beta["live_seen"], beta["left_ns"] = True, None
            elif old in BETA_LIVE_STATES:
                beta["left_ns"] = now
            if new in BETA_SPEED_STATES:
                beta["speed_live_seen"], beta["speed_left_ns"] = True, None
            elif old in BETA_SPEED_STATES:
                beta["speed_left_ns"] = now
            # Time per state, NO_FIX-family periods and SPEED_ENGAGED periods.
            if beta["state_ns"] is not None and beta["state"] == old and now >= beta["state_ns"]:
                self.beta_state_seconds[old] += (now - beta["state_ns"]) / 1e9
            beta["state_ns"] = now
            if new in BETA_SPEED_STATES and old not in BETA_SPEED_STATES:
                beta["no_fix_ns"] = now
            elif old in BETA_SPEED_STATES and new not in BETA_SPEED_STATES and beta["no_fix_ns"] is not None:
                if now >= beta["no_fix_ns"]:
                    add_difference(self.beta_no_fix_seconds, (now - beta["no_fix_ns"]) / 1e9)
                beta["no_fix_ns"] = None
            if new == "SPEED_ENGAGED" and old != "SPEED_ENGAGED":
                self.beta_speed_engaged_periods += 1
                beta["speed_engaged_ns"] = now
            elif old == "SPEED_ENGAGED" and new != "SPEED_ENGAGED" and beta["speed_engaged_ns"] is not None:
                if now >= beta["speed_engaged_ns"]:
                    add_difference(self.beta_speed_engaged_seconds, (now - beta["speed_engaged_ns"]) / 1e9)
                beta["speed_engaged_ns"] = None
            beta["state"] = new
        elif kind == "beta_summary":
            if self.validate(row, source, ("replaced_sends", "replaced_nonzero", "publications", "withdrawals"),
                             ("state", "reason")):
                self.beta_last_summary = {k: row[k] for k in (
                    "mono_ns", "state", "reason", "publications", "publish_skipped", "last_skip",
                    "withdrawals", "replaced_sends", "replaced_nonzero", "original_mode0_sends",
                    "transitions", "bridge", "accuracy_m", "position_class", "payload",
                    "speed_publications", "speed_overlay_sends", "speed_overlay_nonzero",
                    "original_nofix_sends") if k in row}
        elif kind == "beta_hold":
            if not self.validate(row, source, ("count",), ("event",)):
                return
            if row["event"] not in ("hold_set", "hold_cleared"):
                self.issue("partial_record", source, "Unknown BETA hold event")
                return
            self.beta_hold_events[row["event"] + "_rows"] += 1
            if row["event"] == "hold_set" and row["count"] > beta["hold_seen"]:
                self.beta_hold_events["hold_set"] += row["count"] - beta["hold_seen"]
                beta["hold_seen"] = row["count"]
        elif kind == "beta_anchor":
            # One anchor gate evaluation (BETA_DECISIONS 3.2); diagnostic only.
            if self.validate(row, source, ("seq", "mode", "utc_s", "dropped"), ("gate",)):
                self.beta_anchor_gates[row["gate"]] += 1
                self.beta_anchor_dropped = max(self.beta_anchor_dropped, row["dropped"])
        else:  # beta_session_storage
            if self.validate(row, source, ("session_epoch", "previous")):
                self.beta_storage_changes += 1

    def reverse_latch(self, row, source):
        """MODEL reverse latch kept across / cleared after an input gap, or
        cleared by the worker (with per-reason totals). Never BETA evidence."""
        if not self.validate(row, source, ("mono_ns",), ("event", "reason")):
            return
        if row.get("domain") != "model" or row.get("assist_ready", False) is not False:
            self.issue("unexpected_reverse_latch_domain", source, "Reverse latch rows are MODEL diagnostics", True)
            return
        self.beta_reverse_latch[row["event"] + ":" + row["reason"]] += 1

    def beta_overlay(self, row, original, outgoing, source):
        """BETA_SPEED_OVERLAY (choice 4): only on a NO_FIX send (original mode
        1/2 with GetPosition utc_s 0), only bytes 32 and 36..39 changed,
        hasSpeed 1 and a plausible speed. The wheel comparison runs at session
        close because motion batches may be journaled after the send."""
        s = self.session
        beta = s["beta"]
        self.lag_violation(row, source, "BETA_SPEED_OVERLAY")
        self.beta_speed_overlays += 1
        if row["result"] != 0:
            self.beta_speed_overlay_nonzero += 1
        if s["boot"] is not None and s["boot"].get("mode") != 5:
            self.issue("beta_without_beta_config", source, "BETA_SPEED_OVERLAY requires boot configuration BETA=5", True)
        if row["reason"] != 0:
            self.issue("beta_nonpass_reason", source, "BETA_SPEED_OVERLAY must have reason PASS", True)
        position = self.positions.get((row["call"], row["generation"]))
        if row["mode"] not in (1, 2):
            self.issue("beta_overlay_wrong_class", source,
                       "BETA_SPEED_OVERLAY requires original mode 1/2 (NO_FIX), got mode %d" % row["mode"], True)
        elif "class" in row and row["class"] != POSITION_CLASS_NO_FIX:
            self.issue("beta_overlay_wrong_class", source,
                       "BETA_SPEED_OVERLAY on adapter class %r, not NO_FIX" % row["class"], True)
        elif position is None or not integer(position.get("utc_s")):
            self.issue("beta_overlay_class_unverified", source, "No POSITION utc_s to confirm class NO_FIX")
        elif position["utc_s"] != 0:
            self.issue("beta_overlay_wrong_class", source,
                       "BETA_SPEED_OVERLAY on a fix (utc_s %d), only NO_FIX (utc_s 0) may be overlaid"
                       % position["utc_s"], True)
        if not (beta["state"] in BETA_SPEED_STATES or (
                beta["speed_live_seen"] and beta["speed_left_ns"] is not None and
                row["mono_ns"] <= beta["speed_left_ns"])):
            self.issue("beta_overlay_without_no_fix_state", source,
                       "No preceding beta_state NO_FIX/SPEED_ENGAGED covers this overlay", True)
        changed = [i for i in range(48) if original[i] != outgoing[i]]
        if any(i not in BETA_OVERLAY_BYTES for i in changed):
            self.issue("beta_overlay_payload_mismatch", source,
                       "Only bytes 32 and 36..39 may differ from the original (changed %s)" % changed, True)
            return
        if outgoing[32] != 1:
            self.issue("beta_overlay_payload_mismatch", source, "hasSpeed (byte32) must be 1", True)
        speed_e3 = int.from_bytes(outgoing[36:40], 'little', signed=True)
        if not 0 <= speed_e3 <= BETA_MAX_SPEED_E3:
            self.issue("beta_overlay_speed_out_of_range", source,
                       "speed_e3=%d outside 0..%d" % (speed_e3, BETA_MAX_SPEED_E3), True)
            return
        add_difference(self.beta_speed_overlay_mps, speed_e3 / 1000.0)
        if len(beta["overlays"]) < 1000000:
            beta["overlays"].append((row["mono_ns"], speed_e3, source))

    def check_overlay_speeds(self, beta):
        """Each overlay speed must equal (+-1 mm/s rounding) the speed of a
        WHEELS event received within the lease before the send; the band of
        those journaled wheel speeds is the tolerance. Without any journaled
        wheel event in that window only the 0..100 m/s plausibility applies."""
        wheels = sorted(beta["wheels"])
        times = [w[0] for w in wheels]
        for mono_ns, speed_e3, source in beta["overlays"]:
            begin = mono_ns - BETA_LEASE_NS - 1000000
            # Wheel rows of this lease window may be among rows the journal
            # writer dropped: then the remaining wheels prove nothing.
            if any(start <= mono_ns and end >= begin for start, end in beta["journal_gaps"]):
                self.beta_speed_overlay_wheel["unverified_journal_gap"] += 1
                self.issue("beta_overlay_unverified_journal_gap", source,
                           "Wheel rows of this overlay's lease window may have been dropped by the journal writer")
                continue
            lo = bisect.bisect_left(times, mono_ns - BETA_LEASE_NS - 1000000)
            hi = bisect.bisect_right(times, mono_ns)
            window = [w[1] for w in wheels[lo:hi]]
            if not window:
                self.beta_speed_overlay_wheel["unverified_no_wheel_row"] += 1
                continue
            self.beta_speed_overlay_wheel["checked"] += 1
            error = min(abs(speed_e3 - w) for w in window)
            add_difference(self.beta_speed_overlay_error_mps, error / 1000.0)
            if error > 1:
                self.issue("beta_overlay_speed_mismatch", source,
                           "Overlay speed %.3f m/s differs from every wheel speed received in the "
                           "0.5 s lease (%.3f..%.3f m/s)" % (speed_e3 / 1000.0, min(window) / 1000.0,
                                                            max(window) / 1000.0), True)

    def beta_send(self, row, original, outgoing, source):
        s = self.session
        beta = s["beta"]
        self.lag_violation(row, source, "BETA_REPLACEMENT")
        self.beta_replacements += 1
        if row["result"] != 0:
            self.beta_replaced_nonzero += 1
        if s["boot"] is not None and s["boot"].get("mode") != 5:
            self.issue("beta_without_beta_config", source, "BETA_REPLACEMENT requires boot configuration BETA=5", True)
        if row["mode"] != 0:
            self.issue("beta_wrong_mode", source, "BETA_REPLACEMENT requires original mode0", True)
        elif "class" in row and row["class"] != POSITION_CLASS_LOST:
            self.issue("beta_wrong_mode", source,
                       "BETA_REPLACEMENT on adapter class %r, only LOST may be replaced" % row["class"], True)
        if row["reason"] != 0:
            self.issue("beta_nonpass_reason", source, "BETA_REPLACEMENT must have reason PASS", True)
        if not self.beta_live_ok(beta, row["mono_ns"]):
            self.issue("beta_replacement_without_engagement", source,
                       "No preceding beta_state ENGAGED/GPS_LOST covers this replacement", True)
        if beta["gps_returned"]:
            self.issue("beta_replacement_after_gps_return", source,
                       "Replacement after a GPS fix (mode!=0 POSITION) without a new GPS_LOST", True)
        fields = beta_location(outgoing)
        if outgoing[0:8] != original[0:8] or outgoing[24:32] != original[24:32]:
            self.issue("beta_payload_mismatch", source, "Bytes 0..7 and 24..31 must equal the original", True)
        if fields["has_accuracy"] != 1:
            self.issue("beta_payload_mismatch", source, "hasAccuracy (byte16) must be 1", True)
        if not 0 < fields["accuracy_e3"] <= BETA_MAX_ACCURACY_E3:
            self.issue("beta_accuracy_out_of_range", source,
                       "accuracy_e3=%d outside 1..%d" % (fields["accuracy_e3"], BETA_MAX_ACCURACY_E3), True)
            return
        if not (-90 <= fields["lat"] <= 90 and -180 <= fields["lon"] <= 180):
            self.issue("beta_payload_mismatch", source, "Replaced latitude/longitude out of range", True)
            return
        add_difference(self.beta_accuracy_m, fields["accuracy_e3"] / 1000.0)
        beta["pending"] = dict(fields, mono_ns=row["mono_ns"], source=source)

    def beta_position(self, row, source):
        """Owner validation: last BETA LOCATION sent versus the first original
        GPS fix after it. GPS is a reference, not ground truth."""
        if row["mode"] == 0:
            return
        beta = self.session["beta"]
        beta["gps_returned"] = True
        if row["mode"] in (1, 2) and row.get("utc_s") == 0:
            return  # stored no-fix position (class NO_FIX): not a GPS reference
        last = beta["pending"]
        lat, lon = row.get("lat"), row.get("lon")
        if last is None or not finite_number(lat) or not finite_number(lon):
            return
        beta["pending"] = None
        gap = (row["mono_ns"] - last["mono_ns"]) / 1e9
        raw = distance_m(last["lat"], last["lon"], lat, lon)
        aligned = raw
        if last["moving"] and gap > 0:
            # Constant speed/bearing from the send time to the fix time.
            alat, alon = advance(last["lat"], last["lon"], last["bearing_deg"], last["speed_mps"] * gap)
            aligned = distance_m(alat, alon, lat, lon)
        accuracy = last["accuracy_e3"] / 1000.0
        check = dict(dr_send=last["source"], gps_position=source, gap_s=round(gap, 3),
                     distance_m=round(raw, 2), time_aligned_distance_m=round(aligned, 2),
                     reported_accuracy_m=accuracy, within_reported_accuracy=aligned <= accuracy,
                     dr_speed_mps=last["speed_mps"], gps_mode=row["mode"], gps_horizontal=row.get("horizontal"))
        self.beta_returns_total += 1
        if len(self.beta_returns) < 100:
            self.beta_returns.append(check)
        if gap < 0:
            self.issue("beta_clock_regressed", source, "GPS fix precedes the last replaced send")
        elif aligned > accuracy:
            self.issue("beta_return_exceeds_accuracy", source,
                       "First GPS fix %.1f m from the time-aligned last BETA LOCATION; reported %.1f m"
                       % (aligned, accuracy))

    def close_beta_session(self, s=None):
        s = self.session if s is None else s
        if s is None or s["beta"]["closed"]:
            return
        s["beta"]["closed"] = True
        if s["beta"]["gap_open"] is not None:
            s["beta"]["journal_gaps"].append((s["beta"]["gap_open"], float("inf")))
            s["beta"]["gap_open"] = None
        self.check_overlay_speeds(s["beta"])
        s["beta"]["overlays"], s["beta"]["wheels"] = [], []
        if s["beta"]["engaged_ns"] is not None:
            self.beta_open_periods += 1
            self.issue("beta_engaged_unfinished", s["beta"]["engaged_source"],
                       "ENGAGED period has no recorded exit in this session")

    def read_stream(self, stream, name, size, group=None):
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
            self.consume(row, source, group if group is not None else ("stream", name))

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
                    self.read_stream(stream, str(path), path.stat().st_size,
                                     ("file", *trace_group(str(path.resolve()))))
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
                    self.read_stream(stream, str(path) + "!" + member.name, member.size,
                                     ("tar", str(path.resolve()), *trace_group(member.name)))

    def report(self):
        for session in self.sessions:
            self.close_beta_session(session)
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
                    motion_late=dict(self.motion_late, scope="accepted_late_arrivals_diagnostic_only"),
                    log_profiles=dict(self.log_profiles),
                    persistent_profile=dict(self.persistent, digest_kinds=dict(self.persistent["digest_kinds"]),
                                            suppressed=dict(self.persistent["suppressed"]),
                                            raw_window_triggers=dict(self.persistent["raw_window_triggers"]),
                                            suppressed_counts="lower_bound",
                                            scope="digest_counts_not_raw_evidence"),
                    journal_dropped=dict(self.journal_dropped, scope="diagnostic_rows_only"),
                    journal_lag=dict(self.journal_lag),
                    lower_bounds=dict(LOWER_BOUNDS),
                    motion_rejected=dict(reasons=dict(self.motion_rejected_reasons),
                                         suppressed_by_profile=self.persistent["suppressed"]["motion_rejected"],
                                         total_including_suppressed=sum(self.motion_rejected_reasons.values()) +
                                         self.persistent["suppressed"]["motion_rejected"],
                                         sensors=dict(self.motion_rejected_sensors),
                                         profile_counts="lower_bound",
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
                                        reference_links=dict(self.holdout_reference_links),
                                        reference_link_scope="same_trace_group_and_recorded_session",
                                        reference_exclusion="not_provable_from_journal",
                                        comparison_scope="recorded_compared_events_including_later_aborted_windows",
                                        scope="model_to_gps_differences_not_physical_accuracy"),
                    beta=dict(boots=self.beta_boots, states=dict(self.beta_states),
                              transition_reasons=dict(self.beta_transition_reasons),
                              engaged_periods=self.beta_engaged_periods,
                              engaged_seconds=dict(self.beta_engaged_seconds),
                              engaged_exit_reasons=dict(self.beta_exit_reasons),
                              withdraw_reasons=dict(self.beta_withdraw_reasons),
                              open_engaged_periods=self.beta_open_periods,
                              replaced_sends=self.beta_replacements,
                              replaced_nonzero_results=self.beta_replaced_nonzero,
                              replaced_accuracy_m=dict(self.beta_accuracy_m),
                              hold_events=dict(self.beta_hold_events),
                              session_storage_changes=self.beta_storage_changes,
                              position_classes=dict(self.beta_position_classes),
                              state_seconds={k: round(v, 3) for k, v in self.beta_state_seconds.items()},
                              no_fix_seconds=dict(self.beta_no_fix_seconds),
                              speed_engaged_periods=self.beta_speed_engaged_periods,
                              speed_engaged_seconds=dict(self.beta_speed_engaged_seconds),
                              speed_overlay_sends=self.beta_speed_overlays,
                              speed_overlay_nonzero_results=self.beta_speed_overlay_nonzero,
                              speed_overlay_mps=dict(self.beta_speed_overlay_mps),
                              speed_overlay_wheel_checks=dict(self.beta_speed_overlay_wheel),
                              speed_overlay_error_vs_wheel_mps=dict(self.beta_speed_overlay_error_mps),
                              anchor_gates=dict(self.beta_anchor_gates),
                              anchor_rows_dropped=self.beta_anchor_dropped,
                              reverse_latch_events=dict(self.beta_reverse_latch),
                              last_state=self.beta_last_state, last_summary=self.beta_last_summary,
                              gps_return_checks=self.beta_returns,
                              gps_return_checks_total=self.beta_returns_total,
                              gps_return_scope="last_replaced_location_vs_first_original_fix_after_it;"
                                               "time_aligned_by_constant_speed_and_bearing",
                              gps_is_ground_truth=False, phone_acceptance="not_established",
                              assist_ready=False, domain="beta"),
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
                                             wire_headers=dict(self.request_wire_headers),
                                             wire_complete_error_names=dict(self.request_wire_errors),
                                             endpoint_identity=dict(self.request_endpoints),
                                             lds_sideband_matching="exact_wire_key_diagnostic_only",
                                             qualification="not_established"),
                    lds_sideband=self.lds.report(),
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
        beta = report["beta"]
        if beta["boots"] or beta["states"] or beta["replaced_sends"] or beta["speed_overlay_sends"]:
            seconds = beta["engaged_seconds"]
            print("BETA: engaged %d times (seconds min/mean/max: %s/%s/%s), replaced %d sends "
                  "(non-zero results %d), hold %d, last state %s" %
                  (beta["engaged_periods"], seconds["min"], seconds["mean"], seconds["max"],
                   beta["replaced_sends"], beta["replaced_nonzero_results"],
                   beta["hold_events"].get("hold_set", 0),
                   "%s (%s)" % (beta["last_state"]["state"], beta["last_state"]["reason"])
                   if beta["last_state"] else "none"))
            print("BETA exits from ENGAGED: %s; withdrawals: %s; reported accuracy (m): %s" %
                  (beta["engaged_exit_reasons"], beta["withdraw_reasons"], beta["replaced_accuracy_m"]))
            if beta["position_classes"]:
                print("BETA NO_FIX: %d state rows; position classes: %s" %
                      (beta["position_classes"].get("NO_FIX", 0), beta["position_classes"]))
            if beta["speed_overlay_sends"] or beta["speed_engaged_periods"]:
                nofix = beta["no_fix_seconds"]
                print("BETA speed overlay: %d sends (non-zero results %d), SPEED_ENGAGED %d times "
                      "(%.1f s), NO_FIX periods %d (max %s s); wheel checks: %s" %
                      (beta["speed_overlay_sends"], beta["speed_overlay_nonzero_results"],
                       beta["speed_engaged_periods"], beta["state_seconds"].get("SPEED_ENGAGED", 0.0),
                       nofix["count"], nofix["max"], beta["speed_overlay_wheel_checks"]))
            for check in beta["gps_return_checks"][:20]:
                print("BETA GPS return: last DR vs first GPS fix %.1f m (time-aligned %.1f m, gap %.1f s), "
                      "reported accuracy %.1f m -> %s" %
                      (check["distance_m"], check["time_aligned_distance_m"], check["gap_s"],
                       check["reported_accuracy_m"],
                       "within" if check["within_reported_accuracy"] else "EXCEEDS"))
            if not beta["gps_return_checks_total"]:
                print("BETA GPS return: no original GPS fix after a replaced LOCATION in these logs")
        if report['motion_rejected']['reasons'] or report['motion_rejected']['suppressed_by_profile']:
            print("Rejected sensor diagnostics (not accepted input): %s; at least %d more counted only "
                  "in persistent-profile digests (total at least %d; digests can be lost)" %
                  (report['motion_rejected']['reasons'], report['motion_rejected']['suppressed_by_profile'],
                   report['motion_rejected']['total_including_suppressed']))
        if report["journal_lag"].get("not_durable"):
            print("Boot journal flush timeouts (journal_not_durable): at least %d (lower bound; the row "
                  "itself is diagnostic class)" % report["journal_lag"]["not_durable"])
        if report["persistent_profile"]["suppressed"]:
            print("Persistent-profile suppressed rows (lower bound): %s" % report["persistent_profile"]["suppressed"])
        for issue in report["issues"][:10]:
            print("%s %s: %s" % (issue["code"], issue["source"], issue["detail"]))
        print("Phone acceptance and DR accuracy: not established. Polling does not prove source provenance.")
    return {"local_checks_pass": 0, "violation": 1, "inconclusive": 2}[report["status"]]


if __name__ == "__main__":
    sys.exit(main())
