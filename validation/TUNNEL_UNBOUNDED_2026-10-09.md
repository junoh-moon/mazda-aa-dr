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
