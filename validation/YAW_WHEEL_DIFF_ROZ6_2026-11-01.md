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

## 2. Bias-observer / complementary-filter fusion vs oracle on roz6

**Could not be computed on roz6.** The map-tangent oracle metric (the prior
~2.1–2.6° vs stock ~8°) needs a GPS-loss episode that uniquely matches one OSM
tunnel centreline. roz6 had only 6 qualifying GPS-loss episodes (50–600 s), and
**none produced even a single OSM candidate** (`osm_cand ≥ 1 = 0` for every
boot). The roz6 drive's tunnels are outside the coverage of the tunnel DB built
for the old drives' region. So ε = 0 / roz6-ε / pooled-ε fusion scoring yields
**0 clean tunnels** — no independent confirmation (and no refutation) of "ε is
the lever / fusion reaches oracle" from this drive.

## 3. Verdict: NEUTRAL, leaning slightly to WEAKENING. Not enough for a product change.

- **Confirms** the prior precision crux: even pooling roz6 with the old three
  trips leaves ε uncertain at ±0.069 %, an order of magnitude above the ±0.01 %
  a safe fixed ε would need. A handful of drives does not get there; the "~1000+
  accumulated highway-straight segments + confidence guard" requirement stands.
- **New caution (mild weakening):** roz6's usable drives center ε ≈ 0.08 %,
  ~3× the old pooled 0.025 %. Formally consistent (z = 1.24) only thanks to
  roz6's large SEM. If per-drive ε truly varies this much, a single fixed global
  ε is riskier than the old trio implied — directly relevant to "wrong ε is
  worse than none."
- **Fusion lever not re-tested:** no OSM-matched tunnel in roz6, so the headline
  fusion-vs-oracle result got neither independent support nor contradiction here.

**No product change is justified.** The pre-registered-safety-gate bar is not
met: ε precision is still short by ~7×, per-drive ε stability is now in mild
question, and the fusion benefit is unconfirmed on independent data. roz6
reinforces "do not ship a fixed ε yet; keep accumulating clean highway-straight
segments across drives, and add the per-drive ε-stability spread to the guard
criteria." A future export from a region the tunnel DB covers would be needed to
re-test the fusion-vs-oracle metric on independent data.

(Results file: scratchpad/roz6-eps-results.json; script: scratchpad/roz6_yaw.py.
Data stayed in the isolated roz6 dir; nothing committed or pushed.)
