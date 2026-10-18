#!/usr/bin/env python3
"""E3b: OBD 0x079 correction of the CMU yaw (0x078) zero -- the reference implementation.

OFFLINE RESEARCH TOOL AND SINGLE SOURCE OF TRUTH for a later C++ port
(validation/YAW_OBD_BIAS_E3B_2026-10-19.md). Nothing here is product code and nothing reads or
writes the vehicle. The module holds

  * the integer arithmetic both implementations must share (Q4 counts, round-half-up division,
    centred sums, the one floating-point correlation score with a fixed operation order);
  * the two datagram records (worker -> collector MXYW 64 bytes, collector -> worker MXOB 48 bytes);
  * Collector: the phone OBD frame intake (counter chains, leaky lower latency envelope), the
    constant-lag search and the E3b bias estimator (E3 + change-point restart + 10 s hold);
  * Worker: the 0x116 window cell aggregation and the consumption rules (age, uid, boot, sequence,
    hold/ramp, |b| cap, standstill disagreement, GPS consistency guard, rail windows untouched);
  * the simulation (app model with loss filling and dtheta carry, Bluetooth delivery, 0x116 windows,
    owner-like zero models, scenarios) and the pre-registered evaluation.

  yaw_obd_e3b.py evaluate --csv PUBLIC_ND_LOG.csv --seed 1019 --reps 100 --json out.json
  yaw_obd_e3b.py scenario NAME --seed S --reps N          (one scenario, prints stats)

Conventions (FACT, src/navigation/pipeline.cpp, beta_profile.h, OBD_YAW_CHANNEL_DESIGN 4/6):
heading rate (clockwise +) = -(counts - zero) * 0.000658615 rad/s; the BETA zero is 2048; the app
sends dtheta_urad = -(raw12 - 2047) * 13.1723 urad per 20 ms frame with the rounding carried to the
next frame.
"""
import argparse
import hashlib
import json
import math
import struct
import sys
import zlib

import numpy as np

# ------------------------------------------------------------------------------------------------
# Shared constants (the C++ port uses exactly these integers)
# ------------------------------------------------------------------------------------------------
NS = 1_000_000_000
PERIOD_NS = 20_000_000            # CAN 0x078/0x079 period
CELL_NS = 100_000_000             # lag-search cell
CELLS_PER_BIN = 10
Q = 16                            # Q4: 1 count = 16
URAD_X1E4_PER_COUNT = 131723      # 13.1723 urad per count per frame
SENTINEL_URAD = 26900             # |dtheta| >= this: raw12 within 5 of 0 or 4095 (sentinel/rail)
RAD_PER_COUNT = 0.000658615
DEG_PER_COUNT = RAD_PER_COUNT * 180.0 / math.pi
PUBLIC_SHA256 = '18983a366b46d8bea62ea426652798ca24cfef23f932e3751442485938aab54f'

FLAG_FRAME_RESET = 0x08
FLAG_SOURCE_OBD = 0x10
FLAG_OBD_GAP = 0x20
FLAG_OBD_FILLED = 0x40            # NEW (stage 3 app): this frame was re-created by the app's fill


class P:
    """E3b constants (pre-registration 2026-10-19, section 'E3b')."""
    LEAK_NS = 4_000               # envelope may rise 4 us per frame (200 ppm clock drift)
    CHAIN_SLOTS = 64
    FRAME_RING = 6400             # 128 s at 50 Hz
    LAG_STEP_NS = 20_000_000
    LAG_STEPS = 76                # 0.00 .. 1.50 s
    LAG_SPAN_BINS = 120
    LAG_EVERY_BINS = 10
    LAG_MIN_PAIRS = 200
    LAG_MIN_SD_Q = 32             # 2 counts per 0.1 s cell, first difference sd, both signals
    LAG_MIN_CORR = 0.80           # a lag whose best score is below this, or at the search edge, is unusable
    DELAY_BINS = 3                # bin k is processed at (k + 1 + 3) s
    N_MIN = 45                    # frames / samples per aligned bin
    Z79_MIN = 5
    Z79_KEEP = 128
    STILL_FAULT_Q = 16            # 1 count
    BLOCK_S = 60
    W = 20                        # bins
    MIN_USABLE = 10
    MIN_USABLE_CP = 5
    STILL_WEIGHT = 3
    TRIM_NUM, TRIM_DEN = 1, 10
    SD_MAX_Q = 48                 # 3 counts
    B_MAX_Q = 256                 # 16 counts
    CP_M = 5                      # change point: the last 5 usable bins ...
    CP_SPAN = 8                   # ... all inside the last 8 bins ...
    CP_SD_Q = 32                  # ... with sd <= 2 counts ...
    CP_DIFF_Q = 32                # ... whose median differs from the window estimate by > 2 counts
    SCALE_BINS = 60
    SCALE_ROT_MIN_Q = 2400 * 16   # sum |o - zoff| over the scale window, counts*s*16
    SCALE_LO_NUM, SCALE_HI_NUM, SCALE_DEN = 49, 51, 50   # 0.98 .. 1.02 (E3: 0.95 .. 1.05)
    HOLD_BINS = 10                # collector: HOLDING (last valid b) for at most 10 s
    DELATTICE = True              # 0x079 values mapped to the linear lattice value (zero without
                                  # the lattice's integer-rounding bias at standstill)
    EDGE_Q = 848                  # rotation-edge guard: |o_k - o_k+-1| <= 53 counts (2 deg/s) unless
                                  # still; None = off
    USABLE_KEEP = 240


class WP:
    """Worker consumption constants."""
    AGE_MAX_NS = 10 * NS          # record bin start must be within [now - 10 s, now]
    STALE_NS = 15 * NS            # no accepted record for 15 s: start the ramp
    RAMP_NS = 15 * NS
    STILL_RUN_MIN = 8             # standstill disagreement: >= 8 consecutive still bins ...
    STILL_CMP = 5                 # ... compare b with the median offset of the last 5
    STILL_DIS_Q = 32              # 2 counts
    DISABLE_NS = 60 * NS
    GPS_WIN_S = 40
    GPS_EVERY_S = 10
    GPS_DIS_Q = 64                # 4 counts
    GPS_HITS = 2


# ------------------------------------------------------------------------------------------------
# Integer helpers (C++: floor division must be implemented explicitly for negative numerators)
# ------------------------------------------------------------------------------------------------
def rdiv(a, b):
    """round(a / b), halves up (towards +inf), b > 0, exact integers."""
    return (2 * a + b) // (2 * b)


def q_from_urad(dtheta_urad):
    """Q4 counts of one OBD frame: round(-16 * dtheta / 13.1723)."""
    return rdiv(-16 * 10000 * int(dtheta_urad), URAD_X1E4_PER_COUNT)


def centred(xs):
    """n * sum(x^2) - (sum x)^2 for a list of ints (n^2 * population variance)."""
    n = len(xs)
    s = sum(xs)
    return n * sum(x * x for x in xs) - s * s


def median_int(xs):
    v = sorted(xs)
    n = len(v)
    if n % 2:
        return v[n // 2]
    return (v[n // 2 - 1] + v[n // 2]) // 2


def trimmed_mean_q(vals):
    v = sorted(vals)
    cut = (len(v) * P.TRIM_NUM) // P.TRIM_DEN
    mid = v[cut:len(v) - cut] if len(v) - 2 * cut > 0 else v
    return rdiv(sum(mid), len(mid))


def lattice_raw(n):
    """Integer lattice of the decoded 12-bit yaw: floor(2047.00190 + 1.52751423 n + 0.5), exact."""
    return (204700190000 + 152751423 * n + 50000000) // 100000000


def build_lattice_tables():
    """INV[raw] = lattice index n (or None, off-lattice); LIN_Q[n] = Q4 linear counts rel. 2047."""
    inv = [None] * 4096
    lin = {}
    for n in range(-1400, 1401):
        r = lattice_raw(n)
        if 0 <= r <= 4095:
            inv[r] = n
            lin[n] = rdiv(16 * (190000 + 152751423 * n), 100000000)
    return inv, lin


LAT_INV, LAT_LIN = build_lattice_tables()


def raw_from_urad(dtheta_urad):
    """The app's raw12 back from one carried dtheta (|carry| <= 0.5 urad, exact)."""
    return 2047 - rdiv(10000 * int(dtheta_urad), URAD_X1E4_PER_COUNT)


def corr_score(sab, saa, sbb):
    """The one floating-point step: IEEE double, this exact order (C++: same expression)."""
    return float(sab) / math.sqrt(float(saa) * float(sbb))


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


# ------------------------------------------------------------------------------------------------
# Records
# ------------------------------------------------------------------------------------------------
Y78_FMT = '<4sBBHI8sQ10h10BHI'        # 64 bytes
OB_FMT = '<4sBBHI8sQhhHBBhHHHI'      # 48 bytes
assert struct.calcsize(Y78_FMT) == 64 and struct.calcsize(OB_FMT) == 48

Y78_STILL, Y78_RAIL, Y78_NO_WHEELS, Y78_CELL_SAT = 1, 2, 4, 8
ST_INVALID, ST_VALID, ST_HOLDING = 0, 1, 2
H_Z79, H_LAG, H_SCALE_BLOCK, H_STILL_BLOCK, H_SD, H_FEW, H_BMAX, H_CP, H_NO_Y78, H_CHAIN_BREAK, H_OLD_APP, H_LAG_BAD = (
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048)


def encode_y78(seq, boot8, bin_k, flags, sums, counts, nwin):
    body = struct.pack(Y78_FMT[:-1], b'MXYW', 1, flags, nwin & 0xFFFF, seq & 0xFFFFFFFF, boot8,
                       bin_k * NS, *[int(s) for s in sums], *[int(c) for c in counts], 0)
    return body + struct.pack('<I', crc32(body))


def decode_y78(b):
    if len(b) != 64:
        return None
    f = struct.unpack(Y78_FMT, b)
    if f[0] != b'MXYW' or f[1] != 1 or crc32(b[:60]) != f[-1] or f[6] % NS:
        return None
    return dict(flags=f[2], nwin=f[3], seq=f[4], boot=f[5], bin=f[6] // NS,
                sums=list(f[7:17]), counts=list(f[17:27]))


def encode_ob(seq, boot8, bin_k, status, health, b_q, zoff_q, lag_ms, n_usable, n_still, scale_q12,
              cp_count, frames):
    body = struct.pack(OB_FMT[:-1], b'MXOB', 2, status, health & 0xFFFF, seq & 0xFFFFFFFF, boot8,
                       bin_k * NS, b_q, zoff_q, lag_ms, min(n_usable, 255), min(n_still, 255), scale_q12,
                       min(cp_count, 65535), min(frames, 65535), 0)
    return body + struct.pack('<I', crc32(body))


def decode_ob(b):
    if len(b) != 48:
        return None
    f = struct.unpack(OB_FMT, b)
    if f[0] != b'MXOB' or f[1] != 2 or crc32(b[:44]) != f[-1] or f[6] % NS or f[2] > 2:
        return None
    return dict(status=f[2], health=f[3], seq=f[4], boot=f[5], bin=f[6] // NS, b_q=f[7], zoff_q=f[8],
                lag_ms=f[9], n_usable=f[10], n_still=f[11], scale_q12=f[12], cp_count=f[13], frames=f[14])


# ------------------------------------------------------------------------------------------------
# Collector (uid service): OBD frames + MXYW records in, one MXOB record per processed bin out
# ------------------------------------------------------------------------------------------------
class Collector:
    def __init__(self, boot8, p=P):
        self.p = p
        self.boot8 = boot8
        # frame intake
        self.chain = -1                   # current chain id, -1: none (next frame starts one)
        self.next_chain = 0
        self.idx = 0
        self.last_seq = None
        self.env = {}                     # chain id -> envelope (ns)
        self.fr_chain = np.zeros(p.FRAME_RING, np.int64)
        self.fr_idx = np.zeros(p.FRAME_RING, np.int64)
        self.fr_q = np.zeros(p.FRAME_RING, np.int64)
        self.fr_first = np.zeros(p.FRAME_RING, bool)
        self.fr_n = 0                     # frames ever stored
        self.chain_breaks = 0
        self.off_lattice = 0
        self.relearns = 0
        self.old_app = False
        # 0x078 bins from the worker
        self.y78 = {}
        self.y78_last_seq = None
        # estimator
        self.lag_steps = None             # None: never estimated; then int steps of 20 ms
        self.lag_bad = False
        self.z79_vals = []
        self.zoff = None
        self.usable = []                  # (k, r_q, still, a_q, oc_q)
        self.cp_start = None
        self.cp_count = 0
        self.block_until = -1
        self.health_block = 0
        self.last_valid_b = None
        self.last_valid_k = None
        self.scale_q12 = 0x7FFF
        self.seq = 0
        self.next_k = None

    # ---- frame intake ----------------------------------------------------------------------------
    def on_frame(self, t_rx_ns, seq, flags, dtheta_urad):
        p = self.p
        if not flags & FLAG_SOURCE_OBD:
            self.chain = -1               # a phone-gyro frame between OBD frames breaks the chain
            self.last_seq = seq
            return
        if flags & FLAG_OBD_GAP and not flags & FLAG_OBD_FILLED:
            self.old_app = True           # an unfilled loss: not a stage 3 app; no correction this boot
        brk = (self.chain < 0 or flags & FLAG_FRAME_RESET or self.last_seq is None
               or ((seq - self.last_seq) & 0xFFFFFFFF) != 1
               or (flags & FLAG_OBD_GAP and not flags & FLAG_OBD_FILLED))
        self.last_seq = seq
        if abs(int(dtheta_urad)) >= SENTINEL_URAD:
            self.chain = -1               # sentinel / rail value: drop it, next frame starts a chain
            self.chain_breaks += 1
            return
        if brk:
            self.chain = self.next_chain
            self.next_chain += 1
            self.idx = 0
            self.env[self.chain] = int(t_rx_ns)
            if len(self.env) > p.CHAIN_SLOTS:
                del self.env[min(self.env)]
            self.chain_breaks += 1
        else:
            self.idx += 1
            u = int(t_rx_ns) - PERIOD_NS * self.idx
            self.env[self.chain] = min(u, self.env[self.chain] + p.LEAK_NS)
        j = self.fr_n % p.FRAME_RING
        self.fr_chain[j] = self.chain
        self.fr_idx[j] = self.idx
        if p.DELATTICE:
            raw = raw_from_urad(dtheta_urad)
            n = LAT_INV[raw] if 0 <= raw <= 4095 else None
            if n is None:
                self.off_lattice += 1
                self.fr_q[j] = 0
                self.fr_first[j] = True          # an off-lattice value makes its bin unusable
            else:
                self.fr_q[j] = LAT_LIN[n]
                self.fr_first[j] = brk
        else:
            self.fr_q[j] = q_from_urad(dtheta_urad)
            self.fr_first[j] = brk
        self.fr_n += 1

    def _frames(self):
        """Stored frames with a known chain envelope: (t_hat ns, q, first, chain)."""
        n = min(self.fr_n, self.p.FRAME_RING)
        if n == 0:
            return None
        ch = self.fr_chain[:n]
        keys = np.array(sorted(self.env), np.int64)
        envs = np.array([self.env[k] for k in keys], np.int64)
        pos = np.searchsorted(keys, ch)
        pos = np.clip(pos, 0, keys.size - 1)
        ok = keys[pos] == ch
        th = envs[pos] + PERIOD_NS * self.fr_idx[:n]
        return th[ok], self.fr_q[:n][ok], self.fr_first[:n][ok], ch[ok]

    # ---- worker records --------------------------------------------------------------------------
    def on_y78(self, rec_bytes):
        r = decode_y78(rec_bytes)
        if r is None or r['boot'] != self.boot8:
            return False
        if self.y78_last_seq is not None:
            d = (r['seq'] - self.y78_last_seq) & 0xFFFFFFFF
            if d == 0 or d >= 0x80000000:      # repeated or older (modulo 2^32)
                return False
        self.y78_last_seq = r['seq']
        self.y78[r['bin']] = r
        for k in [x for x in self.y78 if x < r['bin'] - 130]:
            del self.y78[k]
        return True

    # ---- lag -------------------------------------------------------------------------------------
    def _lag(self, k, fr):
        p = self.p
        lo_bin = k - p.LAG_SPAN_BINS + 1
        ncell = p.LAG_SPAN_BINS * CELLS_PER_BIN
        m78 = np.zeros(ncell, np.int64)
        ok78 = np.zeros(ncell, bool)
        for b in range(lo_bin, k + 1):
            r = self.y78.get(b)
            if r is None or r['flags'] & (Y78_RAIL | Y78_CELL_SAT):
                continue
            for c in range(CELLS_PER_BIN):
                if r['counts'][c]:
                    m78[(b - lo_bin) * CELLS_PER_BIN + c] = rdiv(Q * r['sums'][c], r['counts'][c])
                    ok78[(b - lo_bin) * CELLS_PER_BIN + c] = True
        d78 = m78[1:] - m78[:-1]
        g78 = ok78[1:] & ok78[:-1]
        if fr is None:
            return None
        th, q = fr[0], fr[1]
        lo_ns = lo_bin * NS
        L = p.LAG_STEPS
        c = (th[None, :] - (np.arange(L, dtype=np.int64) * p.LAG_STEP_NS)[:, None] - lo_ns) // CELL_NS
        m = (c >= 0) & (c < ncell)
        flat = (np.arange(L, dtype=np.int64)[:, None] * ncell + c)[m]
        qq = np.broadcast_to(q[None, :], c.shape)[m]
        s = np.rint(np.bincount(flat, weights=qq, minlength=L * ncell)).astype(np.int64).reshape(L, ncell)
        n = np.bincount(flat, minlength=L * ncell).astype(np.int64).reshape(L, ncell)
        ok79 = n > 0
        nn = np.maximum(n, 1)
        m79 = np.where(ok79, (2 * s + nn) // (2 * nn), 0)
        d79 = m79[:, 1:] - m79[:, :-1]
        g = g78[None, :] & ok79[:, 1:] & ok79[:, :-1]
        best, best_c = None, None
        for li in range(L):
            gi = g[li]
            npair = int(gi.sum())
            if npair < p.LAG_MIN_PAIRS:
                continue
            a = d78[gi]
            bb = d79[li][gi]
            sa, sb = int(a.sum()), int(bb.sum())
            saa = npair * int((a * a).sum()) - sa * sa
            sbb = npair * int((bb * bb).sum()) - sb * sb
            lim = npair * npair * p.LAG_MIN_SD_Q * p.LAG_MIN_SD_Q
            if saa < lim or sbb < lim:
                continue
            sab = npair * int((a * bb).sum()) - sa * sb
            sc = corr_score(sab, saa, sbb)
            if best_c is None or sc > best_c:
                best_c, best = sc, li
        if best is None:
            return None
        return best, best_c

    # ---- one processing tick: bin k at (k + 1 + DELAY) s -----------------------------------------
    def process(self, k):
        p = self.p
        fr = self._frames()
        if k % p.LAG_EVERY_BINS == 0:
            est = self._lag(k, fr)
            if est is not None:
                if est[1] >= p.LAG_MIN_CORR and est[0] < p.LAG_STEPS - 1:
                    self.lag_steps = est[0]
                    self.lag_bad = False
                else:
                    self.lag_bad = True       # the lag left the searchable range or the match is poor
        lag_ns = (self.lag_steps or 0) * p.LAG_STEP_NS
        health = 0
        if self.lag_steps is not None:
            health |= H_LAG
        # OBD side of bin k
        o_q, n79, bad = None, 0, False
        if fr is not None:
            kb = (fr[0] - lag_ns) // NS
            sel = kb == k
            n79 = int(sel.sum())
            if n79:
                o_q = rdiv(int(fr[1][sel].sum()), n79)
                bad = bool(fr[2][sel].any()) or len(set(fr[3][sel].tolist())) > 1
        edge_ok = True
        if p.EDGE_Q is not None and o_q is not None:
            for kk in (k - 1, k + 1):
                sk = kb == kk
                nk = int(sk.sum())
                if nk < 25 or abs(rdiv(int(fr[1][sk].sum()), nk) - o_q) > p.EDGE_Q:
                    edge_ok = False
        if bad:
            health |= H_CHAIN_BREAK
        if self.old_app:
            health |= H_OLD_APP
        # 0x078 side
        y = self.y78.get(k)
        a_q, n78 = None, 0
        if y is None:
            health |= H_NO_Y78
        else:
            n78 = sum(y['counts'])
            if n78 and not y['flags'] & (Y78_RAIL | Y78_CELL_SAT):
                a_q = rdiv(Q * sum(y['sums']), n78)
        # still: 0x078 bins k-2 .. k+1 all still, so every OBD frame of bin k (lag <= 1.5 s) was
        # produced while the car stood still, whatever the lag estimate is
        still3 = all(self.y78.get(j) is not None and self.y78[j]['flags'] & Y78_STILL
                     and not self.y78[j]['flags'] & Y78_NO_WHEELS for j in (k - 2, k - 1, k, k + 1))
        usable = (a_q is not None and o_q is not None and n78 >= p.N_MIN and n79 >= p.N_MIN and not bad
                  and (edge_ok or still3))
        if usable:
            if still3:
                if self.zoff is not None and abs(o_q - self.zoff) > p.STILL_FAULT_Q:
                    # the 0x079 zero moved: block 60 s and learn the zero again from fresh stops
                    self.block_until = max(self.block_until, k + p.BLOCK_S)
                    self.health_block |= H_STILL_BLOCK
                    self.z79_vals = []
                    self.zoff = None
                    self.relearns += 1
                self.z79_vals.append(o_q)
                self.z79_vals = self.z79_vals[-p.Z79_KEEP:]
                if len(self.z79_vals) >= p.Z79_MIN:
                    self.zoff = median_int(self.z79_vals)
            self.usable.append([k, None, still3, a_q, o_q])
            self.usable = self.usable[-p.USABLE_KEEP:]
        reason_ok = True
        b = None
        n_win = 0
        if self.lag_bad:
            health |= H_LAG_BAD
        if self.zoff is None or self.old_app or self.lag_bad:
            reason_ok = False
            if self.zoff is not None:
                health |= H_Z79
        else:
            health |= H_Z79
            zoff = self.zoff
            for u in self.usable:
                u[1] = u[3] - (u[4] - zoff)
            lo = k - p.W
            win = [u for u in self.usable if u[0] > lo and (self.cp_start is None or u[0] >= self.cp_start)]
            b_long = None
            if win:
                vals = []
                for u in win:
                    vals += [u[1]] * (p.STILL_WEIGHT if u[2] else 1)
                b_long = trimmed_mean_q(vals)
            # change point
            rec = self.usable[-p.CP_M:]
            if (b_long is not None and len(rec) == p.CP_M and rec[0][0] > k - p.CP_SPAN
                    and (self.cp_start is None or rec[0][0] > self.cp_start)):
                rv = [u[1] for u in rec]
                if (centred(rv) <= p.CP_M * p.CP_M * p.CP_SD_Q * p.CP_SD_Q
                        and abs(median_int(rv) - b_long) > p.CP_DIFF_Q):
                    self.cp_start = rec[0][0]
                    self.cp_count += 1
                    health |= H_CP
                    win = [u for u in self.usable if u[0] >= self.cp_start and u[0] > lo]
            n_win = len(win)
            min_n = p.MIN_USABLE_CP if (self.cp_start is not None and win and win[0][0] >= self.cp_start
                                        and self.cp_start > k - p.W) else p.MIN_USABLE
            if n_win < min_n:
                reason_ok = False
                health |= H_FEW
            else:
                vals = []
                for u in win:
                    vals += [u[1]] * (p.STILL_WEIGHT if u[2] else 1)
                b = trimmed_mean_q(vals)
                rr = [u[1] for u in win]
                if centred(rr) > n_win * n_win * p.SD_MAX_Q * p.SD_MAX_Q:
                    reason_ok = False
                    health |= H_SD
                elif abs(b) > p.B_MAX_Q:
                    reason_ok = False
                    health |= H_BMAX
            # scale gate (independent of the change point)
            sw = [u for u in self.usable if u[0] > k - p.SCALE_BINS]
            if len(sw) >= p.MIN_USABLE and self.lag_steps is not None and not self.lag_bad:
                oc = [u[4] - zoff for u in sw]
                aa = [u[3] for u in sw]
                if sum(abs(x) for x in oc) >= p.SCALE_ROT_MIN_Q:
                    n = len(sw)
                    so, sa = sum(oc), sum(aa)
                    var = n * sum(x * x for x in oc) - so * so
                    cov = n * sum(x * y for x, y in zip(oc, aa)) - so * sa
                    if var > 0:
                        self.scale_q12 = max(-32767, min(32767, rdiv(4096 * cov, var) - 4096))
                        if not (p.SCALE_LO_NUM * var <= p.SCALE_DEN * cov <= p.SCALE_HI_NUM * var):
                            self.block_until = max(self.block_until, k + p.BLOCK_S)
                            self.health_block |= H_SCALE_BLOCK
        if k < self.block_until:
            reason_ok = False
            health |= self.health_block
        else:
            self.health_block = 0
        if reason_ok and b is not None:
            status, b_out = ST_VALID, b
            self.last_valid_b, self.last_valid_k = b, k
        elif self.last_valid_k is not None and k - self.last_valid_k <= p.HOLD_BINS:
            status, b_out = ST_HOLDING, self.last_valid_b
        else:
            status, b_out = ST_INVALID, 0
        self.seq += 1
        lag_ms = (self.lag_steps * 20) if self.lag_steps is not None else 0xFFFF
        rec = encode_ob(self.seq, self.boot8, k, status, health, b_out,
                        self.zoff if self.zoff is not None else -32768, lag_ms, n_win,
                        len(self.z79_vals), self.scale_q12, self.cp_count, n79)
        return rec


# ------------------------------------------------------------------------------------------------
# Worker (jciAAPA, uid 0): 0x116 windows -> MXYW records; MXOB records -> applied correction
# ------------------------------------------------------------------------------------------------
class Worker:
    def __init__(self, boot8, collector_uid=1001, w=WP):
        self.w = w
        self.boot8 = boot8
        self.uid = collector_uid
        self.seq_out = 0
        self.cur_bin = None
        self.sums = [0] * CELLS_PER_BIN
        self.counts = [0] * CELLS_PER_BIN
        self.flags = 0
        self.nwin = 0
        self.still_all = True
        self.saw_wheels = False
        self.bin_off = {}               # bin -> (offset_q, still) for the standstill check
        self.still_run = 0
        # consumption
        self.last_seq = None
        self.b_q = 0                    # current live value (Q4)
        self.mode = 'none'              # none | live | ramp
        self.ramp_from_q = 0
        self.ramp_t0 = 0
        self.last_accept_ns = None
        self.disabled_until = -1
        self.boot_disabled = False
        self.drops = {}
        self.gps_hits = 0
        self.live_valid = False
        self.events = []

    # ---- 0x116 window aggregation ----------------------------------------------------------------
    def on_window(self, t_ns, raw_sum, count, still, now_records):
        """One 0x116 yaw window (sum of raw, count) received at t_ns; still: all four wheels exactly
        zero in the newest wheel event. Returns a finished MXYW record (bytes) or None."""
        k = t_ns // NS
        out = None
        if self.cur_bin is None:
            self.cur_bin = k
        if k != self.cur_bin:
            out = self.close_bin()
            self.cur_bin = k
        rail = count == 0 or (raw_sum // count) >= 4094 or (raw_sum // count) == 0
        self.nwin += 1
        self.saw_wheels = True
        if not still:
            self.still_all = False
        if rail:
            self.flags |= Y78_RAIL
        else:
            c = (t_ns % NS) // CELL_NS
            s = self.sums[c] + int(raw_sum) - 2048 * int(count)
            n = self.counts[c] + int(count)
            if n > 255 or not -32768 <= s <= 32767:
                self.flags |= Y78_CELL_SAT
            else:
                self.sums[c], self.counts[c] = s, n
        return out

    def close_bin(self):
        k = self.cur_bin
        flags = self.flags
        if self.saw_wheels and self.still_all:
            flags |= Y78_STILL
        if not self.saw_wheels:
            flags |= Y78_NO_WHEELS
        self.seq_out += 1
        rec = encode_y78(self.seq_out, self.boot8, k, flags, self.sums, self.counts, self.nwin)
        n = sum(self.counts)
        still = bool(flags & Y78_STILL) and not flags & (Y78_RAIL | Y78_CELL_SAT) and n > 0
        self.bin_off[k] = (rdiv(Q * sum(self.sums), n) if n else None, still)
        for old in [x for x in self.bin_off if x < k - 16]:
            del self.bin_off[old]
        self.still_run = self.still_run + 1 if still else 0
        self.sums = [0] * CELLS_PER_BIN
        self.counts = [0] * CELLS_PER_BIN
        self.flags = 0
        self.nwin = 0
        self.still_all = True
        self.saw_wheels = False
        return rec

    # ---- consumption -----------------------------------------------------------------------------
    def _drop(self, why):
        self.drops[why] = self.drops.get(why, 0) + 1
        return why

    def _start_ramp(self, t_ns):
        if self.mode == 'live':
            self.ramp_from_q = self.b_q
            self.ramp_t0 = t_ns
            self.mode = 'ramp'

    def on_ob(self, t_ns, rec_bytes, sender_uid):
        w = self.w
        if sender_uid != self.uid:
            return self._drop('uid')
        r = decode_ob(rec_bytes)
        if r is None:
            return self._drop('format')
        if r['boot'] != self.boot8:
            return self._drop('boot')
        if self.last_seq is not None:
            d = (r['seq'] - self.last_seq) & 0xFFFFFFFF
            if d == 0 or d >= 0x80000000:      # repeated or older (modulo 2^32)
                return self._drop('seq')
        bin_ns = r['bin'] * NS
        if not (t_ns - w.AGE_MAX_NS <= bin_ns <= t_ns):
            return self._drop('age')
        if abs(r['b_q']) > P.B_MAX_Q:
            return self._drop('b_max')
        self.last_seq = r['seq']
        self.last_accept_ns = t_ns
        if self.boot_disabled or t_ns < self.disabled_until:
            self._start_ramp(t_ns)
            return 'disabled'
        if r['status'] == ST_INVALID:
            self._start_ramp(t_ns)
            return 'invalid'
        # standstill disagreement (independent of the collector's own standstill gate)
        k = r['bin']
        if self.still_run >= w.STILL_RUN_MIN:
            offs = [self.bin_off.get(j) for j in range(k - w.STILL_CMP + 1, k + 1)]
            if all(o is not None and o[1] for o in offs):
                med = median_int([o[0] for o in offs])
                if abs(r['b_q'] - med) > w.STILL_DIS_Q:
                    self.disabled_until = t_ns + w.DISABLE_NS
                    self.b_q, self.mode = 0, 'none'      # parked: a step to 0 turns no heading
                    self.events.append(('still_disagree', t_ns))
                    return 'still_disagree'
        self.b_q = r['b_q']
        self.mode = 'live'
        self.live_valid = r['status'] == ST_VALID
        return 'ok'

    def gps_check(self, t_ns, b_apply_mean_q, b_gps_q):
        """GPS consistency guard: called every GPS_EVERY_S while a GPS-learned zero over the last
        GPS_WIN_S of open-sky driving is available (b_gps_q, Q4) and the correction was applied
        over the same span (b_apply_mean_q)."""
        if self.boot_disabled:
            return
        if abs(b_apply_mean_q - b_gps_q) > self.w.GPS_DIS_Q:
            self.gps_hits += 1
            if self.gps_hits >= self.w.GPS_HITS:
                self.boot_disabled = True
                self._start_ramp(t_ns)
                self.events.append(('gps_disable', t_ns))
        else:
            self.gps_hits = 0

    def b_at(self, t_ns):
        """Correction (Q4 counts, int) applied to a non-rail 0x116 window received at t_ns."""
        w = self.w
        if self.mode == 'live' and self.last_accept_ns is not None and t_ns - self.last_accept_ns > w.STALE_NS:
            self.ramp_from_q = self.b_q
            self.ramp_t0 = self.last_accept_ns + w.STALE_NS
            self.mode = 'ramp'
        if self.mode == 'live':
            return self.b_q
        if self.mode == 'ramp':
            dt = t_ns - self.ramp_t0
            if dt >= w.RAMP_NS:
                self.mode, self.b_q = 'none', 0
                return 0
            return rdiv(self.ramp_from_q * (w.RAMP_NS - max(dt, 0)), w.RAMP_NS)
        return 0


# ------------------------------------------------------------------------------------------------
# Simulation: signals, app, delivery, windows
# ------------------------------------------------------------------------------------------------
def lattice(n):
    return np.floor(2047.00190 + 1.52751423 * np.asarray(n, dtype=float) + 0.5)


DPS_TO_COUNTS = 1.0 / DEG_PER_COUNT


def smooth(x, n=25):
    return np.convolve(x, np.ones(n) / n, mode='same')


def poisson_events(t, active, rate, rng):
    """Event sample indices of a Poisson process running only while active (rate per s)."""
    dt = np.diff(t, prepend=t[0])
    clock = np.cumsum(np.where(active, dt, 0.0))
    out = []
    c = rng.exponential(1.0 / rate)
    while c < clock[-1]:
        out.append(int(np.searchsorted(clock, c)))
        c += rng.exponential(1.0 / rate)
    return out


OWNER_MODELS = {'A': (1 / 90.0, 2.86, 0.5), 'B': (1 / 45.0, 4.0, 1.0)}


def owner_bias(t, moving, rng, model='A'):
    """Owner-like 0x078 zero offset (counts). A = the 2026-10-18 pre-registered model (start
    N(-3.9,1.5), +2.6 step in the first 300 s, moving events 1/90 s to -1.3 + 0.5(prev+1.3) +
    N(0,2.86)); B = aggressive (events 1/45 s, N(0,4.0), no mean reversion); none = 0."""
    if model == 'none':
        return np.zeros(t.size)
    b = np.empty(t.size)
    level = rng.normal(-3.9, 1.5)
    t_step = t[0] + rng.uniform(0.0, 300.0)
    rate, sd, keep = OWNER_MODELS[model]
    ev = set(poisson_events(t, moving, rate, rng))
    i_step = int(np.searchsorted(t, t_step))
    cur = level
    marks = sorted(ev | {i_step})
    last = 0
    for i in marks + [t.size]:
        b[last:i] = cur
        if i >= t.size:
            break
        if i == i_step:
            cur += 2.6
        if i in ev:
            cur = -1.3 + keep * (cur + 1.3) + rng.normal(0.0, sd)
            cur = float(np.clip(cur, -14.0, 14.0))
        last = i
    return b


def staircase(t, moving, rng, rate=1.0 / 22.0):
    s = np.empty(t.size, int)
    cur = int(rng.integers(0, 4))
    ev = poisson_events(t, moving, rate, rng)
    last = 0
    for i in ev + [t.size]:
        s[last:i] = cur
        if i >= t.size:
            break
        cur = int(min(3, max(0, cur + rng.choice((-1, 1)))))
        last = i
    return s


def app_frames(slot, raw79, tr, rng, fill=True, seq0=None, sentinel_reject=True):
    """Phone app (stage 3): frames delivered (tr finite) in production order; gaps of <= 15 lost
    frames are re-created with the previous delivered raw value and the next delivered frame's
    receipt time (flags 0x60); longer gaps start a new chain (0x08). Sentinel raw values (0, 4095)
    are rejected by the app and become losses. fill=False: the current (stage 1/2) app: no fill,
    the next delivered frame carries only 0x20. Returns arrays (t_rx_ns, seq, flags, dtheta)."""
    ok = np.isfinite(tr)
    if sentinel_reject:
        ok &= (raw79 != 0) & (raw79 != 4095)
    idx = np.where(ok)[0]
    out_t, out_f, out_raw = [], [], []
    prev = None
    for j in idx:
        gap = None if prev is None else int(slot[j] - slot[prev])
        if prev is None or gap > 16:
            out_t.append(tr[j]); out_f.append(FLAG_SOURCE_OBD | FLAG_FRAME_RESET); out_raw.append(raw79[j])
        else:
            if gap > 1:
                if fill:
                    for _ in range(gap - 1):
                        out_t.append(tr[j]); out_f.append(FLAG_SOURCE_OBD | FLAG_OBD_GAP | FLAG_OBD_FILLED)
                        out_raw.append(out_raw[-1])
                    out_t.append(tr[j]); out_f.append(FLAG_SOURCE_OBD); out_raw.append(raw79[j])
                else:
                    out_t.append(tr[j]); out_f.append(FLAG_SOURCE_OBD | FLAG_OBD_GAP); out_raw.append(raw79[j])
            else:
                out_t.append(tr[j]); out_f.append(FLAG_SOURCE_OBD); out_raw.append(raw79[j])
        prev = j
    n = len(out_t)
    raw = np.array(out_raw, float)
    x = -(raw - 2047.0) * 13.1723
    dth = np.empty(n, np.int64)
    carry = 0.0
    for i in range(n):
        v = x[i] + carry
        d = int(math.floor(v + 0.5))
        dth[i] = d
        carry = v - d
    s0 = int(rng.integers(0, 2 ** 32)) if seq0 is None else seq0
    seq = (s0 + np.arange(n)) & 0xFFFFFFFF
    return np.array(out_t, float), seq, np.array(out_f, np.int64), dth


def deliver(tp, rng, L, jitter_sd, P_burst, loss, dropouts=(), shift=0.0):
    tr = tp + L + shift + np.abs(rng.normal(0.0, jitter_sd, size=tp.size))
    tr = np.ceil(tr / P_burst) * P_burst
    tr = np.maximum.accumulate(tr)
    keep = rng.random(tp.size) >= loss
    for a, b in dropouts:
        keep &= ~((tp >= a) & (tp < b))
    return np.where(keep, tr, np.nan)


def make_windows(t, raw78, still, rng, wmin=4, wmax=6, jitter=0.005, hook=0.02):
    """0x116 windows: consecutive groups of wmin..wmax samples, received hook + |N(0,jitter)| after
    the last sample. Returns (t_rx, sum, count, still, first_idx, last_idx)."""
    sizes = rng.integers(wmin, wmax + 1, size=t.size // wmin + 2)
    ends = np.cumsum(sizes)
    ends = ends[ends <= t.size]
    starts = np.r_[0, ends[:-1]]
    cs = np.r_[0, np.cumsum(raw78)]
    sums = (cs[ends] - cs[starts]).astype(np.int64)
    cnt = (ends - starts).astype(np.int64)
    cst = np.r_[0, np.cumsum(still.astype(int))]
    st = (cst[ends] - cst[starts]) == cnt
    trx = t[ends - 1] + hook + np.abs(rng.normal(0.0, jitter, size=ends.size))
    trx = np.maximum.accumulate(trx)
    return trx, sums, cnt, st, starts, ends - 1


def simulate(sc, rng, est=True):
    """Run one scenario instance through app -> collector -> worker. Returns per-window arrays and
    diagnostics. sc: dict from a scenario builder."""
    t, raw78, raw79, still = sc['t'], sc['raw78'], sc['raw79'], sc['still']
    off = 5000.0 + rng.uniform(0.0, 1.0)                 # CLOCK_MONOTONIC offset (random phase)
    boot8 = bytes(rng.integers(0, 256, size=8).astype(np.uint8))
    tr = sc['tr']
    trx_w, wsum, wcnt, wst, wfirst, wlast = make_windows(t, raw78, still, rng, *sc.get('win', (4, 6)))
    w_ns = np.rint((trx_w + off) * NS).astype(np.int64)
    if np.any(np.isfinite(tr)):
        f_t, f_seq, f_flags, f_dth = app_frames(sc['slot'], raw79, tr, rng, fill=sc.get('app_fill', True),
                                                seq0=sc.get('seq0'))
    else:
        f_t, f_seq, f_flags, f_dth = np.zeros(0), np.zeros(0, np.int64), np.zeros(0, np.int64), np.zeros(0, np.int64)
    f_ns = np.rint((f_t + off) * NS).astype(np.int64)
    col = Collector(boot8)
    wk = Worker(boot8)
    # event loop over time: windows (worker), frames (collector), y78 delivery, collector ticks, ob delivery
    b_win = np.zeros(w_ns.size)
    gps = sc.get('gps')                                  # (check fn) or None
    fi = 0
    pend_y78 = []                                        # (arrival ns, bytes)
    pend_ob = []
    first_bin = int(min(w_ns[0], f_ns[0] if f_ns.size else w_ns[0]) // NS)
    next_tick_k = first_bin
    rec_log = []
    gps_next = None
    if gps is not None:
        gps_next = int((t[0] + off + WP.GPS_WIN_S) * NS)
    applied_hist = []                                    # (t_ns, b) for the GPS guard
    for wi in range(w_ns.size):
        tw = int(w_ns[wi])
        # collector ticks due before tw
        while est and (next_tick_k + 1 + P.DELAY_BINS) * NS <= tw:
            tk = (next_tick_k + 1 + P.DELAY_BINS) * NS
            while fi < f_ns.size and f_ns[fi] <= tk:
                col.on_frame(int(f_ns[fi]), int(f_seq[fi]), int(f_flags[fi]), int(f_dth[fi]))
                fi += 1
            while pend_y78 and pend_y78[0][0] <= tk:
                col.on_y78(pend_y78.pop(0)[1])
            rec = col.process(next_tick_k)
            rec_log.append(rec)
            pend_ob.append((tk + 50_000_000, rec))
            next_tick_k += 1
        while pend_ob and pend_ob[0][0] <= tw:
            a, r = pend_ob.pop(0)
            wk.on_ob(a, r, 1001)
        if gps is not None and tw >= gps_next:
            res = gps(tw, applied_hist, off)
            if res is not None:
                wk.gps_check(tw, res[0], res[1])
            gps_next += WP.GPS_EVERY_S * NS
        y = wk.on_window(tw, int(wsum[wi]), int(wcnt[wi]), bool(wst[wi]), None)
        if y is not None:
            pend_y78.append((tw + 50_000_000, y))
        rail = (wsum[wi] // wcnt[wi]) >= 4094 or (wsum[wi] // wcnt[wi]) == 0
        b_win[wi] = 0.0 if rail else wk.b_at(tw) / Q
        applied_hist.append((tw, b_win[wi], int(wcnt[wi]), wk.mode == 'live' and wk.live_valid))
    rail = ((wsum // wcnt) >= 4094) | ((wsum // wcnt) == 0)
    return dict(w_last=wlast, wsum=wsum, wcnt=wcnt, b=b_win, rail=rail, recs=rec_log, worker=wk, col=col,
                off=off, w_ns=w_ns)


def window_heading(sim, n_samples, ht=None):
    """Per-sample heading increments (deg, clockwise +) for corrected and uncorrected arms. A rail
    window is never corrected; its own error (rail policy) is the same in both arms and is taken
    out of both (the window contributes its true heading), so the paired metric measures only the
    correction."""
    inc_u = -(sim['wsum'] - 2048.0 * sim['wcnt']) * DEG_PER_COUNT * 0.020
    if ht is not None and sim['rail'].any():
        cs = np.r_[0.0, np.cumsum(ht)]
        first = sim['w_last'] - sim['wcnt'] + 1
        true_w = cs[sim['w_last'] + 1] - cs[first]
        inc_u = np.where(sim['rail'], true_w, inc_u)
    inc_c = inc_u + sim['b'] * sim['wcnt'] * DEG_PER_COUNT * 0.020
    hu = np.zeros(n_samples)
    hc = np.zeros(n_samples)
    np.add.at(hu, sim['w_last'], inc_u)
    np.add.at(hc, sim['w_last'], inc_c)
    return hc, hu


def window_errors(t, hc, hu, ht, windows):
    c = np.r_[0.0, np.cumsum(hc)]
    u = np.r_[0.0, np.cumsum(hu)]
    g = np.r_[0.0, np.cumsum(ht)]
    r = np.r_[0.0, np.cumsum(np.abs(ht))]
    out = []
    for a, b in windows:
        i, j = np.searchsorted(t, a), np.searchsorted(t, b)
        out.append((float(a), float(b - a), float((c[j] - c[i]) - (g[j] - g[i])),
                    float((u[j] - u[i]) - (g[j] - g[i])), float(r[j] - r[i])))
    return out


# ------------------------------------------------------------------------------------------------
# Scenarios (see the pre-registration for the definitions)
# ------------------------------------------------------------------------------------------------
def sliding(t0, t1, lengths=(60, 90, 120, 180, 240, 300), step=10.0):
    out = []
    for T in lengths:
        s = t0
        while s + T <= t1 + 1e-9:
            out.append((s, s + T))
            s += step
    return out


def motion_garage(rng, pre=30.0):
    total = 150.0
    target = rng.uniform(900.0, 1100.0) * rng.choice((-1, 1))
    t = np.arange(0.0, pre + total, 0.02)
    w = np.zeros(t.size)
    still = t < pre
    stop_at = pre + rng.uniform(60.0, 90.0)
    cur = pre
    while cur < pre + total:
        d = rng.uniform(4.0, 10.0)
        w[(t >= cur) & (t < min(cur + d, pre + total))] = rng.uniform(12.0, 25.0)
        cur += d + rng.uniform(2.0, 6.0)
    stopm = (t >= stop_at) & (t < stop_at + 5.0)
    w[stopm] = 0.0
    w = smooth(w)
    w[still | stopm] = 0.0
    w *= abs(target) / max(np.abs(w).sum() * 0.02, 1e-9) * math.copysign(1.0, target)
    still = still | stopm
    sky = np.zeros(t.size, bool)
    wins = [(pre, pre + x) for x in (60.0, 90.0, 120.0, 150.0)]
    return t, w, still, sky, wins


def motion_tunnel(rng):
    t = np.arange(0.0, 230.0, 0.02)
    still = ((t >= 0.0) & (t < 10.0)) | ((t >= 60.0) & (t < 70.0))
    w = np.zeros(t.size)
    cur = 10.0
    while cur < 120.0:
        d = rng.uniform(5.0, 15.0)
        w[(t >= cur) & (t < cur + d)] = rng.uniform(-8.0, 8.0)
        cur += d + rng.uniform(2.0, 10.0)
    tun = t >= 120.0
    rot = rng.uniform(30.0, 180.0)
    wt = np.zeros(t.size)
    cur = 120.0
    while cur < 230.0:
        d = rng.uniform(8.0, 20.0)
        wt[(t >= cur) & (t < cur + d)] = rng.uniform(-1.0, 1.0)
        cur += d + rng.uniform(3.0, 12.0)
    if np.abs(wt[tun]).sum() > 0:
        wt *= rot / (np.abs(wt[tun]).sum() * 0.02)
    w = np.where(tun, wt, w)
    w = smooth(w)
    w[still] = 0.0
    sky = (~tun) & (~still)
    wins = [(120.0, 180.0), (120.0, 210.0), (120.0, 230.0)]
    return t, w, still, sky, wins


def motion_urban(rng, T=600.0, rail=False, reverse=False, start_still=(20.0, 40.0)):
    t = np.arange(0.0, T, 0.02)
    still = np.zeros(t.size, bool)
    s0 = rng.uniform(*start_still)
    still[t < s0] = True
    cur = s0
    while cur < T:
        drive = rng.uniform(30.0, 120.0)
        cur += drive
        stop = rng.uniform(5.0, 40.0)
        still[(t >= cur) & (t < cur + stop)] = True
        cur += stop
    w = np.zeros(t.size)
    cur = s0
    while cur < T:
        d = rng.uniform(2.0, 12.0)
        w[(t >= cur) & (t < cur + d)] = rng.choice((-1, 1)) * rng.uniform(2.0, 25.0)
        cur += d + rng.uniform(2.0, 20.0)
    if reverse:
        # parking manoeuvre with reverse right after the start stop: 4 alternating 4-8 s arcs at 15-30 deg/s
        cur = s0
        sign = rng.choice((-1, 1))
        for _ in range(4):
            d = rng.uniform(4.0, 8.0)
            m = (t >= cur) & (t < cur + d)
            w[m] = sign * rng.uniform(15.0, 30.0)
            still[m] = False
            sign = -sign
            cur += d + 1.0
    if rail:
        for _ in range(int(rng.integers(2, 5))):
            c = rng.uniform(s0 + 10.0, T - 10.0)
            m = (t >= c) & (t < c + rng.uniform(1.0, 2.5))
            w[m] = rng.choice((-1, 1)) * rng.uniform(82.0, 100.0)
            still[m] = False
    w = smooth(w)
    w[still] = 0.0
    sky = ~still
    wins = sliding(s0, T)
    return t, w, still, sky, wins


def build_signals(rng, t, w_dps, still, z79=None, scale79=1.0, owner='A', stairs=True, jump=None,
                  stuck=None, scale_common=1.0):
    if z79 is None:
        z79 = rng.uniform(2046.0, 2049.0)
    wc = w_dps * DPS_TO_COUNTS * scale_common          # what the sensor measures (truth stays w_dps)
    moving = ~still
    x79 = (wc * scale79 + z79 - 2047.00190) / 1.52751423
    info = dict(z79=z79)
    if jump is not None:
        # a zero change of the 0x079 sensor happens before the output quantisation (stays on the lattice)
        mv = np.where(moving & jump[1])[0]
        i = int(mv[int(rng.integers(0, mv.size))])
        size = jump[0] * rng.choice((-1, 1))
        x79[i:] += size / 1.52751423
        info.update(jump_t=float(t[i]), jump=float(size))
    n79 = np.round(x79 + rng.normal(0.0, 0.3, t.size))
    raw79 = lattice(n79)
    # 0x078: true rate + 2048 + owner-like zero offset (model A/B) + the lattice-step staircase
    # (public log: n79 - n78 in {0..3}, switching while moving) centred on its mean 1.5 steps, so the
    # 0x078 zero offset statistics follow the owner's measured stop zeros (sd ~3.4, |dZ| p90 ~6).
    st = staircase(t, moving, rng) if stairs else np.full(t.size, 1, int)
    ob = owner_bias(t, moving, rng, owner)
    x78 = (wc + 2048.0 + ob - 2047.00190) / 1.52751423 + 1.5
    nt = np.round(x78 + rng.normal(0.0, 0.3, t.size))
    raw78 = np.clip(lattice(nt - st), 0, 4095)
    raw79 = np.clip(raw79, 0, 4095)
    if stuck is not None:
        mv = np.where(moving)[0]
        i = int(mv[int(rng.integers(0, mv.size))])
        j = int(np.searchsorted(t, t[i] + stuck))
        raw79[i:j] = raw79[i]
        info.update(stuck_t=float(t[i]))
    return raw78, raw79, w_dps * DPS_TO_COUNTS, info


DELIV_NOMINAL = dict(L=(0.1, 1.5), jit=0.05, P=(0.02, 0.3), loss=(0.0, 0.10))


def delivery(rng, t, d, dropouts=()):
    L = rng.uniform(*d['L'])
    Pb = rng.uniform(*d['P'])
    loss = rng.uniform(*d['loss'])
    tr = deliver(t, rng, L, d['jit'], Pb, loss, dropouts, d.get('shift', 0.0))
    return tr, dict(L=L, P=Pb, loss=loss)


def make_gps(t, w_counts, sky, raw78, rng, sd_deg=1.5):
    """GPS consistency guard input: every check, over the last GPS_WIN_S, if the span is open-sky
    moving throughout, b_gps = mean(raw78 - 2048) - true mean rate (counts) + noise from two course
    fixes (sd_deg each); b_apply_mean = sample-weighted mean of the applied correction."""
    cum_true = np.r_[0.0, np.cumsum(w_counts)]
    cum_raw = np.r_[0.0, np.cumsum(raw78 - 2048.0)]
    cum_sky = np.r_[0, np.cumsum((sky & (raw78 > 0) & (raw78 < 4094)).astype(int))]   # rail spans skipped

    def fn(tw_ns, hist, off):
        te = tw_ns / NS - off
        ts = te - WP.GPS_WIN_S
        if ts < t[0]:
            return None
        i, j = np.searchsorted(t, ts), np.searchsorted(t, te)
        if j - i < 100 or cum_sky[j] - cum_sky[i] != j - i:
            return None
        n = j - i
        rate_true = (cum_true[j] - cum_true[i]) / n
        noise = (rng.normal(0.0, sd_deg) - rng.normal(0.0, sd_deg)) / (DEG_PER_COUNT * WP.GPS_WIN_S)
        b_gps = (cum_raw[j] - cum_raw[i]) / n - rate_true + noise
        lo = tw_ns - WP.GPS_WIN_S * NS
        num = den = 0.0
        for (tt, b, c, lv) in reversed(hist):
            if tt < lo:
                break
            if not lv:
                return None
            num += b * c
            den += c
        if den <= 0:
            return None
        return int(round(num / den * Q)), int(round(b_gps * Q))
    return fn


def scenario(name, rng, pub=None):
    """Build one instance. Returns sc dict with t, raw78, raw79, still, tr, truth increments (deg),
    windows, kind (N nominal / F fault) and info."""
    d = dict(DELIV_NOMINAL)
    drop = ()
    kw = {}
    gps = True
    owner = 'A'
    app_fill = True
    win = (4, 6)
    if name in ('S01_public', 'S02_public_owner'):
        t, r78, r79, still = pub['t'], pub['raw78'].copy(), pub['raw79'].copy(), pub['still']
        z = float(np.mean(r79[still]))
        if name == 'S02_public_owner':
            r78 = np.clip(np.round(r78 + owner_bias(t, ~still, rng, 'A')), 0, 4095)
        tr, dinfo = delivery(rng, t, d)
        ht = -(r79 - z) * DEG_PER_COUNT * 0.020
        return dict(t=t, raw78=r78, raw79=r79, still=still, tr=tr, ht=ht, wins=sliding(0.0, t[-1]),
                    info=dinfo, gps=None, win=win, slot=pub['slot'])
    if name.startswith('S03') or name.startswith('S07') or name.startswith('S14') or name == 'F1_scale_garage':
        pre = 30.0
        if name.startswith('S07'):
            pre = rng.uniform(6.0, 9.0)
        t, w, still, sky, wins = motion_garage(rng, pre=pre)
        gps = False
        if name.startswith('S14'):
            # no standstill at all with OBD: OBD starts 2 s after departure
            pass
    elif name.startswith('S04') or name.startswith('S09') or name.startswith('S12') or name.startswith('S13') \
            or name in ('F2_jump_tunnel', 'F4_lag_out_of_range', 'F6_drops_loss'):
        t, w, still, sky, wins = motion_tunnel(rng)
    elif name.startswith('S08'):
        t, w, still, sky, wins = motion_urban(rng, reverse=True)
    elif name.startswith('S10'):
        t, w, still, sky, wins = motion_urban(rng, rail=True)
    else:
        t, w, still, sky, wins = motion_urban(rng)
    if name.startswith('S06'):
        owner = 'B'
    if name.startswith('S09'):
        d.update(P=(0.3, 0.6), jit=0.10)
    if name.startswith('S13'):
        d.update(L=(1.2, 1.5))
    if name == 'F4_lag_out_of_range':
        d.update(L=(1.6, 3.0))
    if name.startswith('S12'):
        a = rng.uniform(130.0, 200.0)
        drop = ((a, 1e9),)
    if name.startswith('S11'):
        drop = ((0.0, 60.0),)
    if name.startswith('S14'):
        drop = ((0.0, 32.0),)            # garage pre-stop is 30 s: OBD only after departure
    if name == 'F6_drops_loss':
        d.update(loss=(0.10, 0.10))
        drop = tuple((a, a + rng.uniform(1.0, 3.0)) for a in rng.uniform(0.0, 230.0, size=3))
    if name.startswith('S05x'):
        win = (2, 15)
    if name == 'F5_old_app':
        app_fill = False
        d.update(loss=(0.02, 0.10))
    if name.startswith('F1_scale'):
        kw['scale79'] = rng.uniform(0.95, 1.05)
    if name.startswith('S15'):
        kw['scale_common'] = rng.uniform(0.95, 1.05)
    if name.startswith('F2_jump'):
        kw['jump'] = (rng.uniform(3.0, 6.0), np.ones(t.size, bool) if name == 'F2_jump_urban'
                      else (t >= 120.0))
    if name == 'F3_stuck':
        kw['stuck'] = 30.0
    r78, r79, wc, info = build_signals(rng, t, w, still, owner=owner, **kw)
    tr, dinfo = delivery(rng, t, d, drop)
    info.update(dinfo)
    ht = -wc * DEG_PER_COUNT * 0.020
    sc = dict(t=t, raw78=r78, raw79=r79, still=still, tr=tr, ht=ht, wins=wins, info=info, win=win,
              app_fill=app_fill, sky=sky, slot=np.arange(t.size, dtype=np.int64))
    sc['gps'] = make_gps(t, wc, sky, r78, rng) if gps and sky.any() else None
    return sc


NOMINAL = ['S01_public', 'S02_public_owner', 'S03_garage', 'S04_tunnel', 'S05_urban', 'S05x_urban_windows',
           'S06_urban_ownerB', 'S07_garage_short_stop', 'S08_reverse_parking', 'S09_tunnel_burst',
           'S10_urban_rail', 'S11_urban_obd_late', 'S12_tunnel_obd_lost', 'S13_tunnel_lag_edge',
           'S14_garage_no_stop', 'S15_urban_common_scale']
FAULTS = ['F1_scale_urban', 'F1_scale_garage', 'F2_jump_urban', 'F2_jump_tunnel', 'F3_stuck',
          'F4_lag_out_of_range', 'F5_old_app', 'F6_drops_loss']


def oracle_b(sc, sim):
    """Reference tracker (not implementable): the true 0x078 zero offset (raw78 - 2048 - true rate,
    rail windows excluded) averaged over production [t - 23 s, t - 3 s], applied to each non-rail
    window at its receipt time. Measures the harm any 20 s / 3 s tracker cannot avoid."""
    t = sc['t']
    wc = -sc['ht'] / (DEG_PER_COUNT * 0.020)
    bt = sc['raw78'] - 2048.0 - wc
    okm = np.ones(t.size, bool)
    first = sim['w_last'] - sim['wcnt'] + 1
    for a, b, r in zip(first, sim['w_last'], sim['rail']):
        if r:
            okm[a:b + 1] = False
    cs = np.r_[0.0, np.cumsum(np.where(okm, bt, 0.0))]
    cn = np.r_[0, np.cumsum(okm.astype(int))]
    wt = sim['w_ns'] / NS - sim['off']
    hi = np.searchsorted(t, wt - 3.0)
    lo = np.searchsorted(t, wt - 23.0)
    n = cn[hi] - cn[lo]
    orc = np.where(n > 100, (cs[hi] - cs[lo]) / np.maximum(n, 1), 0.0)
    return np.where(sim['rail'], 0.0, orc)


def run_one(name, seed, pub=None):
    rng = np.random.default_rng(seed)
    sc = scenario(name, rng, pub)
    sim = simulate(sc, rng)
    hc, hu = window_heading(sim, sc['t'].size, sc['ht'])
    errs = window_errors(sc['t'], hc, hu, sc['ht'], sc['wins'])
    sim_o = dict(sim)
    sim_o['b'] = oracle_b(sc, sim)
    ho, _ = window_heading(sim_o, sc['t'].size, sc['ht'])
    errs_o = window_errors(sc['t'], ho, hu, sc['ht'], sc['wins'])
    recs = [decode_ob(r) for r in sim['recs']]
    st = [r['status'] for r in recs]
    nb = max(len(st), 1)
    off_after = None
    if 'jump_t' in sc['info']:
        off_after = jump_detection(sc, sim, recs)
    return dict(name=name, seed=seed, errs=errs, oracle=[e[2] for e in errs_o],
                info={k: v for k, v in sc['info'].items()},
                valid_share=st.count(ST_VALID) / nb, holding_share=st.count(ST_HOLDING) / nb,
                b_nonzero=bool(np.any(sim['b'] != 0.0)), drops=sim['worker'].drops,
                events=[e[0] for e in sim['worker'].events], cp=sim['col'].cp_count,
                relearn=sim['col'].relearns, off_lattice=sim['col'].off_lattice, jump=off_after)


def jump_detection(sc, sim, recs):
    """For a 0x079 zero jump: time from the start of the first >= 3 s standstill after the jump to
    the first window where the applied correction is 0 (None if no such standstill / never off)."""
    t, still = sc['t'], sc['still']
    jt = sc['info']['jump_t']
    i0 = np.searchsorted(t, jt)
    s_start = None
    run = 0
    for i in range(i0, t.size):
        run = run + 1 if still[i] else 0
        if run == 150:
            s_start = t[i - 149]
            break
    gps_off = [e[1] for e in sim['worker'].events if e[0] == 'gps_disable']
    w_t = sim['w_ns'] / NS - sim['off']
    zero = np.where((w_t > jt) & (sim['b'] == 0.0))[0]
    first_zero = float(w_t[zero[0]]) if zero.size else None
    return dict(jump_t=jt, still_start=None if s_start is None else float(s_start), first_zero=first_zero,
                gps_off=[g / NS - sim['off'] for g in gps_off])


# ------------------------------------------------------------------------------------------------
# Public log loader (identical decode to tools/yaw_obd_bias.py)
# ------------------------------------------------------------------------------------------------
def load_public(path):
    import csv
    data = open(path, 'rb').read()
    if hashlib.sha256(data).hexdigest() != PUBLIC_SHA256:
        raise SystemExit('not the public ND log (sha256 differs)')
    f78, f79, f215 = [], [], []
    for row in csv.reader(data.decode('ascii').splitlines()[1:]):
        try:
            tt, addr, dd = float(row[0]), int(row[1]), bytes.fromhex(row[3])
        except (ValueError, IndexError):
            continue
        if addr == 0x078 and len(dd) >= 4:
            f78.append((tt, ((dd[1] & 1) << 11) | (dd[2] << 3) | (dd[3] >> 5)))
        elif addr == 0x079 and len(dd) >= 6 and not (dd[3] >> 2):
            f79.append((tt, ((dd[3] & 3) << 10) | (dd[4] << 2) | (dd[5] >> 6)))
        elif addr == 0x215 and len(dd) >= 8:
            f215.append((tt, all(int.from_bytes(dd[i:i + 2], 'big') == 10000 for i in (0, 2, 4, 6))))
    f78.sort(); f79.sort(); f215.sort()
    n = min(len(f78), len(f79))
    t = np.array([x[0] for x in f78[:n]])
    r78 = np.array([x[1] for x in f78[:n]], float)
    r79 = np.array([x[1] for x in f79[:n]], float)
    tw = np.array([x[0] for x in f215])
    zw = np.array([x[1] for x in f215], bool)
    still = zw[np.clip(np.searchsorted(tw, t) - 1, 0, tw.size - 1)]
    # the logger has gaps: re-time onto the 20 ms production grid by frame order (both signals are
    # produced in pairs every 20 ms; gaps are logger gaps, kept as gaps by skipping grid slots)
    t = t - t[0]
    # Production slots: the logger timestamps are batched (0.4-60 ms apart, 49.55 pairs/s over
    # 291 s), so consecutive pairs are consecutive 20 ms slots unless the logger gap exceeds 0.1 s
    # (then round(gap / 20 ms) slots). The simulation runs on the regular production grid.
    d = np.diff(t)
    step = np.where(d > 0.1, np.maximum(1, np.rint(d / 0.02)), 1).astype(np.int64)
    slot = np.r_[0, np.cumsum(step)]
    t = slot * 0.02
    return dict(t=t, raw78=r78, raw79=r79, still=still, slot=slot)


# ------------------------------------------------------------------------------------------------
# Statistics and the pre-registered rule
# ------------------------------------------------------------------------------------------------
def boot_ci(runs_vals, fn, rng, B=1000):
    """Percentile bootstrap over runs (each element: list of per-window tuples)."""
    n = len(runs_vals)
    vals = []
    for _ in range(B):
        pick = rng.integers(0, n, size=n)
        pool = [x for i in pick for x in runs_vals[i]]
        if pool:
            vals.append(fn(pool))
    return float(np.percentile(vals, 2.5)), float(np.percentile(vals, 97.5))


def p90c(pool):
    return float(np.percentile([abs(e[2]) for e in pool], 90))


def p90u(pool):
    return float(np.percentile([abs(e[3]) for e in pool], 90))


def ratio(pool):
    return p90c(pool) / max(p90u(pool), 1e-9)


def harm(e):
    return abs(e[2]) - abs(e[3])


def share2(pool):
    return float(np.mean([harm(e) > 2.0 for e in pool]))


def kterm(pool):
    r = [max(0.0, abs(e[2]) * math.pi / 180 - 0.02 * e[4] * math.pi / 180) / e[1] for e in pool]
    return float(np.percentile(r, 97))


def clopper_pearson_upper(k, n, a=0.05):
    from scipy.stats import beta
    return float(beta.ppf(1 - a / 2, k + 1, n - k)) if k < n else 1.0


def budget_g3(e):
    """Strict garage (G3) budget of an uncorrected yaw integration over the window (deg):
    0.0023 rad/s * T + 0.02 * |rotation| (src/runtime/g3_heading.h)."""
    return 0.0023 * e[1] * 180.0 / math.pi + 0.02 * e[4]


def summarize(name, runs, rng, B=1000):
    per = [r['errs'] for r in runs]
    pool = [e for x in per for e in x]
    if not pool:
        return dict(n_runs=len(runs), n_windows=0)
    h = np.array([harm(e) for e in pool])
    ho = np.array([abs(o) - abs(e[3]) for r in runs for e, o in zip(r['errs'], r['oracle'])])
    runs_bad5 = sum(1 for x in per if x and max(harm(e) for e in x) > 5.0)
    newviol = sum(1 for e in pool if abs(e[2]) > budget_g3(e) + 2.0 and abs(e[3]) <= budget_g3(e))
    out = dict(n_runs=len(runs), n_windows=len(pool), distinct_windows=len({(e[0], e[1]) for e in pool}),
               corr_med=float(np.median([abs(e[2]) for e in pool])), corr_p90=p90c(pool),
               unc_med=float(np.median([abs(e[3]) for e in pool])), unc_p90=p90u(pool),
               oracle_p90=float(np.percentile([abs(o) for r in runs for o in r['oracle']], 90)),
               ratio_p90=ratio(pool), ratio_p90_ci=boot_ci(per, ratio, rng, B),
               share_h2=share2(pool), share_h2_ci=boot_ci(per, share2, rng, B),
               share_h5=float(np.mean(h > 5.0)),
               h_max=float(h.max()), h_p99=float(np.percentile(h, 99)), h_p999=float(np.percentile(h, 99.9)),
               h_max_short=float(max([harm(e) for e in pool if e[1] <= 150.0], default=float('nan'))),
               oracle_h_max=float(ho.max()), oracle_share_h2=float(np.mean(ho > 2.0)),
               oracle_share_h5=float(np.mean(ho > 5.0)),
               runs_h5=runs_bad5, runs_h5_cp_upper=clopper_pearson_upper(runs_bad5, len(runs)),
               budget_new_violations=newviol,
               k_p97=kterm(pool), valid_share=float(np.mean([r['valid_share'] for r in runs])),
               holding_share=float(np.mean([r['holding_share'] for r in runs])),
               any_b_nonzero_runs=sum(1 for r in runs if r['b_nonzero']),
               cp_mean=float(np.mean([r['cp'] for r in runs])),
               relearn_runs=sum(1 for r in runs if r.get('relearn')),
               off_lattice_runs=sum(1 for r in runs if r.get('off_lattice')),
               events={}, drops={})
    for r in runs:
        for e in r['events']:
            out['events'][e] = out['events'].get(e, 0) + 1
        for k, v in r['drops'].items():
            out['drops'][k] = out['drops'].get(k, 0) + v
    return out


CLASS_N = ['S01_public', 'S02_public_owner', 'S03_garage', 'S04_tunnel', 'S05_urban', 'S05x_urban_windows',
           'S06_urban_ownerB', 'S07_garage_short_stop', 'S08_reverse_parking', 'S09_tunnel_burst',
           'S10_urban_rail', 'S13_tunnel_lag_edge', 'S15_urban_common_scale']
CLASS_D = ['S11_urban_obd_late', 'S12_tunnel_obd_lost', 'F4_lag_out_of_range', 'F5_old_app', 'F6_drops_loss']
CLASS_Z = ['S14_garage_no_stop']
CLASS_F = ['F1_scale_urban', 'F1_scale_garage', 'F2_jump_urban', 'F2_jump_tunnel', 'F3_stuck']


def judge(by, stats_):
    """The pre-registered rule (validation/YAW_OBD_BIAS_E3B_2026-10-19.md, 'pre-registration')."""
    v = {}
    have_pub = 'S01_public' in stats_
    n_ok = all(stats_[n]['n_runs'] == (50 if n == 'S01_public' else 100) for n in stats_)
    npool = sum(stats_[n]['n_windows'] for n in CLASS_N if n in stats_)
    v['R0'] = bool(n_ok and npool >= 10000 and (not have_pub or stats_['S01_public']['distinct_windows'] >= 20))
    r1 = {}
    for n in ('S02_public_owner', 'S03_garage', 'S04_tunnel', 'S05_urban'):
        if n in stats_:
            s_ = stats_[n]
            r1[n] = bool(s_['ratio_p90'] <= 0.40 and s_['ratio_p90_ci'][1] <= 0.50)
    if have_pub:
        s_ = stats_['S01_public']
        r1['S01_public'] = bool(s_['corr_p90'] <= max(0.6 * s_['unc_p90'], 1.0))
    syn = [e for n in CLASS_N if n in by and not n.startswith(('S01', 'S02')) for r in by[n] for e in r['errs']]
    r1['pooled_synthetic_N'] = bool(ratio(syn) <= 0.40)
    v['R1'] = all(r1.values())
    v['R1_detail'] = r1
    r2 = {}
    for n in CLASS_N:
        if n not in stats_:
            continue
        s_ = stats_[n]
        r2[n] = dict(
            share2=bool(s_['share_h2'] <= 0.05),
            max10=bool(s_['h_max'] <= 10.0),
            budget=bool(s_['budget_new_violations'] == 0),
            rel_share2=bool(s_['share_h2'] <= s_['oracle_share_h2'] + 0.010),
            rel_share5=bool(s_['share_h5'] <= s_['oracle_share_h5'] + 0.005),
            rel_max=bool(s_['h_max'] <= max(s_['oracle_h_max'], 0.0) + 2.0))
    v['R2'] = all(all(x.values()) for x in r2.values())
    v['R2_detail'] = r2
    r3 = {}
    for n in CLASS_D:
        if n not in stats_:
            continue
        s_ = stats_[n]
        ok = s_['ratio_p90'] <= 1.10 and s_['h_max'] <= 10.0 and s_['budget_new_violations'] == 0
        if n == 'F5_old_app':
            ok = ok and s_['any_b_nonzero_runs'] == 0
        r3[n] = bool(ok)
    v['R3'] = all(r3.values())
    v['R3_detail'] = r3
    v['R4'] = bool(all(stats_[n]['any_b_nonzero_runs'] == 0 for n in CLASS_Z if n in stats_))
    pool = [e for n in ('S02_public_owner', 'S03_garage', 'S04_tunnel', 'S05_urban') if n in by
            for r in by[n] for e in r['errs']]
    per = [r['errs'] for n in ('S02_public_owner', 'S03_garage', 'S04_tunnel', 'S05_urban') if n in by
           for r in by[n]]
    k = kterm(pool)
    k_ci = boot_ci(per, kterm, np.random.default_rng(20261020), 1000)
    v['k_obd_p97'] = k
    v['k_obd_ci'] = k_ci
    v['k_obd_proposal'] = round(math.ceil(k_ci[1] / 0.0001 - 1e-9) * 0.0001, 4)
    v['R5'] = bool(k <= 0.0010)
    r6 = {}
    for n in CLASS_F:
        if n in stats_ and n != 'F2_jump_tunnel':
            r6[n] = bool(stats_[n]['ratio_p90'] <= 1.10)
    if 'F2_jump_urban' in by:
        det = []
        for r in by['F2_jump_urban']:
            j = r['jump']
            if j is None or j['still_start'] is None:
                continue
            fz = j['first_zero']
            gps = [g for g in j['gps_off'] if g > j['jump_t']]
            ok = (fz is not None and fz <= j['still_start'] + 60.0) or bool(gps and min(gps) <= j['still_start'] + 60.0)
            det.append(ok)
        r6['F2_urban_detect'] = bool(det and np.mean(det) >= 0.95)
        v['F2_urban_detect_share'] = float(np.mean(det)) if det else None
        v['F2_urban_detect_n'] = len(det)
    v['R6'] = all(r6.values())
    v['R6_detail'] = r6
    v['PASS'] = bool(v['R0'] and v['R1'] and v['R2'] and v['R3'] and v['R4'] and v['R5'] and v['R6'])
    return v


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('evaluate')
    e.add_argument('--csv')
    e.add_argument('--seed', type=int, required=True)
    e.add_argument('--reps', type=int, default=100)
    e.add_argument('--only', nargs='*')
    e.add_argument('--json', required=True)
    e.add_argument('--jobs', type=int, default=1)
    a = ap.parse_args(argv)
    pub = load_public(a.csv) if a.csv else None
    names = a.only or (NOMINAL + FAULTS)
    if pub is None:
        names = [n for n in names if not n.startswith(('S01', 'S02'))]
    tasks = []
    for si, n in enumerate(names):
        reps = a.reps // 2 if n == 'S01_public' else a.reps
        for r in range(reps):
            tasks.append((n, a.seed * 1000003 + (zlib.crc32(n.encode()) % 100003) * 1009 + r))
    if a.jobs > 1:
        import multiprocessing as mp
        global _PUB
        _PUB = pub
        with mp.get_context('fork').Pool(a.jobs) as pool:
            results = pool.map(_task, tasks, chunksize=4)
    else:
        results = [run_one(n, s, pub) for n, s in tasks]
    by = {}
    for r in results:
        by.setdefault(r['name'], []).append(r)
    rng = np.random.default_rng(a.seed + 1)
    stats_ = {n: summarize(n, by[n], rng) for n in by}
    verdict = judge(by, stats_)
    json.dump(dict(seed=a.seed, reps=a.reps, stats=stats_, verdict=verdict, results=results), open(a.json, 'w'))
    print(json.dumps(verdict, indent=1, default=str))
    for n, s_ in stats_.items():
        if s_.get('n_windows'):
            print('%-22s n=%6d p90 %6.2f/%6.2f (oracle %5.2f) ratio %.3f [%.3f,%.3f] sh2 %.3f sh5 %.4f hmax %6.2f '
                  '(orc %6.2f) newviol %d valid %.2f'
                  % (n, s_['n_windows'], s_['corr_p90'], s_['unc_p90'], s_['oracle_p90'], s_['ratio_p90'],
                     s_['ratio_p90_ci'][0], s_['ratio_p90_ci'][1], s_['share_h2'], s_['share_h5'], s_['h_max'],
                     s_['oracle_h_max'], s_['budget_new_violations'], s_['valid_share']))
    return 0 if verdict['PASS'] else 1


_PUB = None


def _task(x):
    return run_one(x[0], x[1], _PUB)


if __name__ == '__main__':
    sys.exit(main())
