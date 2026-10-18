#!/usr/bin/env python3
"""Stage 3 prototype: correct the CMU yaw (0x078 via VIM 0x116) zero online with the OBD 0x079 yaw.

OFFLINE RESEARCH TOOL (validation/YAW_OBD_BIAS_DESIGN_2026-10-18.md). Nothing here is product code
and nothing reads or writes the vehicle. It implements the pre-registered estimator E1 exactly as the
record states it, a Bluetooth delivery model, an owner-like 0x078 zero model, synthetic garage and
tunnel scenarios, the public ND CAN log loader (sha256 checked, read only, never copied) and the
pre-registered acceptance rule.

  yaw_obd_bias.py evaluate --csv PUBLIC_ND_LOG.csv [--json out.json] [--runs 200] [--seed 1]
  yaw_obd_bias.py synthetic [--json out.json]           (no public log: D3/D4 only)

Conventions (FACT, src/navigation/pipeline.cpp:448, beta_profile.h:202; make_obd_vectors.py): the
heading rate is (mean_counts - zero) * -0.000658615 rad/s (clockwise +); the BETA zero is 2048; one
0x078/0x079 count is 0.03774 deg/s; both signals sit on the common lattice round(2047.0019 + 1.5275 n).
"""
import argparse
import hashlib
import json
import math
import sys

import numpy as np

RAD_PER_COUNT = 0.000658615
DEG_PER_COUNT = RAD_PER_COUNT * 180.0 / math.pi
PERIOD = 0.020
ZERO78 = 2048.0
ZERO79_NOMINAL = 2047.0
PUBLIC_SHA256 = '18983a366b46d8bea62ea426652798ca24cfef23f932e3751442485938aab54f'
HOOK_LATENCY_S = 0.02


def lattice(n):
    return np.floor(2047.00190 + 1.52751423 * np.asarray(n, dtype=float) + 0.5)


# ----------------------------------------------------------------------------------------------
# Estimator E1 (pre-registered parameters)
# ----------------------------------------------------------------------------------------------
class Params:
    W = 20                 # bins in the trailing fit window
    MIN_USABLE = 10        # H2
    HUBER_C = 1.0          # counts
    IRLS_ITERS = 5
    STILL_WEIGHT = 3.0
    F_MAX = 1.5            # lag tap clamp, seconds
    RMS_MAX = 1.5          # H3, counts
    B_MAX = 16.0           # H4, counts
    SCALE_BINS = 60        # H5 window
    SCALE_ROT_MIN = 2400.0  # counts*s (about 90 deg)
    SCALE_LO, SCALE_HI = 0.95, 1.05
    SCALE_BLOCK_S = 60.0
    STILL_FAULT = 1.0      # H6, counts
    STILL_FAULT_BLOCK_S = 60.0
    Z79_MIN_BINS = 5       # H1
    HOLD_S = 15.0
    RAMP_S = 15.0
    N79_MIN = 25           # usable bin (amended pre-registration)
    N79_SUM3_MIN = 135
    N78_MIN = 45
    PROCESS_DELAY_BINS = 2  # bin k is processed when bin k+2 ends


def huber_fit(r, g, w0, p):
    """Robust fit r = b + f g, f clamped to [0, F_MAX]. Returns b, f, weighted rms."""
    w = w0.copy()
    b, f = float(np.median(r)), 0.0
    for _ in range(p.IRLS_ITERS):
        sw = w.sum()
        if sw <= 0:
            break
        X = np.stack([np.ones_like(g), g], axis=1)
        A = X.T @ (X * w[:, None])
        y = X.T @ (w * r)
        try:
            sol = np.linalg.solve(A + 1e-9 * np.eye(2), y)
            b, f = float(sol[0]), float(sol[1])
        except np.linalg.LinAlgError:
            f = 0.0
        if f < 0.0 or f > p.F_MAX:
            f = min(max(f, 0.0), p.F_MAX)
        b = float((w * (r - f * g)).sum() / sw)
        e = r - b - f * g
        a = np.abs(e)
        w = w0 * np.where(a <= p.HUBER_C, 1.0, p.HUBER_C / np.maximum(a, 1e-12))
    e = r - b - f * g
    rms = math.sqrt(float((w * e * e).sum() / max(w.sum(), 1e-12)))
    return b, f, rms


class E1:
    """Bin-level online estimator. Feed complete bins in order with process(k, t_now, ...)."""

    def __init__(self, p=Params):
        self.p = p
        self.z79_samples = []
        self.z79 = None
        self.usable = []            # list of (k, r, g, still, a, o)
        self.valid = False
        self.b_hat = 0.0
        self.last_valid_b = None
        self.last_valid_t = None
        self.block_until = -1e18
        self.ever_valid = False
        self.reason = 'start'
        self.f = 0.0
        self.scale = None

    def process(self, t_now, k, a, o, still, usable):
        """a, o: mean counts of bin k (0x078 rel. 2048, 0x079 rel. 2047) or None;
        o_prev/o_next folded in via usable tuple; still: bool for k-1..k+1."""
        p = self.p
        if usable is not None:
            o_prev, o_next = usable
            if still:
                if self.z79 is not None and abs(o - (self.z79 - ZERO79_NOMINAL)) > p.STILL_FAULT:
                    self.block_until = max(self.block_until, t_now + p.STILL_FAULT_BLOCK_S)
                    self.reason = 'still_fault'
                self.z79_samples.append(o)
                if len(self.z79_samples) >= p.Z79_MIN_BINS:
                    self.z79 = ZERO79_NOMINAL + float(np.median(self.z79_samples))
            g = (o_next - o_prev) / 2.0
            self.usable.append((k, a, o, g, still))
        # trailing window by bin index
        win = [u for u in self.usable if u[0] > k - p.W]
        ok = True
        reason = 'ok'
        if self.z79 is None:
            ok, reason = False, 'no_z79'
        elif len(win) < p.MIN_USABLE:
            ok, reason = False, 'few_bins'
        b = f = rms = None
        if ok:
            zoff = self.z79 - ZERO79_NOMINAL
            r = np.array([u[1] - (u[2] - zoff) for u in win])
            g = np.array([u[3] for u in win])
            w0 = np.array([p.STILL_WEIGHT if u[4] else 1.0 for u in win])
            b, f, rms = huber_fit(r, g, w0, p)
            self.f = f
            if rms > p.RMS_MAX:
                ok, reason = False, 'rms'
            elif abs(b) > p.B_MAX:
                ok, reason = False, 'b_max'
            # H5 scale gate
            sw = [u for u in self.usable if u[0] > k - p.SCALE_BINS]
            if len(sw) >= p.MIN_USABLE:
                oc = np.array([u[2] - zoff for u in sw])
                if float(np.abs(oc).sum()) >= p.SCALE_ROT_MIN:
                    X = np.stack([np.ones_like(oc), oc, np.array([u[3] for u in sw])], axis=1)
                    y = np.array([u[1] for u in sw])
                    sol, *_ = np.linalg.lstsq(X, y, rcond=None)
                    self.scale = float(sol[1])
                    if not (p.SCALE_LO <= self.scale <= p.SCALE_HI):
                        self.block_until = max(self.block_until, t_now + p.SCALE_BLOCK_S)
                        if ok:
                            reason = 'scale'
        if t_now < self.block_until:
            ok = False
            if reason == 'ok':
                reason = 'blocked'
        self.reason = reason
        if ok:
            self.valid = True
            self.ever_valid = True
            self.b_hat = b
            self.last_valid_b, self.last_valid_t = b, t_now
        else:
            self.valid = False
            if self.last_valid_t is None:
                self.b_hat = 0.0
            else:
                age = t_now - self.last_valid_t
                if age <= p.HOLD_S:
                    self.b_hat = self.last_valid_b
                elif age <= p.HOLD_S + p.RAMP_S:
                    self.b_hat = self.last_valid_b * (1.0 - (age - p.HOLD_S) / p.RAMP_S)
                else:
                    self.b_hat = 0.0
        # keep memory bounded
        if len(self.usable) > 4 * p.SCALE_BINS:
            self.usable = self.usable[-2 * p.SCALE_BINS:]
        return self.b_hat


class P2(Params):
    QUIET = 8.0            # counts, |o - zoff| for k-1..k+1
    SEG_MIN = 10           # bins between quiet endpoints
    SEG_MAX = 60
    SEG_AGE_MAX_S = 30.0


class E2:
    """Segment-sum estimator between quiet endpoints (pre-registration 2).

    Rotation moved between adjacent receipt bins by latency or burst delivery cancels inside a
    contiguous sum; only the endpoint bins matter, and they are chosen quiet."""

    def __init__(self, p=P2):
        self.p = p
        self.z79_samples = []
        self.z79 = None
        self.bins = {}             # k -> (A, n78, S, n79, ok_obd)
        self.quiet = []            # quiet bin indices (processed)
        self.valid = False
        self.b_hat = 0.0
        self.b_raw = None
        self.seg_end_t = None
        self.last_valid_b = None
        self.last_valid_t = None
        self.block_until = -1e18
        self.reason = 'start'
        self.f = 0.0
        self.scale = None
        self.scale_bins = []       # (a, o, g) of bins usable for the scale gate

    def process(self, t_now, k, A, n78, S, n79, obd_ok, still3, o_nb, scale_usable):
        """Bin k complete (with neighbours). obd_ok: frames present, no reset, no unfilled loss.
        o_nb: (o_{k-1}, o_k, o_{k+1}) or None; scale_usable: (a, o, g) or None."""
        p = self.p
        self.bins[k] = (A, n78, S, n79, obd_ok)
        for old in [x for x in self.bins if x < k - 4 * p.SEG_MAX]:
            del self.bins[old]
        if o_nb is not None and still3:
            o = o_nb[1]
            if self.z79 is not None and abs(o - (self.z79 - ZERO79_NOMINAL)) > p.STILL_FAULT:
                self.block_until = max(self.block_until, t_now + p.STILL_FAULT_BLOCK_S)
            self.z79_samples.append(o)
            if len(self.z79_samples) >= p.Z79_MIN_BINS:
                self.z79 = ZERO79_NOMINAL + float(np.median(self.z79_samples))
        reason = 'ok'
        if self.z79 is None:
            reason = 'no_z79'
        else:
            zoff = self.z79 - ZERO79_NOMINAL
            if o_nb is not None and obd_ok and all(abs(x - zoff) <= p.QUIET for x in o_nb):
                self.quiet.append(k)
                self.quiet = [q for q in self.quiet if q >= k - 2 * p.SEG_MAX]
                cands = [q for q in self.quiet if k - p.SEG_MAX <= q <= k - p.SEG_MIN]
                for i in cands:                      # oldest first: longest baseline
                    rng_ = range(i, k + 1)
                    if all(m in self.bins and self.bins[m][4] and self.bins[m][1] > 0 for m in rng_):
                        R78 = sum(self.bins[m][0] for m in rng_) * PERIOD
                        R79 = sum(self.bins[m][2] - zoff * self.bins[m][3] for m in rng_) * PERIOD
                        T = sum(self.bins[m][1] for m in rng_) * PERIOD
                        self.b_raw = (R78 - R79) / T
                        self.seg_end_t = t_now
                        break
            if scale_usable is not None:
                self.scale_bins.append(scale_usable)
                self.scale_bins = self.scale_bins[-p.SCALE_BINS:]
                oc = np.array([u[1] - zoff for u in self.scale_bins])
                if len(self.scale_bins) >= p.MIN_USABLE and float(np.abs(oc).sum()) >= p.SCALE_ROT_MIN:
                    X = np.stack([np.ones_like(oc), oc, np.array([u[2] for u in self.scale_bins])], axis=1)
                    y = np.array([u[0] for u in self.scale_bins])
                    sol, *_ = np.linalg.lstsq(X, y, rcond=None)
                    self.scale = float(sol[1])
                    if not (p.SCALE_LO <= self.scale <= p.SCALE_HI):
                        self.block_until = max(self.block_until, t_now + p.SCALE_BLOCK_S)
            if self.b_raw is None:
                reason = 'no_segment'
            elif t_now - self.seg_end_t > p.SEG_AGE_MAX_S:
                reason = 'segment_old'
            elif abs(self.b_raw) > p.B_MAX:
                reason = 'b_max'
        if reason == 'ok' and t_now < self.block_until:
            reason = 'blocked'
        self.reason = reason
        if reason == 'ok':
            self.valid = True
            self.b_hat = self.b_raw
            self.last_valid_b, self.last_valid_t = self.b_raw, t_now
        else:
            self.valid = False
            if self.last_valid_t is None:
                self.b_hat = 0.0
            else:
                age = t_now - self.last_valid_t
                if age <= p.HOLD_S:
                    self.b_hat = self.last_valid_b
                elif age <= p.HOLD_S + p.RAMP_S:
                    self.b_hat = self.last_valid_b * (1.0 - (age - p.HOLD_S) / p.RAMP_S)
                else:
                    self.b_hat = 0.0
        return self.b_hat


# ----------------------------------------------------------------------------------------------
# Signals: a run is production times t (50 Hz), raw78, raw79, still (wheels zero) per sample.
# ----------------------------------------------------------------------------------------------
def deliver(t, rng, L=None, jitter_sd=0.05, P=None, loss=0.02, dropouts=(), shift=0.0):
    """CMU receipt times of OBD frames (np.nan = lost)."""
    if L is None:
        L = rng.uniform(0.1, 1.0)
    if P is None:
        P = rng.uniform(0.02, 0.3)
    tr = t + L + shift + np.abs(rng.normal(0.0, jitter_sd, size=t.size))
    tr = np.ceil(tr / P) * P
    tr = np.maximum.accumulate(tr)
    keep = rng.random(t.size) >= loss
    for a, b in dropouts:
        keep &= ~((t >= a) & (t < b))
    out = np.where(keep, tr, np.nan)
    return out, L, P


def fill_losses(t, raw79, tr, reset_gap=0.5, fill=True):
    """Phone-side handling of lost frames (pre-registration 2). Returns (raw79', tr', lossflag).

    fill=True: a lost frame inside a production gap <= reset_gap is re-created with the previous
    delivered frame's value and the receipt time of the next delivered frame (flag set, filled).
    fill=False: nothing is re-created; the next delivered frame carries the loss flag (unfilled).
    Gaps longer than reset_gap are left alone (they become frame_reset)."""
    raw = raw79.copy()
    out = tr.copy()
    flag = np.zeros(t.size, bool)
    ok = np.isfinite(tr)
    idx = np.where(ok)[0]
    if idx.size < 2:
        return raw, out, flag
    for a, b in zip(idx[:-1], idx[1:]):
        if b - a > 1:
            if t[b] - t[a] > reset_gap:
                continue
            if fill:
                raw[a + 1:b] = raw[a]
                out[a + 1:b] = tr[b]
                flag[a + 1:b + 1] = True
            else:
                flag[b] = True
    return raw, out, flag


def run_estimator(t, raw78, raw79, still, tr79, p=None, reset_gap=0.5, kind='E1', unfilled=None):
    """Simulate E1, E2 or E3 on one run. Returns (b_hat at each 0x078 sample hook time, diagnostics)."""
    t0 = math.floor(min(t[0], np.nanmin(tr79) if np.any(np.isfinite(tr79)) else t[0]))
    th = t + HOOK_LATENCY_S
    nb = int(math.ceil(max(th[-1], np.nanmax(tr79) if np.any(np.isfinite(tr79)) else th[-1]) - t0)) + 3
    k78 = np.floor(th - t0).astype(int)
    A = np.bincount(k78, weights=raw78 - ZERO78, minlength=nb)[:nb]
    n78 = np.bincount(k78, minlength=nb)[:nb]
    # still per bin: all samples in bin are still and bin has samples
    nst = np.bincount(k78, weights=still.astype(float), minlength=nb)[:nb]
    still_bin = (n78 > 0) & (nst == n78)
    ok79 = np.isfinite(tr79)
    k79 = np.floor(tr79[ok79] - t0).astype(int)
    S = np.bincount(k79, weights=raw79[ok79] - ZERO79_NOMINAL, minlength=nb)[:nb]
    n79 = np.bincount(k79, minlength=nb)[:nb]
    # frame_reset: first frame and after a production gap > reset_gap among delivered frames
    reset = np.zeros(nb, bool)
    tp = t[ok79]
    if tp.size:
        reset[k79[0]] = True
        gaps = np.where(np.diff(tp) > reset_gap)[0] + 1
        reset[k79[gaps]] = True
    if kind == 'E3':
        return run_e3(t, raw78, raw79, still, tr79, unfilled=unfilled)
    if p is None:
        p = P2 if kind == 'E2' else Params
    est = E2(p) if kind == 'E2' else E1(p)
    lossbin = np.zeros(nb, bool)
    if unfilled is not None and np.any(unfilled & ok79):
        lossbin[np.floor(tr79[unfilled & ok79] - t0).astype(int)] = True
    bhat_bin = np.zeros(nb + p.PROCESS_DELAY_BINS + 2)
    times = []
    reasons = {}
    valid_s = 0
    for k in range(1, nb - 1):
        t_now = t0 + k + p.PROCESS_DELAY_BINS + 1
        a = A[k] / n78[k] if n78[k] else None
        o = S[k] / n79[k] if n79[k] else None
        usable = None
        st3 = bool(still_bin[k - 1] and still_bin[k] and still_bin[k + 1])
        if (a is not None and o is not None and n78[k] >= p.N78_MIN and n79[k] >= p.N79_MIN
                and n79[k - 1] > 0 and n79[k + 1] > 0
                and n79[k - 1] + n79[k] + n79[k + 1] >= p.N79_SUM3_MIN
                and not (reset[k - 1] or reset[k] or reset[k + 1])):
            usable = (S[k - 1] / n79[k - 1], S[k + 1] / n79[k + 1])
        if kind == 'E2':
            nb3 = n79[k - 1] > 0 and n79[k] > 0 and n79[k + 1] > 0 and not (reset[k - 1] or reset[k] or reset[k + 1])
            o_nb = (S[k - 1] / n79[k - 1], S[k] / n79[k], S[k + 1] / n79[k + 1]) if nb3 else None
            obd_ok = bool(n79[k] > 0 and not reset[k] and not lossbin[k])
            su = (a, o, (usable[1] - usable[0]) / 2.0) if usable is not None else None
            b = est.process(t_now, k, A[k], n78[k], S[k], n79[k], obd_ok, st3, o_nb, su)
        else:
            b = est.process(t_now, k, a, o, st3, usable)
        idx = k + p.PROCESS_DELAY_BINS + 1     # value applies from t0 + idx onwards
        if idx < bhat_bin.size:
            bhat_bin[idx:] = b
        reasons[est.reason] = reasons.get(est.reason, 0) + 1
        valid_s += 1 if est.valid else 0
        times.append((t_now, b, est.valid, est.reason))
    kk = np.clip(np.floor(th - t0).astype(int), 0, bhat_bin.size - 1)
    return bhat_bin[kk], dict(reasons=reasons, valid_bins=valid_s, z79=est.z79, f=est.f,
                              scale=est.scale, timeline=times)


# ----------------------------------------------------------------------------------------------
# E3 (pre-registration 3): producer-time restoration + constant lag + aligned trailing mean
# ----------------------------------------------------------------------------------------------
class P3(Params):
    CHAIN_GAP_S = 0.3        # > 15 frames: the 4-bit CAN counter is ambiguous -> new chain
    BLOCK = 500              # frames per envelope block (10 s)
    LAG_GRID = 0.1
    LAG_MAX = 1.5
    LAG_STEP = 0.02
    LAG_SPAN_S = 120.0
    LAG_EVERY_S = 10.0
    LAG_MIN_SD = 2.0         # counts per 0.1 s cell, first difference sd, both signals
    TRIM = 0.10
    SD_MAX = 3.0
    E3_DELAY_S = 3.0
    N_MIN = 45


def restore_times(tp, tr, p=P3):
    """Causal producer-time estimate of delivered OBD frames (collector side).

    tp: true producer times (used ONLY to model the CAN counter step between consecutive
    delivered frames, which the app knows); tr: CMU receipt times. Returns (t_hat, chain_id)."""
    n = tr.size
    t_hat = np.empty(n)
    chain = np.empty(n, int)
    cid = -1
    for j in range(n):
        if j == 0 or tp[j] - tp[j - 1] > p.CHAIN_GAP_S:
            cid += 1
            i = 0
            blocks = []          # (i_at_min, u_min) of completed blocks
            cur_min = None
            cur_i = None
            blk_start = 0
            first_min = None
        else:
            i += max(1, int(round((tp[j] - tp[j - 1]) / PERIOD)))
        u = tr[j] - PERIOD * i
        if i - blk_start >= p.BLOCK:
            if cur_min is not None:
                blocks.append((cur_i, cur_min))
            blk_start = i - ((i - blk_start) % p.BLOCK)
            cur_min, cur_i = None, None
        if cur_min is None or u < cur_min:
            cur_min, cur_i = u, i
        if first_min is None or (not blocks and u < first_min):
            first_min = u if first_min is None else min(first_min, u)
        if len(blocks) >= 2:
            bi = np.array([b[0] for b in blocks[-6:]], float)
            bu = np.array([b[1] for b in blocks[-6:]])
            A = np.stack([np.ones_like(bi), bi], axis=1)
            sol, *_ = np.linalg.lstsq(A, bu, rcond=None)
            line = sol[0] + sol[1] * i
            line = min(line, cur_min)          # never later than an observed minimum latency
        elif blocks:
            line = min(blocks[0][1], cur_min)
        else:
            line = cur_min
        t_hat[j] = line + PERIOD * i
        chain[j] = cid
    return t_hat, chain


def estimate_lag(th78, raw78, t_hat79, raw79, t_now, p=P3):
    """Lag (s) of the restored OBD time axis behind the 0x078 hook axis; None if not decidable."""
    lo = t_now - p.LAG_SPAN_S
    m78 = (th78 >= lo) & (th78 < t_now)
    m79 = (t_hat79 >= lo) & (t_hat79 < t_now + p.LAG_MAX)
    if m78.sum() < 1000 or m79.sum() < 1000:
        return None
    ncell = int(p.LAG_SPAN_S / p.LAG_GRID)
    c78 = np.floor((th78[m78] - lo) / p.LAG_GRID).astype(int)
    ok = (c78 >= 0) & (c78 < ncell)
    s78 = np.bincount(c78[ok], raw78[m78][ok], ncell); n78 = np.bincount(c78[ok], minlength=ncell)
    v78 = np.where(n78 > 0, s78 / np.maximum(n78, 1), np.nan)
    d78 = np.diff(v78)
    best, best_c = None, -2.0
    sd_ok = False
    for lag in np.arange(0.0, p.LAG_MAX + 1e-9, p.LAG_STEP):
        c79 = np.floor((t_hat79[m79] - lag - lo) / p.LAG_GRID).astype(int)
        ok = (c79 >= 0) & (c79 < ncell)
        s79 = np.bincount(c79[ok], raw79[m79][ok], ncell); n79 = np.bincount(c79[ok], minlength=ncell)
        v79 = np.where(n79 > 0, s79 / np.maximum(n79, 1), np.nan)
        d79 = np.diff(v79)
        g = np.isfinite(d78) & np.isfinite(d79)
        if g.sum() < 200:
            continue
        a, b = d78[g], d79[g]
        if a.std() < p.LAG_MIN_SD or b.std() < p.LAG_MIN_SD:
            return None
        sd_ok = True
        c = float(np.corrcoef(a, b)[0, 1])
        if c > best_c:
            best_c, best = c, float(lag)
    return best if sd_ok else None


def run_e3(t, raw78, raw79, still, tr79, unfilled=None, p=P3):
    th = t + HOOK_LATENCY_S
    ok79 = np.isfinite(tr79)
    idx = np.where(ok79)[0]
    if idx.size == 0:
        return np.zeros(t.size), dict(reasons={'no_obd': 1}, valid_bins=0, z79=None, f=None, scale=None, timeline=[])
    order = idx[np.argsort(tr79[idx], kind='stable')]
    order = np.sort(order)                         # producer order (receipt is monotone anyway)
    tp = t[order]; tr = tr79[order]; r79 = raw79[order]
    unf = unfilled[order] if unfilled is not None else np.zeros(order.size, bool)
    t_hat, chain = restore_times(tp, tr, p)
    reset_frame = np.r_[True, chain[1:] != chain[:-1]]
    t0 = math.floor(min(th[0], tr[0]))
    nb = int(math.ceil(max(th[-1], tr[-1]) - t0)) + 6
    k78 = np.floor(th - t0).astype(int)
    A = np.bincount(k78, weights=raw78 - ZERO78, minlength=nb)[:nb]
    n78 = np.bincount(k78, minlength=nb)[:nb]
    nst = np.bincount(k78, weights=still.astype(float), minlength=nb)[:nb]
    still_bin = (n78 > 0) & (nst == n78)
    lag = 0.0
    cached_lag = None
    next_lag_t = -1e18
    z79_samples, z79 = [], None
    usable = []          # (k, r, still, a, o)
    block_until = -1e18
    last_valid_b = last_valid_t = None
    bhat_bin = np.zeros(nb + 8)
    reasons, valid_n, scale = {}, 0, None
    timeline = []
    for k in range(1, nb - 5):
        t_now = t0 + k + 1 + p.E3_DELAY_S
        if t_now >= next_lag_t:
            have = tr <= t_now
            m78 = th <= t_now
            est = estimate_lag(th[m78], raw78[m78], t_hat[have], r79[have], t_now, p)
            if est is not None:
                lag = est
            next_lag_t = t_now + p.LAG_EVERY_S
        if lag != cached_lag:
            kb = np.floor(t_hat - lag - t0).astype(int)
            order_k = np.argsort(kb, kind='stable')
            kb_sorted = kb[order_k]
            cached_lag = lag
        lo_i, hi_i = np.searchsorted(kb_sorted, k), np.searchsorted(kb_sorted, k + 1)
        cand = order_k[lo_i:hi_i]
        sel = np.zeros(tr.size, bool)
        sel[cand[tr[cand] <= t_now]] = True
        n79k = int(sel.sum())
        a = A[k] / n78[k] if n78[k] else None
        o = float((r79[sel] - ZERO79_NOMINAL).mean()) if n79k else None
        bad = bool(np.any(reset_frame[sel] & (kb[sel] == k)) or np.any(unf[sel]))
        st3 = bool(still_bin[k - 1] and still_bin[k] and still_bin[k + 1])
        if a is not None and o is not None and n78[k] >= p.N_MIN and n79k >= p.N_MIN and not bad:
            if st3:
                if z79 is not None and abs(o - (z79 - ZERO79_NOMINAL)) > p.STILL_FAULT:
                    block_until = max(block_until, t_now + p.STILL_FAULT_BLOCK_S)
                z79_samples.append(o)
                if len(z79_samples) >= p.Z79_MIN_BINS:
                    z79 = ZERO79_NOMINAL + float(np.median(z79_samples))
            usable.append((k, a, o, st3))
            usable = usable[-4 * p.SCALE_BINS:]
        reason = 'ok'
        b = None
        if z79 is None:
            reason = 'no_z79'
        else:
            zoff = z79 - ZERO79_NOMINAL
            win = [u for u in usable if u[0] > k - p.W]
            if len(win) < p.MIN_USABLE:
                reason = 'few_bins'
            else:
                r = []
                for u in win:
                    r += [u[1] - (u[2] - zoff)] * (int(p.STILL_WEIGHT) if u[3] else 1)
                r = np.sort(np.array(r))
                cut = int(math.floor(p.TRIM * r.size))
                rt = r[cut:r.size - cut] if r.size - 2 * cut > 0 else r
                b = float(rt.mean())
                rr = np.array([u[1] - (u[2] - zoff) for u in win])
                if float(rr.std()) > p.SD_MAX:
                    reason = 'sd'
                elif abs(b) > p.B_MAX:
                    reason = 'b_max'
            sw = [u for u in usable if u[0] > k - p.SCALE_BINS]
            if len(sw) >= p.MIN_USABLE:
                oc = np.array([u[2] - zoff for u in sw])
                if float(np.abs(oc).sum()) >= p.SCALE_ROT_MIN:
                    X = np.stack([np.ones_like(oc), oc], axis=1)
                    sol, *_ = np.linalg.lstsq(X, np.array([u[1] for u in sw]), rcond=None)
                    scale = float(sol[1])
                    if not (p.SCALE_LO <= scale <= p.SCALE_HI):
                        block_until = max(block_until, t_now + p.SCALE_BLOCK_S)
        if reason == 'ok' and t_now < block_until:
            reason = 'blocked'
        reasons[reason] = reasons.get(reason, 0) + 1
        timeline.append((t_now, reason))
        if reason == 'ok':
            valid_n += 1
            out = b
            last_valid_b, last_valid_t = b, t_now
        elif last_valid_t is None:
            out = 0.0
        else:
            age = t_now - last_valid_t
            out = last_valid_b if age <= p.HOLD_S else (
                last_valid_b * (1.0 - (age - p.HOLD_S) / p.RAMP_S) if age <= p.HOLD_S + p.RAMP_S else 0.0)
        idx_from = int(math.ceil(t_now - t0))
        if idx_from < bhat_bin.size:
            bhat_bin[idx_from:] = out
    kk = np.clip(np.floor(th - t0).astype(int), 0, bhat_bin.size - 1)
    return bhat_bin[kk], dict(reasons=reasons, valid_bins=valid_n, z79=z79, f=lag, scale=scale, timeline=timeline)


def heading_deg(raw, zero, mask):
    """Clockwise heading change (deg) per sample, 0 outside mask."""
    return np.where(mask, -(raw - zero) * DEG_PER_COUNT * PERIOD, 0.0)


# ----------------------------------------------------------------------------------------------
# Owner-like 0x078 zero model (counts), pre-registered
# ----------------------------------------------------------------------------------------------
def owner_bias(t, moving, rng):
    b = np.empty(t.size)
    level = rng.normal(-3.9, 1.5)
    t_step = t[0] + rng.uniform(0.0, 300.0)
    stepped = False
    dt = np.diff(t, prepend=t[0])
    for i in range(t.size):
        if not stepped and t[i] >= t_step:
            level += 2.6
            stepped = True
        if moving[i] and rng.random() < dt[i] / 90.0:
            level = -1.3 + 0.5 * (level + 1.3) + rng.normal(0.0, 2.86)
        b[i] = level
    return b


def staircase(t, moving, rng, rate=1.0 / 22.0):
    """Lattice-step difference n79 - n78 in {0..3}, changing only while moving."""
    s = np.empty(t.size, int)
    cur = int(rng.integers(0, 4))
    dt = np.diff(t, prepend=t[0])
    for i in range(t.size):
        if moving[i] and rng.random() < dt[i] * rate:
            cur = int(min(3, max(0, cur + rng.choice((-1, 1)))))
        s[i] = cur
    return s


# ----------------------------------------------------------------------------------------------
# Public log
# ----------------------------------------------------------------------------------------------
def load_public(path, check_hash=True):
    import csv
    with open(path, 'rb') as handle:
        data = handle.read()
    if check_hash and hashlib.sha256(data).hexdigest() != PUBLIC_SHA256:
        raise SystemExit('not the public ND log (sha256 differs)')
    f78, f79, f215 = [], [], []
    for row in csv.reader(data.decode('ascii').splitlines()[1:]):
        try:
            tt, addr, d = float(row[0]), int(row[1]), bytes.fromhex(row[3])
        except (ValueError, IndexError):
            continue
        if addr == 0x078 and len(d) >= 4:
            f78.append((tt, ((d[1] & 1) << 11) | (d[2] << 3) | (d[3] >> 5)))
        elif addr == 0x079 and len(d) >= 6 and not (d[3] >> 2):
            f79.append((tt, ((d[3] & 3) << 10) | (d[4] << 2) | (d[5] >> 6)))
        elif addr == 0x215 and len(d) >= 8:
            f215.append((tt, all(int.from_bytes(d[i:i + 2], 'big') == 10000 for i in (0, 2, 4, 6))))
    f78.sort(); f79.sort(); f215.sort()
    n = min(len(f78), len(f79))
    t = np.array([x[0] for x in f78[:n]])
    r78 = np.array([x[1] for x in f78[:n]], float)
    r79 = np.array([x[1] for x in f79[:n]], float)
    tw = np.array([x[0] for x in f215])
    zw = np.array([x[1] for x in f215], bool)
    idx = np.clip(np.searchsorted(tw, t) - 1, 0, tw.size - 1)
    still = zw[idx]
    return dict(t=t - t[0], raw78=r78, raw79=r79, still=still, valid=np.ones(n, bool))


# ----------------------------------------------------------------------------------------------
# Synthetic scenarios (production at 50 Hz, true rate in counts, counter-clockwise +, i.e. raw up)
# ----------------------------------------------------------------------------------------------
DPS_TO_COUNTS = 1.0 / DEG_PER_COUNT


def smooth(x, n=25):
    k = np.ones(n) / n
    return np.convolve(x, k, mode='same')


def garage(rng, obd_delay_after_departure=None):
    pre = 30.0
    total = 150.0
    target = rng.uniform(900.0, 1100.0) * rng.choice((-1, 1))
    t = np.arange(0.0, pre + total, PERIOD)
    w = np.zeros(t.size)
    still = t < pre
    stop_at = pre + rng.uniform(60.0, 90.0)
    cur = pre
    segs = []
    while cur < pre + total:
        d = rng.uniform(4.0, 10.0)
        segs.append((cur, min(cur + d, pre + total), rng.uniform(12.0, 25.0)))
        cur += d + rng.uniform(2.0, 6.0)
    for a, b, r in segs:
        w[(t >= a) & (t < b)] = r
    stopm = (t >= stop_at) & (t < stop_at + 5.0)
    w[stopm] = 0.0
    w = smooth(w)
    w[still | stopm] = 0.0
    w *= abs(target) / max(np.abs(w).sum() * PERIOD, 1e-9)
    w *= math.copysign(1.0, target)
    still = still | stopm
    obd_ok = np.ones(t.size, bool) if obd_delay_after_departure is None else t >= pre + obd_delay_after_departure
    eval_t = [(pre, pre + x) for x in (60.0, 90.0, 120.0, 150.0)]
    return t, w * DPS_TO_COUNTS, still, obd_ok, eval_t


def tunnel(rng):
    t = np.arange(0.0, 230.0, PERIOD)
    still = ((t >= 0.0) & (t < 10.0)) | ((t >= 60.0) & (t < 70.0))
    w = np.zeros(t.size)
    cur = 10.0
    while cur < 120.0:
        d = rng.uniform(5.0, 15.0)
        m = (t >= cur) & (t < cur + d)
        w[m] = rng.uniform(-8.0, 8.0)
        cur += d + rng.uniform(2.0, 10.0)
    tun = (t >= 120.0)
    rot = rng.uniform(30.0, 180.0)
    cur = 120.0
    wt = np.zeros(t.size)
    while cur < 230.0:
        d = rng.uniform(8.0, 20.0)
        m = (t >= cur) & (t < cur + d)
        wt[m] = rng.uniform(-1.0, 1.0)
        cur += d + rng.uniform(3.0, 12.0)
    if np.abs(wt[tun]).sum() > 0:
        wt *= rot / (np.abs(wt[tun]).sum() * PERIOD)
    w = np.where(tun, wt, w)
    w = smooth(w)
    w[still] = 0.0
    return t, w * DPS_TO_COUNTS, still, np.ones(t.size, bool), [(120.0, 230.0)]


def synth_signals(t, w_counts, still, rng, z79=None, with_owner=True, with_stairs=True, scale79=1.0):
    if z79 is None:
        z79 = rng.uniform(2047.4, 2047.8)
    moving = ~still
    x79 = (w_counts * scale79 + z79 - 2047.00190) / 1.52751423
    n79 = np.round(x79 + rng.normal(0.0, 0.3, t.size))
    raw79 = lattice(n79)
    nt = np.round((w_counts + z79 - 2047.00190) / 1.52751423 + rng.normal(0.0, 0.3, t.size))
    stairs = staircase(t, moving, rng) if with_stairs else np.zeros(t.size, int)
    raw78 = lattice(nt - stairs)
    ob = owner_bias(t, moving, rng) if with_owner else np.zeros(t.size)
    raw78 = np.clip(np.round(raw78 + ob), 1, 4094)
    true_counts = w_counts      # truth rate in counts (ccw +)
    return raw78, raw79, true_counts, z79


# ----------------------------------------------------------------------------------------------
# Evaluation
# ----------------------------------------------------------------------------------------------
def window_errors(t, h_corr, h_unc, h_true, rot_true, windows):
    c = np.concatenate([[0.0], np.cumsum(h_corr)])
    u = np.concatenate([[0.0], np.cumsum(h_unc)])
    g = np.concatenate([[0.0], np.cumsum(h_true)])
    r = np.concatenate([[0.0], np.cumsum(rot_true)])
    out = []
    for a, b in windows:
        i, j = np.searchsorted(t, a), np.searchsorted(t, b)
        out.append(dict(t0=float(a), T=float(b - a), e_corr=float((c[j] - c[i]) - (g[j] - g[i])),
                        e_unc=float((u[j] - u[i]) - (g[j] - g[i])), rot=float(r[j] - r[i])))
    return out


def public_windows(t):
    end = t[-1]
    out = []
    for T in (60, 90, 120, 180, 240, 300):
        s = 0.0
        while s + T <= end:
            out.append((s, s + T))
            s += 10.0
    return out


def one_public_run(pub, rng, owner=False, fault=None, kind='E1'):
    t, r78, r79, still = pub['t'], pub['raw78'].copy(), pub['raw79'].copy(), pub['still']
    z79_true = float(np.mean(r79[still]))
    if owner:
        r78 = np.round(r78 + owner_bias(t, ~still, rng))
    tr, info = fault_delivery(t, rng, fault)
    r79o = fault_signal79(t, r79, still, rng, fault, z79_true)
    unf = None
    if kind in ('E2', 'E3'):
        r79o, tr, flag = fill_losses(t, r79o, tr, fill=(fault != 'F4u'))
        unf = flag if fault == 'F4u' else None
    bh, diag = run_estimator(t, r78, r79o, still, tr, kind=kind, unfilled=unf)
    mask = np.ones(t.size, bool)
    h_true = heading_deg(r79, z79_true, mask)
    h_unc = heading_deg(r78, ZERO78, mask)
    h_corr = heading_deg(r78 - bh, ZERO78, mask)
    rot = np.abs(h_true)
    wins = window_errors(t, h_corr, h_unc, h_true, rot, public_windows(t))
    return wins, diag, info


def fault_delivery(t, rng, fault):
    kw = {}
    if fault == 'F1':
        a = rng.uniform(t[0] + 30.0, max(t[0] + 31.0, t[-1] - 60.0))
        kw['dropouts'] = [(a, a + 60.0)]
    elif fault == 'F3':
        kw.update(L=1.0, jitter_sd=0.3, P=0.5)
    elif fault in ('F4', 'F4u'):
        kw['loss'] = 0.10
        drops = []
        for _ in range(3):
            a = rng.uniform(t[0], t[-1]); drops.append((a, a + rng.uniform(1.0, 3.0)))
        kw['dropouts'] = drops
    elif fault == 'F6':
        kw['shift'] = 1.5
    tr, L, P = deliver(t, rng, **kw)
    return tr, dict(L=L, P=P, fault=fault)


FAULT_AT = [None]


def fault_signal79(t, r79, still, rng, fault, z79_ref, scale=None):
    r = r79.copy()
    if fault in ('F2a', 'F2b'):
        s = 1.03 if fault == 'F2a' else 1.08
        r = np.round(z79_ref + s * (r79 - z79_ref))
    elif fault == 'F5a':
        mv = np.where(~still)[0]
        i = mv[int(rng.integers(0, mv.size))]
        r[i:] = r[i:] + 5.0
        FAULT_AT[0] = t[i]
    elif fault == 'F5b':
        mv = np.where(~still)[0]
        i = mv[int(rng.integers(0, mv.size))]
        j = np.searchsorted(t, t[i] + 30.0)
        r[i:j] = r[i]
    return r


def one_synth_run(kind, rng, fault=None, obd_delay=None, owner=True, stairs=True, est='E1'):
    if kind == 'garage':
        t, w, still, obd_ok, wins = garage(rng, obd_delay)
    else:
        t, w, still, obd_ok, wins = tunnel(rng)
    s79 = {'F2a': 1.03, 'F2b': 1.08}.get(fault, 1.0)
    r78, r79, true_c, z79 = synth_signals(t, w, still, rng, with_owner=owner, with_stairs=stairs, scale79=s79)
    if fault in ('F5a', 'F5b'):
        r79 = fault_signal79(t, r79, still, rng, fault, z79)
    tr, info = fault_delivery(t, rng, fault if fault not in ('F2a', 'F2b', 'F5a', 'F5b') else None)
    tr = np.where(obd_ok, tr, np.nan)
    unf = None
    if est in ('E2', 'E3'):
        r79, tr, flag = fill_losses(t, r79, tr, fill=(fault != 'F4u'))
        unf = flag if fault == 'F4u' else None
    bh, diag = run_estimator(t, r78, r79, still, tr, kind=est, unfilled=unf)
    mask = np.ones(t.size, bool)
    h_true = -true_c * DEG_PER_COUNT * PERIOD
    h_unc = heading_deg(r78, ZERO78, mask)
    h_corr = heading_deg(r78 - bh, ZERO78, mask)
    return window_errors(t, h_corr, h_unc, h_true, np.abs(h_true), wins), diag, info


def stats(wins):
    ec = np.array([abs(w['e_corr']) for w in wins])
    eu = np.array([abs(w['e_unc']) for w in wins])
    if not ec.size:
        return dict(n=0)
    worse = ec - eu
    T = np.array([w['T'] for w in wins])
    rot = np.array([abs(w['rot']) for w in wins])
    rate = np.maximum(0.0, ec * math.pi / 180.0 - 0.02 * rot * math.pi / 180.0) / T
    rate_u = np.maximum(0.0, eu * math.pi / 180.0 - 0.02 * rot * math.pi / 180.0) / T
    return dict(n=int(ec.size), corr_med=float(np.median(ec)), corr_p90=float(np.percentile(ec, 90)),
                unc_med=float(np.median(eu)), unc_p90=float(np.percentile(eu, 90)),
                corr_max=float(ec.max()), unc_max=float(eu.max()),
                ratio_p90=float(np.percentile(ec, 90) / max(np.percentile(eu, 90), 1e-9)),
                worse_2deg_share=float(np.mean(worse > 2.0)), worse_max=float(worse.max()),
                k_p97=float(np.percentile(rate, 97)), k_max=float(rate.max()),
                k_unc_p97=float(np.percentile(rate_u, 97)))


def evaluate(pub, runs, seed, est='E1'):
    rng = np.random.default_rng(seed)
    res = {}
    diag_valid = {}

    def collect(name, fn, n):
        wins, vb, nb, reasons = [], 0, 0, {}
        for _ in range(n):
            w, d, _ = fn()
            wins += w
            vb += d['valid_bins']; nb += sum(d['reasons'].values())
            for k, v in d['reasons'].items():
                reasons[k] = reasons.get(k, 0) + v
        res[name] = stats(wins)
        res[name]['valid_share'] = vb / max(nb, 1)
        res[name]['reasons'] = reasons
        res[name]['_wins'] = wins

    if pub is not None:
        collect('D1', lambda: one_public_run(pub, rng, kind=est), max(10, runs // 4))
        collect('D2', lambda: one_public_run(pub, rng, owner=True, kind=est), runs)
    collect('D3', lambda: one_synth_run('garage', rng, est=est), runs)
    collect('D3b', lambda: one_synth_run('garage', rng, obd_delay=40.0, est=est), runs)
    collect('D4', lambda: one_synth_run('tunnel', rng, est=est), runs)
    nf = max(10, runs // 2)
    faults = ('F1', 'F2a', 'F2b', 'F3', 'F4', 'F6', 'F5a', 'F5b') + (('F4u',) if est in ('E2', 'E3') else ())
    for f in faults:
        if pub is not None:
            collect('D2_' + f, lambda f=f: one_public_run(pub, rng, owner=True, fault=f, kind=est), nf)
        collect('D4_' + f, lambda f=f: one_synth_run('tunnel', rng, fault=f, est=est), nf)
    # exploratory (not in the rule): no staircase in synthetic 0x078
    collect('X_D3_nostairs', lambda: one_synth_run('garage', rng, stairs=False, est=est), max(10, runs // 2))
    collect('X_D4_nostairs', lambda: one_synth_run('tunnel', rng, stairs=False, est=est), max(10, runs // 2))
    d3_150 = [w for w in res['D3']['_wins'] if w['T'] == 150.0]
    res['D3_150'] = stats(d3_150)
    return res


def judge(res):
    out = {}
    have_pub = 'D1' in res
    r0 = res['D3']['n'] >= 4 * 200 and res['D4']['n'] >= 200
    if have_pub:
        d1_distinct = len({(w['t0'], w['T']) for w in res['D1']['_wins']})
        r0 = r0 and d1_distinct >= 20 and res['D2']['n'] > 0
        out['D1_distinct_windows'] = d1_distinct
    out['R0'] = bool(r0)
    r1 = res['D3_150']['ratio_p90'] <= 0.4 and res['D4']['ratio_p90'] <= 0.4
    if have_pub:
        r1 = r1 and res['D2']['ratio_p90'] <= 0.4 and \
            res['D1']['corr_p90'] <= max(0.6 * res['D1']['unc_p90'], 1.0)
    out['R1'] = bool(r1)
    names = (['D1', 'D2'] if have_pub else []) + ['D3', 'D3b', 'D4']
    out['R2'] = bool(all(res[n]['worse_2deg_share'] <= 0.05 and res[n]['worse_max'] <= 5.0 for n in names))
    out['R2_detail'] = {n: (res[n]['worse_2deg_share'], res[n]['worse_max']) for n in names}
    r3 = {}
    for f in ('F1', 'F2a', 'F2b', 'F3', 'F4', 'F6', 'F4u'):
        if 'D4_' + f not in res:
            continue
        for base in (['D2'] if have_pub else []) + ['D4']:
            s = res[base + '_' + f]
            r3[base + '_' + f] = bool(s['corr_p90'] <= 1.10 * s['unc_p90'])
    out['R3'] = bool(all(r3.values()))
    out['R3_detail'] = r3
    pool = (res['D2']['_wins'] if have_pub else []) + res['D3']['_wins'] + res['D4']['_wins']
    s = stats(pool)
    k = math.ceil(s['k_p97'] / 0.0001 - 1e-9) * 0.0001
    out['k_obd'] = round(k, 4)
    out['k_obd_max'] = s['k_max']
    out['k_unc_p97'] = s['k_unc_p97']
    out['R4'] = bool(k <= 0.0010 + 1e-12)
    out['PASS'] = bool(out['R0'] and out['R1'] and out['R2'] and out['R3'] and out['R4'])
    return out


def strip(res):
    return {k: {kk: vv for kk, vv in v.items() if not kk.startswith('_')} for k, v in res.items()}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('evaluate')
    e.add_argument('--csv', required=True)
    e.add_argument('--runs', type=int, default=200)
    e.add_argument('--seed', type=int, default=1)
    e.add_argument('--json')
    e.add_argument('--estimator', choices=('E1', 'E2', 'E3'), default='E1')
    s = sub.add_parser('synthetic')
    s.add_argument('--runs', type=int, default=200)
    s.add_argument('--seed', type=int, default=1)
    s.add_argument('--json')
    s.add_argument('--estimator', choices=('E1', 'E2', 'E3'), default='E1')
    a = ap.parse_args(argv)
    pub = load_public(a.csv) if a.cmd == 'evaluate' else None
    res = evaluate(pub, a.runs, a.seed, est=a.estimator)
    verdict = judge(res)
    pc = {'E2': P2, 'E3': P3}.get(a.estimator, Params)
    doc = dict(estimator=a.estimator, params={k: getattr(pc, k) for k in dir(pc) if k.isupper()},
               runs=a.runs, seed=a.seed, results=strip(res), verdict=verdict)
    text = json.dumps(doc, indent=1, sort_keys=True)
    if a.json:
        open(a.json, 'w').write(text)
    print(json.dumps(verdict, indent=1, sort_keys=True))
    for k, v in strip(res).items():
        if v.get('n'):
            print('%-16s n=%5d corr med/p90 %6.2f/%6.2f  unc med/p90 %6.2f/%6.2f  ratio %.3f  worse>2 %.3f  worse_max %6.2f  valid %.2f'
                  % (k, v['n'], v['corr_med'], v['corr_p90'], v['unc_med'], v['unc_p90'], v['ratio_p90'],
                     v['worse_2deg_share'], v['worse_max'], v.get('valid_share', float('nan'))))
    return 0 if verdict['PASS'] else 1


if __name__ == '__main__':
    sys.exit(main())
