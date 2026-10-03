#!/usr/bin/env python3
"""Decode a public MX-5 ND raw CAN log with the layouts the CMU firmware reads.

Usage: python3 analyze_nd_can.py raw_can.csv   (columns: time,addr,bus,data; needs numpy)
The layouts for 0x078/0x079 come from the VIP image analysis in
validation/VIP_CAN_MAP_2026-10-03.md; the public decodes (0x075, 0x076, 0x202, 0x215, 0x086)
are used only as cross-checks. Timestamps in the public log are batched, so pairing is by nearest
time and frame counts, not exact timing.
"""
import collections
import csv
import sys

import numpy as np


def load(path):
    rows = []
    with open(path, newline="") as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            if len(row) < 4:
                continue
            try:
                rows.append((float(row[0]), int(row[1]), bytes.fromhex(row[3])))
            except ValueError:
                continue
    return rows


def near(tt, tsrc, vsrc):
    i = np.clip(np.searchsorted(tsrc, tt), 1, len(tsrc) - 1)
    j = np.where(abs(tsrc[i - 1] - tt) < abs(tsrc[i] - tt), i - 1, i)
    return vsrc[j]


def main(path):
    rows = load(path)
    by = collections.defaultdict(list)
    for t, a, d in rows:
        by[a].append((t, d))
    duration = rows[-1][0] - rows[0][0]

    def arr(i):
        return (np.array([t for t, _ in by[i]]),
                np.array([list(d) for _, d in by[i]], dtype=np.int64))

    print("rows %d, duration %.1f s" % (len(rows), duration))
    for i in (0x215, 0x202, 0x75, 0x76, 0x78, 0x79, 0x9F, 0x240, 0x86, 0x82):
        print("rate 0x%03X: %.1f Hz (frames/duration)" % (i, len(by[i]) / duration))

    t78, B = arr(0x78)  # firmware layout: Qf2 | long13 | yaw12 | brake13 | Qf2
    qf1, qf2 = B[:, 0] >> 6, B[:, 5] >> 6
    long13 = ((B[:, 0] & 0x3F) << 7) | (B[:, 1] >> 1)
    yaw12 = ((B[:, 1] & 1) << 11) | (B[:, 2] << 3) | (B[:, 3] >> 5)
    t79, C = arr(0x79)  # Qf2 | lat13
    lat13 = ((C[:, 0] & 0x3F) << 7) | (C[:, 1] >> 1)
    print("Qf values 0x078:", dict(collections.Counter(qf1.tolist())),
          dict(collections.Counter(qf2.tolist())),
          "0x079:", dict(collections.Counter((C[:, 0] >> 6).tolist())))
    print("yaw12 range %d..%d, median %.0f" % (yaw12.min(), yaw12.max(), np.median(yaw12)))

    t2, S = arr(0x202)
    spd = ((S[:, 2] << 8) | S[:, 3]) * 0.01  # km/h
    t215, W = arr(0x215)
    w = [(((W[:, 2 * k] << 8) | W[:, 2 * k + 1]) * 0.01 - 100) for k in range(4)]
    t75, R = arr(0x75)
    yaw_raw = ((R[:, 4] << 8) | R[:, 5]) * 0.01 - 180  # public RCM yaw, deg/s
    lat_raw = ((R[:, 2] << 8) | R[:, 3]) * 0.001 - 2
    t76, L = arr(0x76)
    long_raw = ((L[:, 2] << 8) | L[:, 3]) * 0.001 - 2
    t86, E = arr(0x86)
    steer_right = (16000 - ((E[:, 0] << 8) | E[:, 1])) * 0.1  # public: positive = right

    sp = near(t78, t2, spd)
    mv, st_ = sp > 15, sp < 0.5
    st = near(t78, t86, steer_right)
    c = np.corrcoef(yaw12[mv], st[mv])[0, 1]
    print("corr(yaw12, steering_right) v>15: %.3f -> yaw12 increases in %s turns"
          % (c, "LEFT" if c < 0 else "RIGHT"))
    yr = near(t78, t75, yaw_raw)
    sl = np.polyfit(yr[mv], yaw12[mv].astype(float), 1)[0]
    print("yaw12 counts per deg/s of public 0x075: %.2f (corr %.4f); firmware constant 26.5"
          % (sl, np.corrcoef(yr[mv], yaw12[mv])[0, 1]))
    print("stationary yaw12 mean %.2f std %.2f counts (nominal centre 2047)"
          % (yaw12[st_].mean(), yaw12[st_].std()))

    yc = (yaw12 - 2046).astype(float)
    wn = [near(t78, t215, x) for x in w]
    for a, b, name in ((0, 1, "w1-w2"), (2, 3, "w3-w4"), (0, 2, "w1-w3"), (1, 3, "w2-w4")):
        print("corr(%s, yaw12) = %+.3f" % (name, np.corrcoef((wn[a] - wn[b])[mv], yc[mv])[0, 1]))
    print("0x202 / mean(4 wheels), v>20: median %.4f"
          % np.median(spd[spd > 20] / np.mean([near(t2, t215, x) for x in w], axis=0)[spd > 20]))

    # front/rear: the steered axle gains speed with steering angle
    t215_yaw = near(t215, t78, yaw12.astype(float))
    sp215, st215 = near(t215, t2, spd), near(t215, t86, steer_right)
    m = sp215 > 15
    d = ((w[0] + w[1]) / 2 - (w[2] + w[3]) / 2)[m]
    print("corr(avg(w1,w2)-avg(w3,w4), |steering|) = %.3f" % np.corrcoef(d, np.abs(st215[m]))[0, 1])
    for name, (lo, hi, track) in {"(w1=L,w2=R) track 1.496": (0, 1, 1.496),
                                   "(w3=L,w4=R) track 1.505": (2, 3, 1.505)}.items():
        yk = np.degrees((w[hi] - w[lo]) / 3.6 / track)
        mk = m & (np.abs(yk) < 40)
        so = ((yk[mk]) * (t215_yaw[mk] - 2046.0)).sum() / (yk[mk] ** 2).sum()
        print("yaw12 counts per kinematic deg/s, axle %s: %.2f" % (name, so))

    for name, x13, tx, gref, tg in (("lat13 vs 0x075", lat13, t79, lat_raw, t75),
                                     ("long13 vs 0x076", long13, t78, long_raw, t76)):
        g = near(tx, tg, gref)
        sl, ic = np.polyfit(g, x13.astype(float), 1)
        print("%s: corr %.3f, counts/G %.0f, count at 0 G %.0f" % (name, np.corrcoef(g, x13)[0, 1], sl, ic))
    t9, N = arr(0x9F)
    print("0x9F byte0 values:", dict(collections.Counter(N[:, 0].tolist())))


if __name__ == "__main__":
    main(sys.argv[1])
