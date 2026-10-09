# Tunnel mode (unbounded LOST replacement) - owner decision and DHU experiment 6, 2026-10-09

Status: implemented and host-tested; NOT run on the vehicle; DHU only. Target: v1.0.0-beta.6 (not published by this record).

## Problem and decision
The first real BETA drive (validation/TRIP_BETA3_2026-10-08.md) showed that the stock mode-0 resend stops Naver navigation in a tunnel, and
that honest dead reckoning (reported accuracy <= 40 m, rejected by Naver at >= 50 m) ends after ~15-30 s while Korean tunnels last minutes.
The owner decided (2026-10-09, "5분이고 1시간이고 무한히 되게 만들라고", then explicit full approval): a GPS-lost episode must keep being
replaced until the GPS returns. This supersedes the ACCURACY_RULE "never clamp" for the BETA domain only; the rule's reasoning (Naver
rejects >= 50 m) is unchanged and is why the REPORTED accuracy is clamped at 40 m.

## Behavior
- BetaProfile.unbounded (beta_profile_tunnel(), production since beta.6): no duration/distance/error/heading-budget withdrawal. Numeric
  sanity caps only: 6 h, 1000 km (core extended limits, MODEL domain only; mx5_dr_init for QUALIFIED rejects them).
- Reported accuracy = min(honest budget, 40 m). The honest budget is journaled as accuracy_honest_m in beta_state / beta_summary rows
  (the analyzer must treat accuracy_m == 40 with accuracy_honest_m > 40 as CLAMPED, not as a claim of 40 m accuracy).
- Still ends the replacement: GPS return (mode != 0), sensor silence, reverse-latch suspicion, send-result hold, session/storage change,
  cadence gap, core fault, DISABLE. A missing anchor still means stock behavior (no replacement).
- Limits of honesty: after the honest budget passes 40 m the reported position may be hundreds of metres off the true position
  (measured 77/280/311 m at the exits of the real tunnels with the yaw-zero shift). Naver is expected to map-match onto the route; this is
  DHU evidence, not vehicle evidence.

## Independent review of b1ec29a and follow-up fixes
An independent Opus review (code reading, no blocker) found: (high) the in-outage limits were also the only age bound of a stale anchor, so
an anchor that integrated for minutes through a GPS-present stretch above the anchor speed ceiling would have started an episode with a
km-scale honest error at the first send; (medium) a standstill was sent without bearing (DHU T9: rejected), (medium) a stale
accuracy_honest_m from an earlier episode could excuse a return jump after a short outage in the analyzer; (low) struct padding of
mx5_dr_config. Fixes: pipeline.cpp applies the bounded envelope (60 s, 40 m honest budget) at the moment the GPS goes (otherwise DISABLE, the
outage stays stock); tunnel mode sends speed 0 with the held body heading while the car stands; the controller resets accuracy_honest_m at
GPS_LOST and the analyzer clears it there and only treats accuracy >= 40 m as clamped; mx5_dr_config.extended_limits moved to the end, zero
padding. Tests: stale_anchor_does_not_start_a_tunnel_episode, standstill_in_a_tunnel_episode_keeps_a_bearing, analyzer stale-honest case,
core reserved field. The standstill form (speed 0 + bearing) has NOT been tested in the DHU.

## Long-outage stress test (synthetic, offline replay of the product path; second agent, ASan+UBSan build)
Constant 50 km/h and repeated 90 deg turns: replacement continues for the whole outage up to 7200 s (e.g. 3599/3600, 7199/7200 mode-0
sends replaced; the single unreplaced send is the first mode-0 send by design), accuracy always in (0, 40] m, no NaN, speed equals wheel speed,
no replaced send after the GPS return, no sanitizer finding, closed-form straight-line check within 1 mm over 100 km. Honest budget
(straight): 120 s 308 m, 600 s 5.3 km, 3600 s 86 km (finite, monotone). Memory of the product path stays at 5.5 MB RSS. Journal volume while
ENGAGED is about 4.3 KB/s (15.5 MB/h), about 2.1x the 2 KB/s persistent profile budget and inside the 120 MiB trace rotation; no drops.
DEFECT FOUND AND FIXED: a complete standstill (stop-confirmation wait of 1.5 s, then stopped) ended the episode at the first stop
(reason mislabelled budget_limit; bridge BEARING) and it never resumed before the GPS return. Tunnel mode now sends speed 0 with the held body
heading for both the wait and the stopped state; the reason is labelled bearing_unavailable. Rerun after the fix: stops of 30/60/90 s
inside a 600 s and a 1800 s outage keep the replacement (599/600 and 1799/1800). The speed 0 + held bearing form has not been shown to Naver in DHU.
KNOWN LIMIT: a synthetic garage creep with abrupt yaw/speed edges (3 km/h turns with short stops) still ends with a core E_FRAME after ~46 s
(stopped estimate with |yaw| > 0.02 rad/s); the episode returns to stock. Not claimed as G2.
UNEXPLAINED: heading error accumulates with turns in the synthetic runs (about 0.1 deg per 90 deg turn at speed; about 0.57 deg per
creep cycle), independent of quantization of the fixture yaw; needs follow-up for G2.
Not tested: vehicle, phone, DHU, exact ARM, QEMU, sensor noise or bias, wheel scale error, reverse, cadence gaps, real OEM timing, threads.

## G2 heading accumulation (root cause found, fixed)
A third agent bisected the heading drift seen in the stress test: dr_core integrates exactly (error 2e-11 deg); the pipeline truncated the yaw
window mean with an integer division (pipeline.cpp, `unsigned mean=raw/count`). With a noisy real sensor that is a nearly constant zero
shift of about -0.39 count (already absorbed in the fixed zero 2048); with the noise-free synthetic fixture it appears as a rate-dependent bias per
turn (34 deg after 3600 s of creep cycles, 0.6 deg after the fix). The yaw scale is correct (0.000658615 rad/s/count). Fix: the exact
sum/count mean is used for every rate (Event.mean_counts); the integer stays only for the core's raw guard. Effective BETA zero moves by
+0.39 count (0.015 deg/s) toward the fitted real-data zero (2048.1 +- 0.45 from trip4); the profile zero stays 2048. Real-data fit of
trip4 (3 usable stretches, 89 samples): scale 1.000 +- 0.006, no measurable gain from a scale correction. G2 outlook: the dominant error is the
per-drive zero (about 4 counts 1-sigma, TRIP_BETA3): garage heading within a few degrees for about 30 s, no 10 degree guarantee beyond about
120 s. Not tested: vehicle, low-speed garage turns against GPS (no data), zero 2048.4 variant.

## DHU experiment 6 final (27 trials, 855 photos, S25 + Google DHU + Naver Map; private data, summary only)
Operational labels from screen reading (not Naver internals). Repeats r1/r2 (+ approved T4 r3):
| Condition | Result |
| --- | --- |
| T0M stock mode-0 form (fast), T0 no input, T8C/T0MS slow controls | REJECTED-EXTRAPOLATES x2 (gray, extrapolates at entry speed, ~1 km display correction at the slow exit); the real-car navigation stall was NOT reproduced |
| T7 accuracy 100 m, T9 no bearing | REJECTED-EXTRAPOLATES x2: accuracy must be <= 40 m and a bearing must be present |
| T2 +50 m, T3 +150 m cross offset, accuracy 40 | FOLLOWS-ON-ROUTE x2 |
| T5/T6 +-100 m along offset, accuracy 40 | FOLLOWS-ON-ROUTE x2 |
| T8 real speed profile, accuracy 40 | FOLLOWS-ON-ROUTE x2, all 60 photos blue, display speed follows 16 -> 29, no exit correction |
| T4 cross offset growing 0 -> 300 m, accuracy 40 | r1 OTHER (14/18 gray, non-monotonic), r2 and r3 FOLLOWS-ON-ROUTE; primary r1/r2 = INCONCLUSIVE |
| T1 ideal, accuracy 40 | r1 OTHER (2/18 gray), r2 FOLLOWS: INCONCLUSIVE; the only gray episodes at accuracy 40 were in the FIRST run of T1 and T4 (order/warm-up effect not excluded) |
No route departure or re-route was observed in any of the 27 valid trials.
Interpretation for the product: reporting accuracy 40 m with a bearing keeps Naver on the route in DHU even with offsets of 150 m (constant)
and 300 m (growing) in 5 of 6 runs; accuracy 100 m or a missing bearing does not. Constants of the product therefore stay: accuracy <= 40 m,
bearing always present. Unknown: real wireless-dongle car behavior, internal acceptance, why T1/T4 r1 differed.

## Offline checks
- tests/core/test_core.c (extended limits MODEL-only), tests/navigation/test_beta.cpp (unbounded clamp and honest value), tests/runtime/
  test_worker_beta.cpp (budget scenario now continues, accuracy clamped at 40000e-3), tests/replay/test_replay_beta.py (no budget
  withdrawal; replacement until GPS return). The previous bounded behavior stays covered by beta_profile() tests.
- Replay of the private beta.3 drive: 3 real outages, no engagement (that recording lacks pre-tunnel wheel/yaw rows, so no anchor can be
  formed offline); NOT evidence for or against tunnel mode.
- Not run: exact-ARM and QEMU probes for this change, vehicle.
