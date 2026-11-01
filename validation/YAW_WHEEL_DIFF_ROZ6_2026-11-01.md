# Yaw wheel-diff ε / fusion — roz6 extension (2026-11-01)

Exploratory, MODEL-only, no product change. Extends the 20261010 study
(private-data/yaw-analysis-20261010) onto the fresh roz6 vehicle drive, using the
**exact** legacy machinery (`legacy_eval`, `variant_eval`, `gps_straight_eps`
logic) so numbers are comparable. A roz6 loader mirrors `legacy_eval.load()`
per boot (roz6 is one multi-boot export). GPS is used only as the held-out
straightness / edge check, never as accuracy truth. No coordinates, place names,
OSM IDs, raw bytes or identifiers appear here.

Method recap: on a GPS-straight (|Δcourse|<2° between consecutive ≥18 km/h fixes
≤5 s apart) the true yaw ≈ 0, so the front wheel-pair diff `d01 ≈ ε·v`; fit
`ε = Σ(d01·v)/Σ(v²)` over moving coasting samples (v = wheel speed, |accel|<0.5).
K = 0.03773585982400871 (0x078), SLOPE_FRONT = −41.66, τ = 10 s.

## 1. ε from roz6 GPS-straight segments

Per-boot sample-weighted ε (the actual estimator), driving boots only:

| boot | fixes | straight samples | segments | ε (%) | per-seg SEM (%) | seg std (%) |
|------|------:|-----------------:|---------:|------:|----------------:|------------:|
| 4caa793f | 207 | 328 | 39 | **0.074** | 0.070 | 0.435 |
| 8973091e | 480 | 1041 | 118 | **0.083** | 0.182 | 1.974 |
| fd8f8029 | 144 | 75 | 8 | −0.274 | 0.285 | 0.807 |
| 75875537 / 08375179 / 582468e0 | — | 0 | 0 | n/a | — | — |

- The two boots with real highway-straight coverage (4caa793f, 8973091e)
  **agree at ε ≈ 0.07–0.08 %**. fd8f8029 (only 8 segments, ε = −0.27 %) is
  unreliable and discarded. Three boots had no qualifying straight segment.
- roz6 pooled (mean of 165 segments): ε = 0.19 %, SEM 0.13 %, but std 1.69 % —
  the mean is inflated by fd8f8029 and 8973091e's wide spread; the robust
  per-boot estimator (~0.08 %) is the fair figure.

Comparison to 20261010 (same estimator):

| set | ε (%) | n seg | pooled SEM (%) |
|-----|------:|------:|---------------:|
| old trip2 | 0.043 | 120 | 0.034 |
| old trip4 | 0.0105 | 39 | 0.034 |
| old pooled | **0.025** | 159 | **0.027** |
| roz6 pooled | 0.19 (robust ~0.08) | 165 | 0.13 |
| roz6 + old pooled | 0.11 | 324 | 0.069 |

- roz6 vs old (pooled means): |Δε| = 0.168 %, combined SEM 0.135 %, **z = 1.24**
  → statistically consistent (not a significant disagreement), but only because
  roz6's SEM is large. roz6's central value runs **higher** than old (≈0.08 %
  vs 0.025 %).

**Precision crux (the whole question): still not met, and roz6 confirms it.**
Prior finding: per-drive SEM ≈ ±0.034 %, need ≈ ±0.01 %. roz6's best single
drive is ±0.07 %; pooled roz6 ±0.13 %; **pooling all drives (roz6 + old) gives
±0.069 %** — still ~7× short of ±0.01 %. Adding roz6 did **not** tighten the
pooled estimate below the old-alone SEM (0.027 % → 0.069 %), because roz6's
segments carry much higher variance. roz6 is lower-quality for ε estimation than
the old trio.

## 2. (Superseded) Map-tangent fusion path — not the right oracle

An initial attempt scored fusion against the OSM map-tangent oracle and found 0
uniquely-matched tunnels in roz6 (its region is outside the tunnel DB), and I
wrongly reported "fusion could not be computed." That conflated two oracles. The
prior ~2.17°/~8° result rests on the **GPS-free integration-zero oracle**, which
needs no map. The correct, map-free computation is in the Measured-fusion section
below; the OSM path is dropped. (Kept as a note so the correction is traceable.)

## 3. Verdict (superseded by the measured verdict at the end).

- **Confirms** the prior precision crux: even pooling roz6 with the old three
  trips leaves ε uncertain at ±0.069 %, an order of magnitude above the ±0.01 %
  a safe fixed ε would need. A handful of drives does not get there; the "~1000+
  accumulated highway-straight segments + confidence guard" requirement stands.
- **New caution (mild weakening):** roz6's usable drives center ε ≈ 0.08 %,
  ~3× the old pooled 0.025 %. Formally consistent (z = 1.24) only thanks to
  roz6's large SEM. If per-drive ε truly varies this much, a single fixed global
  ε is riskier than the old trio implied — directly relevant to "wrong ε is
  worse than none."
- **Fusion lever:** see the measured section below (map-free). The ε-precision
  and ε-stability bullets above stand regardless.

(§1 results file: scratchpad/roz6-eps-results.json; script: scratchpad/roz6_yaw.py.
Data stayed in the isolated roz6 dir; nothing committed or pushed.)

---

## § Measured fusion (GPS-free yaw-zero oracle + GPS-return differential)

Map-free, as the original study intended. Same observer (bias observer /
complementary filter, τ = 10 s), SLOPE_FRONT = −41.66, K = 0.03773585982400871.
GPS course / GPS re-acquisition is the held-out differential reference only,
never accuracy ground truth. ε cases: ε0 = 0, roz6-ε = 0.0766 %
(sample-weighted over roz6 straight samples), pooled-ε = 0.0601 % (roz6 + old).
(Script: scratchpad/roz6_fusion.py; results: scratchpad/roz6-fusion-results.json.)

### A. Yaw-zero vs the integration-zero oracle — the ~8° regime is ABSENT in roz6

Over roz6's short driving windows the **fixed z = 2048 baseline is already
~1.9–2.8° median** heading RMS vs GPS course, with the per-window integration-zero
oracle at ~0.6–2°. There is essentially **no ~8° gap for ε to close**. Reason:
the prior ~2.17° vs ~8° contrast arises only over a **long, steady GPS-lost DR
window** where a drifted zero integrates into large heading error. roz6 contains
no such window — its longest GPS loss is 36 s (most 2–18 s). The product's own
166 driving zero-fits do show the zero wandering ~2040–2058 counts
(≈ ±0.38 deg/s), so the *potential* for an ~8° error over a 60 s tunnel exists
(≈ K·10·60 ≈ 23° peak / ~8° RMS), but roz6 never realises it. A clean constant-
zero fit is also hard to isolate on roz6's windows (gentle windows leave the zero
unobservable; sharp-turn windows carry lane-change / integer-course noise), so the
RMS-vs-oracle number itself is not cleanly reproducible here — not because of
OSM, but because the drive lacks the steady-DR regime the metric needs.

### B. GPS-return DR differential — ε benefit is MIXED / not robust

Replayed DR over the product's 5 real GPS-loss intervals (gps_lost → gps_returned),
anchored at the last pre-loss valid fix, endpoint vs the first re-acquired fix.
Delta = |DR endpoint − GPS re-acquisition| (m); ε enters only through the fused
heading. (This replay uses mean wheel speed + the observer heading, a cruder DR
than the product core, so absolute metres run larger than the product's own
as-shipped 6–20 m; the **relative** ε comparison is the signal.)

| loss (s) | covered (m) | ε0 | roz6-ε | pooled-ε |
|---------:|------------:|----:|-------:|---------:|
| 7  | 123 | 12.9 | 8.2  | 9.3  |
| 36 | 664 | 89.1 | 28.7 | **4.8** |
| 3 (17 s anchor) | 290 | 6.7 | 36.4 | 28.8 |
| 18 | 374 | 11.5 | 47.5 | 39.6 |
| 14 | 323 | 29.2 | 11.1 | 13.6 |
| **median** | | **12.9** | 28.7 | 13.6 |
| **mean** | | 29.9 | 26.4 | 19.2 |

- ε-calibration **helps 3 of 5** intervals and **dramatically helps the single
  longest dropout** (36 s / 664 m: 89 → 5 m with pooled-ε), but **hurts two
  shorter ones** (18 s: 11.5 → 47.5; 3 s/17 s: 6.7 → 36.4).
- **Median delta does not improve** (ε0 12.9 → roz6-ε 28.7 / pooled-ε 13.6);
  the mean only improves because the one long-dropout outlier shrinks. Net
  benefit is **not robust** — the measured "wrong ε is worse than none"
  signature, now on independent data.

### Measured verdict: NEUTRAL → WEAKENING. No product change.

On this independent, measured drive ε is **not** demonstrated as a reliable
lever. The yaw-zero fusion benefit (the ~2.17°/~8° headline) **cannot be
exhibited on roz6 because the long-steady-DR regime that produces the 8° stock
error never occurs** — over roz6's short losses the uncalibrated fixed zero is
already ~2°. Where ε could be tested at all — the measured GPS-returns — its
effect is **mixed and net-unhelpful on the median**, helping only the longest
dropout and worsening shorter ones. Combined with §1 (per-drive ε noisy, pooled
precision still ~7× short of the needed ±0.01 %, central value ~3× the old
estimate), roz6 **weakens** confidence in shipping a fixed ε and provides **no**
basis for a product change. The pre-registered-safety-gate bar is not met.
Standing guidance: keep accumulating clean highway-straight segments across
drives; add the per-drive ε-stability spread to the guard criteria; and a future
export containing a genuine long GPS-lost tunnel is needed to test the
fusion-vs-oracle benefit directly.

---

## § GPS-holdout long-DR drift (no tunnel needed)

Manufacture the long-DR regime: take long CONTINUOUS GPS-valid driving windows,
withhold GPS from the DR, dead-reckon across the window, and compare to the
HELD-OUT GPS track (standard SHADOW-calibration holdout; GPS is a held-out
differential reference only, never accuracy ground truth). Heading error is the
clean ε-sensitive metric; position endpoint is reported too but is confounded by
wheel-speed scale (common-mode across variants) and turn geometry. Observer runs
GPS-free over the whole boot (warm), seed heading/position from the window-start
fix. Variants: fixed2048 (no fusion), fused ε0 / roz6-ε / pooled-ε, oracle
(best constant zero fit to the held-out course = lower bound). Script:
scratchpad/roz6_holdout.py.

### 1. Window census — the long regime barely exists on these drives

GPS fix rate ~1 Hz, heading integer degrees (≈0.5–1° truth-resolution floor).
Continuous GPS-valid highway windows (valid fixes ≥15 km/h, gaps ≤2.5 s, grid
good ≥0.9):

| drive | ≥30 s | ≥60 s | ≥90 s | curve ≥60 s (ptp≥20°) |
|-------|------:|------:|------:|----------------------:|
| roz6 (all boots) | 3 | **0** | 0 | 0 |
| old trip2 | 3 | 1 | 0 | — |
| old trip4 | 0 | 0 | 0 | — |

roz6 has **no** continuous GPS-valid window ≥60 s (longest 54 s); even the old
exports yield only one (trip2). These drives are city/mixed, not sustained
highway, so the 60 s / 90 s horizons are essentially unreachable — a real
selection limit, not a tuning choice. The 3 roz6 windows that qualify are curvy
(course span 34–70°: ramps/interchanges), straining the constant-zero assumption.

### 2. Holdout DR drift (heading error, the clean metric)

roz6, 30 s horizon (n = 3 windows; median deg):

| variant | heading err (deg) | note |
|---------|------------------:|------|
| oracle (best constant zero) | 0.8 | lower bound |
| **fixed 2048 (no fusion)** | **2.3** | already good |
| fused pooled-ε (0.060 %) | 4.1 | |
| fused roz6-ε (0.077 %) | 6.6 | |
| fused ε0 (uncalibrated) | 7.8 | worst |

old trip2, the single 60 s window (n = 1; median deg): oracle 1.2, **fixed 2048
1.2**, fused pooled-ε 1.6, fused roz6-ε 5.6, fused ε0 **12.9**.

(Endpoint position, roz6 n=3 median m — confounded, directional only: fixed 17.0,
oracle 35.6, fused ε0 66.6, roz6-ε 75.1, pooled-ε 62.4.)

### 3. What this shows

- **ε is strongly confirmed as the lever ON THE FUSION.** Uncalibrated fusion is
  badly wrong (ε0 heading 7.8°@30 s on roz6, 12.9°@60 s on trip2); ε-calibration
  cuts that to 4.1°/1.6° (pooled-ε), approaching the oracle. More ε (within the
  fitted range) monotonically reduced the fusion's heading error. This is the
  clearest independent evidence yet that ε drives fusion accuracy.
- **But fusion did not beat the no-fusion fixed-2048 baseline on any window
  here.** Every qualifying window happened to have a **near-nominal gyro zero**
  (oracle zeros 2042.6 / 2050.6 / 2050.7 on roz6, 2049.1 on trip2 — within ~1–3
  counts of 2048), so fixed-2048 was already ~1–2° and the wheel reference's own
  residual error (ε miscalibration + noise) exceeded the tiny gyro offset it was
  meant to correct. The ~8° stock regime (badly drifted zero), where fusion/ε
  previously reached oracle, **is not represented in any long GPS-valid window
  of either dataset.**

### 4. Caveats

Tiny n (roz6 3 windows @30 s, trip2 1 @60 s; 0 @90 s anywhere) — directional
only. 1 Hz / integer-degree GPS caps resolution and makes 90 s unreachable from
the short continuous stretches. Selection bias: only curvy ramp/interchange
windows qualify (the steady highway that would give a clean constant zero is also
where turning is least, so the zero is weakly observable there). Position
endpoint is wheel-speed-scale-confounded. All qualifying windows had near-nominal
zeros, so this cannot test the drifted-zero regime where fusion is supposed to
win.

### Holdout verdict: ε-lever STRENGTHENED; fusion-as-product WEAKENED.

The holdout confirms **ε is the dominant lever on fusion heading accuracy**
(ε0 → ε-calibrated cuts fusion error ~3–8×, toward oracle) — the strongest
independent support so far for that specific claim. But it simultaneously shows
**fusion is not worth shipping unconditionally**: on every long GPS-valid window
available (roz6 + old), the gyro zero was near-nominal, fixed-2048 already
achieved ~1–2°, and fusion — even with the best ε — only matched it at best and
underperformed at worst, while uncalibrated fusion was far worse. Net for a
product decision: **no change.** Fusion+ε pays off only when the zero is badly
drifted (the earlier tunnel regime), a state absent from all long windows here;
shipping it unconditionally risks degrading the common near-nominal case, and a
wrong/averaged ε clearly worsens it. To decide for real we need a drive that
contains BOTH a genuinely drifted zero AND long continuous windows — neither
roz6 nor the 20261010 exports provide that combination.

---

## § Gyro-zero drift characterization (gate feasibility)

Does the gyro zero drift enough, often enough, to justify designing a "fuse only
when the zero is drifted" gate? GPS-free throughout: standstill (ZUPT) zeros and
wheel/yaw only. roz6 uses the product's 151 `yaw_stop` digests (offset counts =
`ys/yn`, matches the human-readable table); the old exports (no `yaw_stop`) use a
uniform Grid ZUPT scan (all-wheel-zero 2 s windows, `ptp≤4`, `var≤1`,
zero = mean raw − 2048). Conversion: **K = 0.037736 deg/s per count**, so the
uncorrected integrated heading error over T s = K·|offset|·T; the "drifted"
threshold |offset| for 3°/60 s is **1.32 counts**, for 5°/60 s **2.21 counts**.
Script: scratchpad/roz6_drift.py.

### 1. Standstill-zero distribution (offset from 2048, counts)

| drive | n stops | median | mean | std | p10/p90 | >3c | >5c | >10c |
|-------|--------:|-------:|-----:|----:|---------|----:|----:|-----:|
| roz6 | 151 | −0.92 | −0.28 | 2.71 | −3.1 / +3.5 | 36 (24%) | 11 (7%) | 0 |
| trip2 | 461 | +0.64 | +0.18 | 1.20 | −2.7 / +0.9 | 0 | 0 | 0 |
| trip3 | 157 | +2.0 | −2.7 | 6.29 | −10.4 / +2.7 | 79 | 71 | **62** |
| trip4 | 96 | −1.05 | +0.9 | 2.45 | −1.4 / +3.8 | 41 | 0 | 0 |

Uncorrected heading error if you trust fixed 2048 (roz6): from the median-abs
offset (1.31 c) **1.5 / 3.0 / 4.5°** at 30/60/90 s; from the p90-abs offset
(4.06 c) **4.6 / 9.2 / 13.8°**. So the stock zero is typically only ~3°/60 s off,
reaching the ~8–9° regime only at the p90 tail.

**The zero is near-nominal on 3 of 4 drives (roz6, trip2, trip4: std ≤2.7 c, no
stop beyond ~5 c) and strongly drifted on exactly one (trip3: 62 stops beyond 10
counts, p10 −10.4 c ≈ 24°/60 s).** The badly-drifted regime is real but
occasional — a per-drive condition, not the norm.

### 2. Within-drive drift (stop-to-stop, roz6, 144 consecutive pairs)

- Zero movement between consecutive stops: **median |jump| 1.79 c, p90 5.55 c**;
  drift rate **median 1.24 c/min, p90 5.45 c/min** (≈ 2.8°/60 s typical, 12°/60 s
  at the p90 on a stop-free stretch).
- 103/144 pairs (72 %) jump more than the 3°/60 s threshold (1.32 c), 61/144
  (42 %) more than the 5°/60 s threshold — but each standstill carries ~0.6 c
  measurement noise (≈0.85 c per difference), which inflates the small-jump end.
- Takeaway: **the zero does move during a drive, not just boot-to-boot** — so a
  one-time standstill zero goes stale over minutes. On roz6, though, stops are
  frequent (151), so the zero is re-anchored often and staleness stays small
  unless a stretch is long and stop-free.

### 3. Base rate of the drifted regime

By the (small) 3°/60 s criterion, **50 % of roz6 standstills and 72 % of
stop-to-stop pairs** qualify — but that threshold is only ~1.3 counts, near the
measurement noise. By a more meaningful **5°/60 s** bar, **38 %** of standstills
and 42 % of pairs; by |offset| > 10 c (≈23°/60 s, the genuine "stock fails"
regime), **0 %** of roz6 and trip2/trip4 stops, versus **40 %** of trip3's. So:
occasional at the 5° level, essentially absent at the 10° level except on one
drive.

### 4. Worth-it read: a fusion gate is MARGINAL — try ZUPT correction first

- **(a) Does it drift enough, often enough?** Modestly and occasionally. The
  big-drift regime where fusion clearly wins (≥10 c, ~20°+/60 s) appeared in 1 of
  4 drives (trip3) and 0 % of roz6/trip2/trip4 stops. Typical uncorrected error
  is ~3°/60 s, tail ~9°/60 s.
- **(b) Detectable from the standstill zero alone?** Partly. The zero moves
  mid-drive (median 1.2 c/min), so a stale standstill zero drifts ~2.8°/60 s on a
  stop-free minute; tracking that mid-drive needs a running residual (the fusion).
  But stops are frequent, so periodic **ZUPT re-anchoring** (use the last
  standstill zero, GPS-free, **no ε, no wheel-diff**) already removes the per-boot
  offset and most slow drift — the part fusion adds is only the between-stop
  residual on long stop-free stretches.
- **(c) Confounds:** standstill noise (~0.6 c) inflates small jumps; a cold-start
  warm-up trend is not separated from genuine drift (trip3's spread may be a
  warm-up transient or a single drifted session — unresolved); the "offset from
  2048" mixes a stable per-boot bias (cheaply correctable by ZUPT) with real
  drift. Standstill-zero reliability itself depends on true zero-speed (crawl
  contaminates it).

**Read: a drifted-zero-gated fusion is not clearly worth designing yet.** The
drift that fusion *uniquely* addresses (mid-drive, between stops) is small
(~2.8°/60 s typical), the ≥10 c regime is rare (1/4 drives), and fusion's benefit
is gated behind an ε we cannot calibrate to the needed ±0.01 % and which actively
harms the common near-nominal case (§ holdout). The cheaper, ε-free lever —
**ZUPT standstill-zero correction** — already covers the per-boot offset and slow
drift because stops are frequent; it should be characterized/evaluated first.
Fusion+ε would earn its place only for drives with **long stop-free stretches AND
a genuinely drifting zero** (e.g. a trip3-like session through a long tunnel) —
a combination none of the current exports contain. Recommend: Step 2 = evaluate
ZUPT re-anchoring drift coverage, not a fusion gate.

---

## § ZUPT re-anchor coverage (ε-free lever)

Characterizes whether holding the last standstill zero (ε-free) beats fixed-2048.
Per stop-to-stop driving segment, three estimators are seeded at the segment-start
GPS course and integrated, then scored by heading error vs the held-out GPS course
(reference only, not ground truth): **A** fixed-2048, **B** ZUPT (hold last
standstill zero), **C** ZUPT + linear drift extrapolation (project the zero by the
recent stop-to-stop rate). Oracle = best constant zero fit to the course
(lower bound). Zeros come from wheel+yaw standstills only — no GPS, no ε, no
wheel-diff. Script: scratchpad/roz6_zupt.py.

### Scorability — the decisive data gap

Only **5 stop-to-stop segments across all exports are scorable**, and **all are
near-nominal-zero** (trip2 ×4, roz6 ×1). Critically, **trip3 — the one drifted-
zero drive, with the long stop-free segments (51, 100, 200 s) this test most
needs — has ZERO valid GPS course** (fixes never qualify), so it cannot be scored
at all. The regime where ZUPT (or fusion) is supposed to help is therefore
**unmeasurable on existing data**, same blocker as the holdout section.

### Results on the scorable (near-nominal) segments

Pooled heading RMS (n = 5): **A 2.2° = oracle 2.2°**, **B 6.8°**, **C 7.4°**.
Fixed-2048 already equals the oracle; ZUPT and drift-extrapolation are markedly
**worse**.

Heading error vs time since last stop (median deg; n_fixes per bucket 52–61):

| bucket | A fixed | B ZUPT | C ZUPT+drift | oracle |
|--------|--------:|-------:|-------------:|-------:|
| 0–15 s | 1.0 | 1.9 | 2.0 | 1.4 |
| 15–30 s | 1.6 | 3.1 | 3.4 | 1.9 |
| 30–60 s | 2.3 | 5.2 | 5.7 | 0.7 |
| >60 s | 3.0 | **9.6** | 11.1 | 1.3 |

A (fixed) degrades slowly with time (1.0 → 3.0°, the small near-nominal offset
integrating); **B (ZUPT) degrades fast (1.9 → 9.6°)** and C worse. Coverage:
fraction of segments under 3°/5° — A 0.60/0.60, B 0.40/0.40, C 0.20/0.40.
**B is worse than A in 80 % of segments** (by >2° in 20 %). Long segments (>60 s,
n = 2, near-nominal): A 4.2° vs B 7.2° vs oracle 3.4°.

### Why ZUPT hurts here, and what it means

On a near-nominal drive the oracle driving-zero ≈ 2048 (which A uses), but the
**stationary standstill zero is a noisier, biased estimate of the driving zero**
(stop-specific settling/vibration/temperature + ~0.6 c measurement noise). Holding
it (B) injects that error, integrating worse the longer the stop-free interval —
the opposite of the intended benefit. ZUPT's core assumption (stationary zero =
driving zero) is violated on these drives. (Bad-anchor outliers flagged in Step 1
sat in trip3/trip4, which were unscorable here, so the bad-anchor damage could not
be quantified against a reference; n = 0 scorable bad-anchor segments.)

### Verdict: ZUPT is NOT a safe unconditional ε-free win; it does NOT shrink the fusion case — both are blocked on the same missing data

- **Is B/C a worthwhile low-risk improvement over fixed-2048?** On existing
  (near-nominal) data, **no** — B and C are worse than fixed-2048 and degrade with
  time-since-stop, because the stationary zero mis-estimates the driving zero.
  Like fusion, ZUPT needs a **drifted-zero gate**; applied unconditionally it
  degrades the common case. Its only structural advantage over fusion remains that
  it is ε-free and simpler **if** such a gate is built.
- **Does ZUPT capture most of the achievable benefit ε-free, shrinking the fusion
  case to near-nothing?** Cannot be shown — the drifted, long-stop-free regime
  (trip3) has no heading reference, so neither ZUPT's benefit nor the fusion-only
  residual is measurable. The fusion case is not shrunk; it is simply **undecided
  on the same grounds** as ZUPT.
- **Net:** neither the ε-free ZUPT lever nor fusion+ε is justified on current
  evidence, and both face the identical blocker. The real prerequisite for
  deciding *either* is a single export that contains **a genuinely drifted gyro
  zero AND a usable held-out heading reference across long stop-free driving**
  (a trip3-like drifted session, but with valid GPS course, or another GPS-free
  truth). Until such data exists, the honest recommendation is **no correction
  shipped** (fixed-2048 stays best-or-tied on every scorable segment) and the next
  step is **targeted data capture**, not further estimator design.
