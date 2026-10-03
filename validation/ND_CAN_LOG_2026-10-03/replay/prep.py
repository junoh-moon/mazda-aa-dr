#!/usr/bin/env python3
"""Build VIP-like 100 ms windows from a public ND raw CAN log, by FRAME COUNT.

Window k = 0x215 frames [10k,10k+10) and 0x078 frames [5k,5k+5) and 0x075 frames [10k,10k+10).
Writes windows.csv consumed by replay.cpp and analyze.py. Logger timestamps are kept only as
diagnostics (t_log); the synthetic clock is k*100 ms.
"""
import sys
import numpy as np

def main(src, out):
    t, a, d = [], [], []
    with open(src) as f:
        next(f)
        for line in f:
            p = line.rstrip().split(',')
            if len(p) < 4:
                continue
            ad = int(p[1])
            if ad not in (0x215, 0x078, 0x075, 0x202, 0x079):
                continue
            t.append(float(p[0])); a.append(ad); d.append(bytes.fromhex(p[3]))
    t = np.array(t); a = np.array(a)
    def frames(i):
        m = a == i
        return t[m], np.array([list(d[j]) for j in np.nonzero(m)[0]], dtype=np.int64)
    t215, W = frames(0x215); t78, B = frames(0x078); t75, R = frames(0x075); t2, S = frames(0x202); t79, C = frames(0x079)
    print("frames: 0x215 %d, 0x078 %d, 0x075 %d, 0x202 %d; ratio 215/78 %.4f"
          % (len(t215), len(t78), len(t75), len(t2), len(t215) / len(t78)))
    wr = np.stack([(W[:, 2*k] << 8) | W[:, 2*k+1] for k in range(4)], axis=1)  # raw u16
    yaw12 = ((B[:, 1] & 1) << 11) | (B[:, 2] << 3) | (B[:, 3] >> 5)
    long13 = ((B[:, 0] & 0x3F) << 7) | (B[:, 1] >> 1)  # diagnostic only
    lat13 = ((C[:, 0] & 0x3F) << 7) | (C[:, 1] >> 1)   # diagnostic only
    rcm = ((R[:, 4] << 8) | R[:, 5]) * 0.01 - 180.0  # deg/s, + = left
    spd = ((S[:, 2] << 8) | S[:, 3]) * 0.01
    n = min(len(t215) // 10, len(t78) // 5, len(t75) // 10, len(t2) // 10, len(t79) // 5)
    rows = []
    for k in range(n):
        ww = wr[10*k:10*k+10]
        wmean = np.floor(ww.mean(axis=0) + 0.5).astype(int)  # rounded window mean of raw u16
        ys = yaw12[5*k:5*k+5]
        ysum = int(ys.sum()) & 0xFFFF
        rows.append((k, *wmean, ysum, 5, ys.mean(), ys.std(), rcm[10*k:10*k+10].mean(),
                     spd[10*k:10*k+10].mean(), long13[5*k:5*k+5].mean(), lat13[5*k:5*k+5].mean(), t215[10*k], t78[5*k]))
    with open(out, 'w') as f:
        f.write("k,w1,w2,w3,w4,yaw_sum,yaw_count,yaw12_mean,yaw12_std,rcm_dps_left,spd202_kmh,long13,lat13,t215_log,t078_log\n")
        for r in rows:
            f.write("%d,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.5f,%.4f,%.2f,%.2f,%.6f,%.6f\n" % r)
    print("windows", n, "->", out)
    # logger-gap diagnostics: frame-count vs timestamp drift
    dt = np.diff(t215)
    big = np.nonzero(dt > 0.3)[0]
    print("0x215 timestamp gaps >0.3s at frame idx / t / gap:", [(int(i), round(t215[i], 2), round(dt[i], 2)) for i in big])
    print("log span 0x215 %.2f s, frames/99Hz = %.2f s" % (t215[-1] - t215[0], len(t215) / 99.0))
    # wheel continuity across gaps
    for i in big:
        print("  speed across gap idx %d: before %.2f after %.2f km/h, yaw12 continuity n/a"
              % (i, wr[i].mean() * 0.01 - 100, wr[i+1].mean() * 0.01 - 100))
    # cross-alignment check: timestamps of window k from 0x215 vs 0x078
    t2w = np.array([r[-2] for r in rows]); t7w = np.array([r[-1] for r in rows])
    print("window start skew t215-t078: median %.3f, max abs %.3f s" % (np.median(t2w - t7w), np.abs(t2w - t7w).max()))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
