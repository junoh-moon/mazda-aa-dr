#!/usr/bin/env python3
"""Replay a raw CAN CSV (time,addr,bus,data) into an exported-trip directory for tools/pimu_eval.py.

RECORD-ONLY offline tool (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, section 15). It
exercises the stage 2 evaluation end to end on REAL CAN 0x079 bytes, normally the public ND log
(sha256 18983a366b46..., outside the repository; it is read only and never copied):

  * phone side (as the app would): every 0x079 frame is decoded with the documented layout
    (phone/imu-sender/tests/make_obd_vectors.py `decode`), integrated like ObdIntegrator
    (-(raw12 - 2047) * 658.615 urad/s * 20 ms, rounding carried, reset after a restart), flagged
    like ObdLossMeter/ObdLink (bit 4 always; bit 5 after a counter step > 1 or a receipt gap
    > 160 ms; bit 3 on the first frame and after a receipt gap > 0.5 s, where the app's selector
    would fall back to the gyro and the monitor would restart) and encoded as PHONE IMU CHANNEL v1
    DATA frames with the canonical golden encoder (tests/phone_imu/make_golden.py), CRC included,
    then decoded again;
  * CMU side: the frames are binned exactly like the collector's Aggregator (1 s bins of CMU
    receipt time from the first frame, ten bins per pimu row) and written to logs/pimu.0.jsonl;
    the car's 0x078 yaw (the firmware layout of the VIP map) becomes log_digest rows with ten 1 s
    moving sums and a stationary sum on the CMU's 2048-based scale, and the stops (all four 0x215
    wheel speeds exactly zero for >= 1 s, merged when < 5 s apart) become yaw_stop rows with the
    0x078 mean of the stop (first 0.2 s and the time after the last zero wheel frame left out),
    in logs/trace.0.jsonl. 0x202 gives the digest's mean speed.

What this does NOT model: the adapter (no real ELM327, no BUFFER FULL, no clone throughput), the
Bluetooth hops (one constant delay, no batching), the selector's gyro windows (there is no phone),
the VIP's own 0x116 windows (0x078 frames stand in for them) and GPS (the public log has none, so no
heading-error window can be formed). The logger's own gaps act as if they were adapter stalls.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tests" / "phone_imu"))
sys.path.insert(0, str(REPO / "phone" / "imu-sender" / "tests"))
import make_golden          # noqa: E402  canonical v1 encoder/decoder
import make_obd_vectors     # noqa: E402  independent 0x079 decoder, 0x078 layout, public log hash

PUBLIC_SHA256 = make_obd_vectors.LOG_SHA256
BOOT_ID = "0b0d0079-0000-4000-8000-0000000c0a01"
CMU_OFFSET_S = 1000.0      # logger time -> CMU CLOCK_MONOTONIC
BT_DELAY_S = 0.080         # phone receipt -> CMU receipt (constant)
GAP_FLAG_S = 0.160         # ObdLossMeter: more than 8 periods is an ambiguous gap
RESET_GAP_S = 0.5          # SourceSelector STALE: the app would switch to the gyro, then back with frame_reset
STOP_MIN_S, STOP_MERGE_S, STOP_SKIP_S = 1.0, 5.0, 0.2
WHEEL_ZERO_RAW = 10000     # 0x215 raw * 0.01 - 100 km/h
FLAG_RESET, FLAG_OBD, FLAG_OBD_GAP = 8, 16, 32


def read_csv(path):
    by = {0x078: [], 0x079: [], 0x215: [], 0x202: []}
    with open(path, newline="") as handle:
        for row in csv.reader(handle):
            try:
                t, addr, data = float(row[0]), int(row[1]), bytes.fromhex(row[3])
            except (ValueError, IndexError):
                continue
            if addr in by:
                by[addr].append((t, data))
    for v in by.values():
        v.sort(key=lambda x: x[0])
    return by


def obd_frames(f079):
    """(cmu receipt ns, v1 frame bytes) of every decodable 0x079 frame, plus decode counts."""
    out, counts = [], {}
    carry, prev_t, prev_c, seq = 0.0, None, None, 0
    reset_pending = True
    for t, d in f079:
        status, raw, counter, _ = make_obd_vectors.decode(0x079, d)
        counts[status] = counts.get(status, 0) + 1
        if status != "OK":
            continue
        lost = 0
        if prev_t is not None:
            gap = t - prev_t
            if gap > RESET_GAP_S:
                reset_pending, carry = True, 0.0
            if gap > GAP_FLAG_S:
                lost = 1
            elif counter >= 0 and prev_c is not None and ((counter - prev_c) & 15) > 1:
                lost = ((counter - prev_c) & 15) - 1
        exact = -(raw - make_obd_vectors.ZERO) * make_obd_vectors.URAD_S_PER_COUNT * make_obd_vectors.PERIOD_S + carry
        rounded = int(math.floor(exact + 0.5))       # Java Math.round
        carry = exact - rounded
        flags = FLAG_OBD | (FLAG_OBD_GAP if lost else 0) | (FLAG_RESET if reset_pending else 0)
        reset_pending = False
        t_phone_ns = int(round(t * 1e9))
        frame = make_golden.encode_data(flags, seq, t_phone_ns, rounded, 20000, 0)
        out.append((int(round((t + CMU_OFFSET_S + BT_DELAY_S) * 1e9)), frame))
        seq = (seq + 1) & 0xFFFFFFFF
        prev_t, prev_c = t, counter
    return out, counts


def pimu_rows(frames):
    """The collector's Aggregator: 1 s bins of CMU receipt time from the first frame, ten per row."""
    if not frames:
        return []
    sec = 10 ** 9
    t0 = frames[0][0]
    rows, bins, row_t0 = [], {}, t0
    state = dict(s0=None, s1=None, fr=0)

    def flush(nb):
        d = []
        for i in range(nb):
            n, dth, wu, gap, fl = bins.get(i, (0, 0, 0, 0, 0))
            d.append([n, dth, (wu + 500) // 1000, gap, fl])
        rows.append(dict(kind="pimu", m=row_t0 // 10 ** 6, s0=state["s0"] if state["s0"] is not None else -1,
                         s1=state["s1"] if state["s1"] is not None else -1, b=0, pt=0, ce=0, bv=0, sk=0, dr=0,
                         fr=state["fr"], d=d))
    for now, frame in frames:
        f = make_golden.fields_of(frame)          # the CMU decodes what the phone encoded
        assert f["magic"] == "MX" and f["version"] == 1
        idx = (now - row_t0) // sec
        if idx >= 10:
            flush(10)
            row_t0 += (idx // 10) * 10 * sec      # whole empty rows are skipped
            idx -= (idx // 10) * 10
            bins.clear()
            state.update(s0=None, s1=None, fr=0)
        n, dth, wu, gap, fl = bins.get(idx, (0, 0, 0, 0, 0))
        if state["s1"] is not None and not f["flags"] & FLAG_RESET:
            step = (f["seq"] - state["s1"]) & 0xFFFFFFFF
            if 1 < step < 0x80000000:
                gap += step - 1
        if state["s0"] is None:
            state["s0"] = f["seq"]
        state["s1"] = f["seq"]
        state["fr"] += 1 if f["flags"] & FLAG_RESET else 0
        bins[idx] = (n + 1, dth + f["dtheta_urad"], wu + f["window_us"], gap, fl | f["flags"])
    flush(max(bins) + 1 if bins else 1)
    return rows


def wheel_zero(d):
    return all(int.from_bytes(d[i:i + 2], "big") == WHEEL_ZERO_RAW for i in (0, 2, 4, 6))


def stops_of(f215):
    """Standstill episodes: all four wheels exactly zero for >= 1 s, merged when < 5 s apart.
    (start, last zero wheel frame, end) in logger seconds."""
    eps, start, last = [], None, None
    for t, d in f215:
        if wheel_zero(d):
            start = t if start is None else start
            last = t
        elif start is not None:
            eps.append([start, last, t])
            start = None
    if start is not None:
        eps.append([start, last, last])
    merged = []
    for e in eps:
        if merged and e[0] - merged[-1][2] < STOP_MERGE_S:
            merged[-1][1], merged[-1][2] = e[1], e[2]
        else:
            merged.append(e)
    return [e for e in merged if e[1] - e[0] >= STOP_MIN_S]


def trace_rows(by, stops):
    f078 = [(t, make_obd_vectors.raw078(d)) for t, d in by[0x078] if len(d) >= 4]
    speeds = [(t, int.from_bytes(d[2:4], "big") * 0.01) for t, d in by[0x202] if len(d) >= 4]
    rows = [dict(kind="boot", schema=1, pid=1, mono_ns=int(CMU_OFFSET_S * 1e9), boot_id=BOOT_ID)]

    def stationary(t):
        return any(a + STOP_SKIP_S <= t <= last for a, last, _ in stops)

    def in_stop(t):
        return any(a <= t <= e for a, _, e in stops)
    t_lo = int(math.floor(min(t for t, _ in f078)))
    t_hi = int(math.ceil(max(t for t, _ in f078)))
    for since in range(t_lo, t_hi, 10):
        y1, y1n, yst, ystn = [0] * 10, [0] * 10, 0, 0
        for t, raw in f078:
            if since <= t < since + 10:
                if in_stop(t):
                    if stationary(t):
                        yst += raw - 2048
                        ystn += 1
                else:
                    j = int(t - since)
                    y1[j] += raw - 2048
                    y1n[j] += 1
        v = [s for t, s in speeds if since <= t < since + 10]
        rows.append(dict(kind="log_digest", schema=1, mono_ns=int((since + 10 + CMU_OFFSET_S) * 1e9),
                         since_ns=int((since + CMU_OFFSET_S) * 1e9), y1=y1, y1n=y1n, yst=yst, ystn=ystn, gc=[],
                         speed_kmh_mean=round(sum(v) / len(v), 1) if v else None,
                         speed_kmh_min=round(min(v), 1) if v else None))
    for a, last, e in stops:
        vals = [raw - 2048 for t, raw in f078 if a + STOP_SKIP_S <= t <= last]
        if not vals:
            continue
        m = sum(vals) / float(len(vals))
        sd = math.sqrt(sum((x - m) ** 2 for x in vals) / len(vals))
        rows.append(dict(kind="yaw_stop", schema=1, mono_ns=int((e + CMU_OFFSET_S) * 1e9), boot_s=round(a + CMU_OFFSET_S, 1),
                         dur_ms=int(round((e - a) * 1000)), ys=sum(vals), yn=len(vals), sd=int(round(sd * 100)),
                         first=None, last=None, pre=None, pre_kmh=None))
    rows.sort(key=lambda r: (r["kind"] != "boot", r["mono_ns"]))
    return rows


def replay(csv_path, out_dir, check_hash=True):
    data = Path(csv_path).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if check_hash and digest != PUBLIC_SHA256:
        raise SystemExit("not the public ND log (sha256 %s); pass --any-csv for another CSV" % digest)
    by = read_csv(csv_path)
    frames, counts = obd_frames(by[0x079])
    stops = stops_of(by[0x215])
    logs = Path(out_dir) / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    trace = trace_rows(by, stops)
    (logs / "trace.0.jsonl").write_text("".join(json.dumps(r) + "\n" for r in trace))
    pimu = [dict(kind="pimu_boot", schema=1, m=frames[0][0] // 10 ** 6 if frames else 0, boot_id=BOOT_ID, svc=15,
                 budget_bph=0, cap=0)] + pimu_rows(frames)
    (logs / "pimu.0.jsonl").write_text("".join(json.dumps(r) + "\n" for r in pimu))
    return dict(csv_sha256=digest, public_log=digest == PUBLIC_SHA256, frames_079=len(by[0x079]),
                frames_078=len(by[0x078]), decode=counts, v1_frames=len(frames),
                stops=[dict(start_cmu_s=round(a + CMU_OFFSET_S, 1), seconds=round(e - a, 1)) for a, _, e in stops],
                out=str(logs))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", help="raw CAN CSV (time,addr,bus,data); read only")
    parser.add_argument("out", help="output directory (logs/trace.0.jsonl, logs/pimu.0.jsonl)")
    parser.add_argument("--any-csv", action="store_true", help="accept a CSV other than the public ND log (tests)")
    args = parser.parse_args(argv)
    print(json.dumps(replay(args.csv, args.out, not args.any_csv), indent=1, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
