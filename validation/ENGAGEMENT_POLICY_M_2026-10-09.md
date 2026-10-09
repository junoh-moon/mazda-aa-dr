# BETA engagement policy M and its review fixes - 2026-10-09

Status: implemented and host/replay-tested only. NOT run on the vehicle, with a phone, in the DHU, on exact ARM or in QEMU.
Applies to the unbounded tunnel profile (`beta_profile_tunnel()`, `continuous_anchor`), the target of v1.0.0-beta.6 (not published by this
record). It supersedes the "bounded envelope at the moment the GPS goes" start rule and the "gated anchor" wording of
validation/TUNNEL_UNBOUNDED_2026-10-09.md for that profile; the bounded `beta_profile()` and its rule-3 gate are unchanged.

## Problem
Tunnel mode (TUNNEL_UNBOUNDED_2026-10-09) keeps replacing a GPS-lost episode until the GPS returns, but an episode could only start from a
"gated anchor": 10 s of settled 1 Hz fixes, 20-60 km/h, a 3 deg course step, a quiet yaw window and a wheel/GPS speed match. In the owner's
recordings many real and pseudo outages started below 20 km/h, in a curve, or right after a GPS return, so the replacement never engaged
(G1 tunnel speed and G2 garage heading both need the episode to start). The goal of M is availability: start an honest episode whenever the
position and heading evidence is fresh, without speed or settling gates. It does NOT make the dead-reckoned position more accurate.

## Candidates and head-to-head (private trips 2 and 4, replay_beta pseudo outages, truth lag 1.3 s, 120 s windows)
A: continuous anchoring with a course-speed minimum and the estimate kept across a GPS return. B: every valid fix refreshes position,
speed-weighted robust course blend, absolute displacement slack. C: geometric course weights, rotation agreement (course change vs yaw),
teleport rejection. M: B plus a 3 m chord check for the first course, a 5-fix consistent-course resync, reverse position refresh and a 3 s
position freshness rule at the GPS loss (chosen by the head-to-head, scripts in the private h2h scratch).

| Candidate | trip2 engaged | trip2 @60 s pos p50/p90 m | trip2 @60 s brg p50/p90 deg | trip4 engaged | trip4 start brg p50/p90 | trip4 @10 s brg p50/p90 |
| --- | --- | --- | --- | --- | --- | --- |
| A | 250/258 | 16.8/36.5 | 2.3/9.2 | 65/90 | 0.6/11.5 | 1.3/9.7 |
| B | 252/258 | 17.6/34.1 | 2.3/8.6 | 69/90 | 1.0/9.5 | 1.7/11.5 |
| C | 252/258 | 19.3/40.3 | 3.2/9.7 | 69/90 | 1.1/9.7 | 1.4/14.8 |
| M | 251/258 | 18.1/37.4 | 2.5/8.7 | 68/90 | 1.0/9.4 | 1.7/9.9 |

For comparison the legacy gated policy (`replay_beta --engagement legacy`, same harness, rerun for this record) engaged 214/258 (trip2)
and 10/90 (trip4) of the same windows. The GPS fixes are the reference, not ground truth.

## Independent review of M and what was done
| Finding | Disposition |
| --- | --- |
| H1: after boot, a reverse then forward drive could seed a heading 180 deg wrong (the unseeded core rejects reverse fixes as REVERSE, so the course reference survived the reverse manoeuvre) | FIXED: in continuous mode the course reference and the resync run are cleared whenever the reverse state is unknown or nonzero, whatever the gate. Test `reverse_then_forward_does_not_seed_backwards`. |
| T: no test that a 1 Hz reverse position refresh keeps the 3.4 reverse-contradiction timer | ADDED `tunnel_reverse_refresh_keeps_contradiction_timer` (from the review). |
| M2: GPS_RETURN unseeded the core; a second tunnel needed PREVIOUS -> COURSE -> seed (3 fixes) | FIXED: on a GPS return of an ACTIVE, unfailed estimate the core is reseeded at its own dead-reckoned estimate with the carried heading and the honest position/heading budgets (`beta_carry_across_return`). This is not a GPS position (the last-fix time and the rotation budget are unchanged). The next fix that passes every check refreshes only the position (source yaw/blend, never a new seed). Test `back_to_back_tunnels_keep_the_heading`. |
| M3: the 3 s freshness rule left ~1 s of margin on the real losses (degraded HDOP 58/78 fixes 1 s before 2 of 3 losses) | FIXED as proposed: positions still refresh only from fixes passing every check (HDOP <= 3). At the loss: FRESH if the last accepted position is <= 3 s old; otherwise FALLBACK from the estimate only if the core is seeded, READY, unfailed and current, the last accepted position is <= 60 s old, the honest budget is <= 40 m and every rejected data-valid fix since then agreed with the estimate within honest budget + 20 m; otherwise REFUSED (stock outage, DISABLE). Each loss writes one beta_anchor row ENTRY_FRESH/ENTRY_FALLBACK/ENTRY_REFUSED with `"entry":[age s, honest budget m, reason]`. Test `stale_position_does_not_start_a_tunnel_episode` (rewritten: fresh, fallback, disagree, budget, age, unseeded). |
| M4: persistent log rate 2043.4 B/s against the 2048 B/s check | FIXED: compact beta_anchor row (below), 1942.0 B/s. |
| M1: the design summary claimed an "8 deg + abs(rotation)/2" course-change vs yaw-integral agreement check that existed only in C | EVALUATED and ADDED (criterion met, below): `BETA_COURSE_TURN_MAX_DEG=8`, `BETA_COURSE_TURN_FRACTION=0.5`; a carried heading is blended with, or resynced from, the GPS course only while the course change since the reference fix matches the closed yaw integral. Test `course_step_without_yaw_turn_is_not_blended`. |
| L: stale comments ('new gated anchor', SETTLING, 'Rule 3'), replay test sources | FIXED: comments in beta_profile.h/pipeline.h/pipeline.cpp/beta_controller.h; `unbounded && !continuous_anchor` is documented as the replay-only `--engagement legacy` counterfactual; tests/replay accepts the new source codes (4 reverse, 5 resync). |

Mutation checks (test_beta built with each protection removed; every one fails at the named test): H1, T, M1, M2 carry, M3 fallback,
M3 agreement.

## Compact beta_anchor row (M4)
`{"kind":"beta_anchor","mono_ns":..,"seq":..,"mode":..,"utc_s":..,"gate":"..","hdop":..,"kmh":..}` plus optional fields: `ratio` (pair
displacement ratio; only when evaluated and not on a continuous ACCEPTED row), `streak_s` (legacy gate only), `reverse_exit_seen:false`
(only while unproven), `dropped` (only when nonzero), `h:[source, heading 0.1 deg, uncertainty 0.1 deg(, GPS weight %)]` on an accepted
fix (sources 1 seed, 2 blend, 3 yaw, 4 reverse, 5 resync, 9 legacy; weight only for blend), `entry:[..]` on ENTRY_* rows. `domain` is
omitted: the kind exists only in the beta domain; the analyzer still flags any explicit other domain. Numbers use 4 significant digits.
tools/analyze_logs.py reads both the compact and the earlier form and adds `heading_sources`, `heading_resyncs` and `entry_decisions` to the
BETA JSON summary and one text line.

| Synthetic 1 h drive (tests/runtime/log_rate.cpp, persistent) | bytes/s | beta_anchor bytes/row |
| --- | --- | --- |
| HEAD e05e325 (from the M design notes) | 2018.5 | - |
| M (working tree before this record) | 2043.4 | 246 |
| after the review fixes | 1942.0 (5.2 % under 2048) | 140 |

## Measurements (replay_beta, private trips; GPS fixes are the reference, not ground truth; truth lag 1.3 s unless stated)
Measurement-only hooks lived in a scratch copy, not in the product diff: `KEEP_LATCH` (the h2h trip4 latch setting), `NO_FALLBACK` (3 s-only
rule), `NO_CARRY`, and `PRE_DEGRADE_MS` (fork each pseudo-outage window that much earlier and raise the HDOP of the recorded fixes before T0
to 50, data still valid: the pattern seen before the real losses).

Regression with no degradation (120 s windows, position/bearing p50/p90):
| Trip, lag | engaged M -> final | start pos | start brg | 60-120 s pos | 60-120 s brg |
| --- | --- | --- | --- | --- | --- |
| trip4, 1.3 s | 68/90 -> 68/90 | 6.1/26.9 -> same | 1.0/9.4 -> same | (outages end before 60 s) | - |
| trip2, 1.3 s | 251/258 -> 251/258 | 6.4/17.3 -> same | 1.2/5.7 -> 1.3/5.7 | 21.1/43.7 -> 21.1/40.9 | 3.4/9.6 -> 3.5/9.7 |
| trip4, 0 s | 68/90 -> 68/90 | 1.7/5.0 -> same | 1.0/30.1 -> same | - | - |
| trip2, 0 s | 251/258 -> 251/258 | 1.8/6.4 -> same | 1.3/5.2 -> 1.3/5.0 | 21.4/43.0 -> 21.4/41.1 | 3.6/10.7 -> 3.6/10.8 |
The final code differs from M here only through M1 (all pseudo outages enter FRESH; H1/M2/M3 do not trigger).

M3, degraded fixes before the loss (3 s-only rule -> final with fallback; start error of the engaged windows):
| Degraded before T0 | trip4 engaged | trip4 start pos p50/p90 | trip2 engaged | trip2 start pos p50/p90 |
| --- | --- | --- | --- | --- |
| 1.5 s | 57/85 (67 %) -> 64/85 (75 %) | 7.6/27.1 -> 7.5/27.1 | 239/257 (93 %) -> 250/257 (97 %) | 6.5/20.3 -> 7.0/20.3 |
| 2.5 s | 0/81 -> 61/81 (75 %) | - -> 7.5/26.2 | 84/256 (33 %) -> 243/256 (95 %) | 10.3/21.8 -> 7.3/18.8 |
| 3.5 s | 0/80 -> 60/80 (75 %) | - -> 7.7/26.3 | 0/256 -> 231/256 (90 %) | - -> 7.0/19.9 |
| 5 s | 0/76 -> 56/76 (74 %) | - -> 8.1/26.0 | 0/255 -> 220/255 (86 %) | - -> 6.1/18.8 |
| 10 s | 0/67 -> 11/67 (16 %) | - -> 11.3/26.7 | 0/253 -> 147/253 (58 %) | - -> 2.8/17.9 |
Refusals with fallback are reason `budget` (honest budget > 40 m) or `unseeded`; no `disagree` occurred because the degraded fixes keep their
recorded positions. Over the whole windows the fallback episodes stay within the same bands as fresh ones (e.g. trip2 2.5 s: 60-120 s
position p90 40.1 m, bearing p90 9.7 deg).

Real losses (trip4 with the h2h latch setting, trip2): unchanged, all three trip4 losses and the trip2 loss enter FRESH (ages 2.0/1.9/1.0 s
and 1.8 s); replaced sends 69/158 (trip4), 29/158 without the latch setting, 2/3 (trip2), identical to M. The degraded HDOP 78/58 fixes 1 s
before the first two trip4 losses are 0.2 m and 1.9 m from the dead-reckoned continuation of the previous good fix, so they would have
"agreed" had the fallback been needed. The second trip4 loss (4.9 s after the return) still seeds from the GPS course: at that return the
core had already been reset by the cadence fence of the first outage, so the M2 carry did not apply to it.

M2, synthetic straight drive, second loss g s after the return (published fraction of 0.5 s slots in the next 10 s; M -> final):
| First tunnel | g=1 | g=2 | g=3 | g=5 |
| --- | --- | --- | --- | --- |
| 5 s | 0/21 -> 21/21 (FALLBACK, accuracy 34.9 m) | 0/21 -> 21/21 | 21/21 -> 21/21 | 21/21 -> 21/21 |
| 60 s | 0/21 -> 0/21 (REFUSED: budget) | 0/21 -> 21/21 | 21/21 -> 21/21 | 21/21 -> 21/21 |

M1 (C's agreement check) decision: on the sweeps above it changed no engagement and moved bearing p90 by at most +0.1 deg (start p90
5.2 -> 5.0 deg on trip2 at lag 0); position p90 at 60-120 s improved 2-3 m on trip2. Synthetic h2h scenarios: a 25 deg wrong first course
resyncs 1 s later (recover 4 -> 5 s); 4-fix 20/30 deg multipath bursts leave 0.3-0.7 deg less bearing error at loss + 1 s (2.3 -> 1.7,
1.0 -> 0.7 at 30 km/h; 2.5 -> 1.8, 1.1 -> 0.8 at 60 km/h); 40/90 deg wrong courses, turns and turns at the loss are unchanged. The criterion
(no lower engagement, bearing p90 not worse by more than 0.5 deg) is met, so the check is part of the product code.

## Offline checks run (host, scratch copy of the working tree)
test-navigation (test_beta 38789 checks), test_worker_beta (all 11 scenarios), test-runtime, test-core, test-integration, test-replay-beta
(7 tests; private trip replay via MX5DR_TRIP_DIR not used by the target), test-tools (85 tests incl. tests/tools/test_analyze_beta.py 38),
test-motion-journal (192 tests incl. the log-rate check). Not run: exact-ARM build/tests, QEMU probes, the full `make test` (owner's run).

## Not verified
Vehicle, phone, DHU (no Naver session saw the FALLBACK start, the carry across a return or the compact journal), exact ARM and QEMU, real
OEM timing of back-to-back tunnels, how Naver treats a wrong bearing (a H1-type 180 deg seed was not shown to Naver either way), and any
accuracy claim: the fallback starts from an older estimate, so its position error can only be at least as large as a fresh start of the same
episode; M improves availability, not position accuracy. The honest budget and the 20 m agreement slack are MODEL heuristics, not
qualified bounds.
