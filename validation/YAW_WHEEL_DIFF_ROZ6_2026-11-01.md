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
