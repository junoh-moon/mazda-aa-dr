# G1 first real tunnels: error decomposition, wheel scale, yaw zero, seed and BETA coverage - 2026-10-11

Status: offline analysis of the owner's private trip8 recording (beta.9/10 logic, tunnel mode) plus a coverage
count over the private BETA drives trip4, trip6, trip7 and trip8. RECORD ONLY: no product code, threshold or
profile change. GPS fixes are the reference, not ground truth. No coordinates, raw rows or payloads are reproduced;
times are CLOCK_MONOTONIC seconds of the boot. Not run on the vehicle, in the DHU, on exact ARM or in QEMU for this
record. Scripts live in the private scratch directory only.

## Data
- trip8 last boot `5a9f9b2b` (trace.0.jsonl from line 16859, 17,790 rows, 0 torn rows in this boot), about 44 min.
  Persistent profile: wheel/yaw rows exist only inside raw windows (about 700 s of the boot: 44-165, 591-973,
  1971-2057, 2328-2428, 2578-2638 s). Inside episode (b) the raw window overwrote 8,294 rows, leaving a 21.1 s hole
  (677.7-698.8 s) in the journaled wheel/yaw. The pipeline itself had the data; the published DR sends cover the hole.
- POSITION/send rows (646/647) exist in the same windows; their 48-byte LOCATION payloads give the GPS fix
  (and, while ENGAGED, the published DR position, speed and bearing).
- BETA uses yaw zero 2048 fixed (`beta_profile.h:149`, `pipeline.cpp:769`) and unscaled wheel speed
  (`pipeline.cpp:927`); the MODEL calibration rows (active zero 2047, wheel scale 1, gate mostly REVERSE) are not
  used by BETA.

## Method
- Return error: last published DR send vs the first GPS fix after `gps_returned`, the DR advanced along its own
  bearing at its own speed to the fix receipt time ("aligned"). Reproduces the coordinator's numbers (121.4/120.6 m,
  50.3/53.1 m, 32.5/25.5 m).
- Decomposition: along/cross-track relative to the GPS chord from the last pre-loss fix to the return fix; the chord
  rotation angle(DR chord) - angle(GPS chord) is the mean heading error, |DR chord|/|GPS chord| the length ratio.
- Offline re-integration: 20 ms grid, wheel mean of four wheels (count*0.01-100 km/h), yaw mean = sum/count,
  rate = (mean - Z) * -0.000658615 rad/s, heading += rate. Seeded at the first published DR send with its bearing,
  it reproduces the published DR end within 1.7 m (c), 0.4 m (e) and, with the 21.1 s hole filled from the
  published DR sends, 1.4 m (b). This validates the re-integration as the product's DR for counterfactuals.
- GPS latency: the wheel/Doppler cross-correlation is best when the GPS describes the state 1.2 s before its receipt
  (rms 0.73 m/s; the 2026-10-09 study bracketed 0-1.3 s). Assumed constant.

## Q1. Per-episode decomposition (DR vs first fix after return, aligned)

| | (a) 132.2 s | (b) 647.6 s | (c) 848.0 s | (d) 941.4 s | (e) 2384.0 s |
| --- | --- | --- | --- | --- | --- |
| Duration / state | 3 s, not engaged (unseeded) | 111 s ENGAGED | 60 s ENGAGED | 2 s ENGAGED (1 DR send) | 14 s ENGAGED |
| Return fix accuracy | - | 8.2 m | 3.2 m | 3.2 m | 198 m (3.4 m 2 s later) |
| Error raw / aligned | - | 121.4 / 120.6 m | 50.3 / 53.1 m | 38.0 / 10.2 m | 32.5 / 25.5 m (33.4 m vs the 3.4 m fix) |
| Along-track (+ = GPS ahead of DR) | - | +22.5 m | -8.5 m | +9.3 m | +8.9 m |
| Cross-track (+ = GPS right of DR) | - | +118.5 m | +52.4 m | -4.3 m | -23.9 m |
| GPS chord / DR wheel distance | - | 1182 / 1143 m | 777 / 762 m | 92 / - | 234 / 183 m (DR part) |
| Chord rotation DR-GPS | - | -5.8 deg | -3.8 deg | +3.0 deg | +6.1 deg |
| Along / distance (scale estimate) | - | -1.9 % | +1.1 % | - | -3.8 % |
| Latency-corrected along (-1.2 s x (v_exit - v_entry)) | - | +22.2 m (1.9 %) | +4.2 m (0.5 %) | - | +7.6 m |
| Wheel speed min/median/max | - | 13/41/61 km/h, no stop | 29/48/61 km/h | 107 km/h | 50/55/59 km/h |
| Gyro net heading change (Z 2048) | - | -5.0 deg (57 deg absolute) | -11.8 deg | - | -1.1 deg |
| DR end bearing - exit GPS courses | - | -16 to -22 deg | -9.0 to -12.4 deg | - | +5.8 to +7.1 deg |
| Honest budget / reported | - | 474 / 40 m | 484 / 40 m | 40 / 40 m | 54 / 40 m |

The error is cross-track in every long episode: |cross| / |error| = 0.98 (b), 0.99 (c), 0.94 (e). The along-track
residual is within +-2 % of the distance and changes sign between episodes; in (c) most of it is the GPS latency
artefact (entry 7 km/h, exit 45 km/h).

## Q2. Wheel-speed scale from FIX periods (outside and around the episodes)
- 436 fixes with accuracy <= 5 m inside the raw windows; Doppler speed vs wheel speed at the best lag:
  wheel/GPS = 1.0135 over all speeds, 1.0104 over steady samples (|dv/dt| < 0.3 m/s^2, n = 175).
- Per speed range (steady, ratio of sums / median): 7-18 km/h 1.070/1.047 (n 15), 18-29 km/h 1.028/1.025 (69),
  29-40 km/h 1.004/1.008 (24), 40-50 km/h 1.003/1.004 (24), 50-72 km/h 1.000/0.999 (37), 72-126 km/h 1.001/0.998 (6).
- Per window (steady): 0.999-1.039 (median about 1.007); 1-s position steps vs wheel: 1.04 (7-18 km/h), 1.006
  (18-29), 0.98-0.99 above 29 km/h. Position-step/Doppler is 1.004-1.055 and grows at low speed, i.e. the GPS
  references themselves disagree by 1-5 % at low speed (fix noise inflates step distance; Doppler smoothing).
- Conclusion: above 29 km/h the wheel scale is 1.000 +- 0.5 % against Doppler (trip4 found 1.0045 by distance).
  The low-speed excess is not separable from GPS artefacts. A single constant (e.g. 1.005) is adequate; its effect
  on this data is <= 0.5 % of distance: <= 6 m on (b), <= 4 m on (c), <= 1 m on (e). Scale changes of +-1-2 % move
  the (b)/(c)/(e) errors by at most 5 m (re-integration). The tunnel speed (G1) itself is therefore right to about
  1 % at tunnel speeds.
- 0x15B speed (chan_digest `v`, last value per 20 s, 0.01 km/h assumed): 31 samples above 15 km/h with a
  wheel sample within 0.2 s, 0x15B/wheel median 1.000 (0.91-1.07, spread from the change-only timing). It carries
  the same speed as the wheel mean; no scale gain from using it.

## Q3. Heading: yaw zero, seed and the first episode
Counterfactual return errors (re-integration from the first DR send to the return fix with accuracy <= 8.5 m):

| Variant | (b) | (c) | (e) |
| --- | --- | --- | --- |
| As published (Z 2048, published seed) | 122.1 m | 51.7 m | 33.4 m |
| Best constant Z (in-sample) | 2051.07: 16.9 m | 2051.28: 12.2 m | 2039.75: 24.8 m (implausible) |
| Z = last standstill mean before the loss | 2051.57 (309.6 s stop): 26.0 m | 2051.57: 13.0 m | 2047.04 (2217.4 s stop): 32.4 m |
| Z = analyzer driving fit just before | 2051.44: 22.4 m | 2046.99: 66.8 m | 2049.15: 34.6 m |
| Z = 2049 / 2050 fixed | 83.3 / 45.4 m | 37.0 / 23.2 m | 34.5 / 35.5 m |
| Seed = median of gyro-projected >= 15 km/h courses of the last ~11 fixes, Z 2048 | 156.8 m | 37.8 m | 10.2 m |
| Same seed + last-standstill Z | 23.0 m | 22.2 m | 10.0 m |
| Wheel scale 0.98 / 1.02 (Z 2048) | 126.3 / 122.1 m | 50.2 / 57.4 m | 34.7 / 32.7 m |
| Seed -3 / +3 deg (Z 2048) | 182.1 / 62.9 m | 91.3 / 15.1 m | 22.2 / 45.0 m |

- (b) is yaw-zero limited. Seed within 1 deg of the entry courses (319-323 deg at 22-30 km/h). The heading drifted
  left: the DR end bearing is 16-22 deg left of the exit courses; 1 count = 0.0377 deg/s, so -19 deg over 111 s is
  about 4.5 counts at the end and the chord rotation (-5.8 deg mean) about 3 counts. Stationary means in this period:
  2051.57 (309.6 s, 27.6 s stop), 2051.75 (970.6 s), 2051.66 (1212.0 s); the fixed 2048 was 3-3.6 counts low.
- (c) is also zero limited, not seed limited: the entry course 59 deg was measured at 7 km/h, but the >= 15 km/h
  courses 7-13 s before, projected with the gyro to the loss, give 48.5-53.6 deg (median 51.8) and the DR seed was
  51.5 deg. The DR end is 9-12 deg left of the exit courses at 39-45 km/h; with Z 2051.57 the error drops to 13 m.
- (e) is seed limited: the last three pre-loss courses (296, 298, 300 deg at 58 km/h) were 4-7 deg right of the
  gyro-projected course history (median 292.5) and of the exit courses (292-294); the seed followed them (299.8).
  Zero variants change nothing (14 s); the median-projected seed gives 10 m. The first fix after return (198 m
  accuracy) is not a usable reference; the 3.4 m fix 2 s later is.
- (d) 2 s at 107 km/h: one DR send; 10 m aligned is within seed/latency noise.
- (a) was never engaged: BETA was unseeded since boot because every anchor from 81 to 131 s (20-31 km/h,
  HDOP 1-1.2) was rejected only by gate REVERSE (no 0x118 message since the AA session started; first one at 638.3 s).
  The `g3_first` course row at 560.1 s is the G3 study's first accepted course, also after the same gate. The
  first-episode error difference ((b) 121 m vs (c) 53 m) is explained by duration (111 vs 60 s; the zero-induced
  cross-track grows with t^2) and by the same zero offset, not by a first-loss seed or calibration issue: both
  share a Z of about 2051.1-2051.3 and the (b) seed was good.
- Stop-to-stop variability in this drive: stationary means 2036.3-2056.2 overall; 2051.66 -> 2045.06 within 12 s
  (1212 -> 1223.7 s), 2047.04 -> 2056.18 -> 2036.34 around parking (2467-2555 s, sd up to 6.8). A last-stop rule
  that had picked the 1223.7 s stop (2045.06) before (b) would have been about 6 counts off and roughly doubled the
  error of the fixed 2048. The 2026-10-09 study found the last stop worse than 2048 at the median on trip2.

## Coverage: when BETA could not engage (coordinator addendum)
Continuous-anchor builds (trip6, trip7, trip8; 6 driving boots; trip4 is beta.3 and has a different anchor design),
moving time = 10 s digests with mean wheel speed >= 1 km/h:

| Boot | Moving | Unseeded moving | Main causes (s) | First seed after BETA armed |
| --- | --- | --- | --- | --- |
| trip6 82d37f8a | 1514 s | 238 s (15.7 %) | reset->REVERSE 221 | 138 s |
| trip6 322800ef | 2507 s | 237 s (9.5 %) | boot REVERSE 227, withdrawn 10 | 275 s |
| trip7 e5584f62 | 1013 s | 182 s (17.9 %) | boot no/poor fix (garage start) 180 | 302 s |
| trip8 1c1de788 | 541 s | 10 s (1.8 %) | boot COURSE 10 | 80 s |
| trip8 0914e18f | 411 s | 234 s (56.9 %) | fence then no POSITION polls 161, boot REVERSE 60 | 194 s |
| trip8 5a9f9b2b | 2226 s | 821 s (36.9 %) | boot REVERSE 526, reset->REVERSE 243, no polls 50 | 596 s |
| Total | 8212 s | 1721 s (21.0 %) | REVERSE gate 1276 s (15.5 % of moving, 74 % of unseeded) | - |

- GPS losses in these boots: 10; 8 engaged with a DR position, 1 unseeded by the REVERSE gate ((a), 3 s), 1 at a
  garage start without any fix (trip7 86.4 s, 198 s, speed overlay only). trip4 (beta.3): 3 losses, none engaged
  (budget/anchor design of beta.3, TRIP_BETA3_2026-10-08), not a REVERSE-gate case.
- Resets that unseeded: BAD_INPUT (yaw mean >= 4094 during a sharp low-speed turn) at trip6 1631.9 s and trip8
  2026.7 s, each followed by 250/355 s of REVERSE gate because the reset also clears the reverse latch;
  MISSING_SENSOR resets at parking only. Fences: 2520.5 s here (3044 ms position gap), and trip8 0914e18f at 500 s
  where the AA position polls stopped entirely.
- 0x118 is change-only. Digest counts (complete, unlike the windowed motion rows): 10 messages in this boot, the
  first 6 (value 0) at 638.3, 754.7, 842.4, 905.3, 2380.0, 2396.1 s, each 2-10 s before a GPS loss or return
  (lamp-like coupling is a guess; trip6 messages do not line up with its losses), then 4 at parking including the
  only reverse=1 (2494.0 s). In 4 boots (trip6 82d37f8a, 322800ef; trip8 1c1de788, 0914e18f) messages that
  arrived before the AA session start (e.g. 6 at engine start) did not prevent REVERSE afterwards; each boot seeded
  only after the first message following the session start. The latch is cleared by the reset at the session/bus
  boundary or was not yet armed (mechanism not traced further).

### The reverse gate (`pipeline.cpp:1062`, `:1210`, latch `:205`, `:835`, integration `:762`)
Anchors and seeds require a known forward state: `reverse_at()` must return a latched 0 and `reverse_exit_seen_`
(a 1->0 transition or a first message 0 in this source epoch). Integration also waits for `reverse_known()`.
Protection: while reversing, the GPS course is the motion direction, 180 deg from the body heading; a seed from it
would integrate 180 deg wrong, and reverse travel integrated as forward doubles the displacement error.

Smallest safe rule proposed (rule F, inferred forward), not implemented: in the current source epoch, with no
0x118 message at all, after 3 consecutive data-valid fixes whose GPS speed is >= 20 km/h (the existing anchor
minimum) and agrees with the wheel speed (existing 4 km/h check), set the latch to forward with a distinct journal
reason (`inferred_forward`). Any real 0x118 message overrides it; it is cleared by everything that clears the latch
and additionally by a tap `lost` increase. Reasoning: engaging reverse is a change, so a reverse=1 message is sent
(seen at every parking reverse: trip4 4209.4, trip6 2066.0/3355.8/3369.2, trip7 1436.7, trip8 722.9/2494.0 s);
the only state an absent message hides is "in reverse since before the session", which cannot produce 3 s at
>= 20 km/h in practice.
- Failure mode, reversing out of a garage at boot: below 20 km/h rule F does not fire, no seed comes from the
  reverse motion; the first seed is from forward driving. If a fast reverse did seed (>= 20 km/h for 3 fixes), the
  heading is 180 deg off; the continuous policy resyncs after 5 consecutive chord-confirmed courses disagreeing by
  > 20 deg (BETA_RESYNC_FIXES 5, gap <= 2.5 s), i.e. about 5-6 s of forward driving with fixes. A tunnel entered
  inside that window would integrate backwards: error about 2 x distance travelled until the return.
- Counterfactual on these boots (first time 3 consecutive REVERSE-gated anchors reach >= 20 km/h, +2 s): seeds at
  85 s instead of 640 s and 2039 s instead of 2382 s in this boot; recovers 1056 of the 1276 s REVERSE-blocked moving
  time (12.9 % of all moving time). Episode (a): seeded about 47 s before the loss, last anchor-eligible fix 130.1 s
  (HDOP 1), loss at 132.2 s, so `ENTRY_FRESH` (<= 3 s) would most likely have engaged it for 3 s; the gates masked
  by REVERSE (course/displacement/yaw-quiet) were not evaluated in the recording, so this is an estimate.
- Secondary, narrower option: keep the latch across the session/bus reset when no REVERSE event is queued (as the
  runtime small-gap path already does). Covers only boots with pre-session messages (trip6 322800ef: about 227 s).
- Not available from this data: a CAN reverse register read at boot (no request path in the product) or a proven
  reverse-lamp coupling.

## Q4. What would reduce the errors cheaply and safely (ranked by reduction on this data / risk)
1. Rule F (inferred forward, above). Tunnel error unchanged, coverage +1056 s moving across 6 boots, +747 s here,
   one more engaged loss. Risk very low (needs a >= 20 km/h reverse); journaled reason. Highest value per risk.
2. Yaw zero from the last standstill before the loss, as a BETA-labeled correction. Measured: (b) 122 -> 26 m,
   (c) 52 -> 13 m, (e) 33 -> 32 m. Risk medium: in-drive stop-to-stop jumps of 6.6 counts and the trip2 result
   (last stop worse at the median). This data is new evidence (two real tunnel exits where a stop 338/539 s earlier
   matched the in-sample zero within 0.3-0.5 counts) but two tunnels of one drive do not meet the 2026-10-09
   pre-registered criterion. Proposed first as a record-only counterfactual: at each GPS return journal the DR end
   under (last-stop Z, gated |Z-2048| <= 6 and stop sd <= 1 count) next to the published one, and re-run the
   2026-10-09 harness on trip4 A/B with this candidate. Adopt only if it passes on more tunnels.
3. Seed from the gyro-projected course history (median of the last ~10 s of >= 15 km/h chord-confirmed courses
   projected to the loss) instead of the latest blended course. Measured: (e) 33 -> 10 m; alone it worsens (b)
   122 -> 157 m, improves (c) 52 -> 38 m; with (2): 23/22/10 m. Record-only alongside (2).

Not proposed: a wheel scale correction or the 0x15B speed (<= 6 m on this data; scale within 0.5 % above 29 km/h;
0x15B equals the wheel mean). Accuracy semantics: the LOCATION payload carries one horizontal accuracy (a flag and
one value); it cannot express along/cross-track or a covariance. The honest values (474/484 m) would be ignored by
Naver (DHU_NAVER_EXP3: >= 100 m ignored, 5-30 m followed), so the 40 m cap is a deliberate under-report (factor 3 in
(b), 1.3 in (c)); the honest route to a truthful 40 m is a smaller real error ((2)/(3)), not a different field.

## Measured vs assumed
Measured: return errors and their decomposition, the re-integration reproducing the published DR (<= 1.7 m), wheel
vs Doppler ratios, 0x15B vs wheel, stop means, counterfactual errors, coverage times and gate counts.
Assumed: GPS fixes as reference (8.2 m accuracy at the (b) exit; 198 m at the (e) return), a constant 1.2 s GPS
latency, the 0.01 km/h unit of 0x15B, the filled 21.1 s hole in (b) (taken from the published DR, so variant errors in
(b) assume the pipeline saw the same yaw), the masked anchor gates in the rule F counterfactual, and that the owner's
"first correction did not work" refers to (b) (Naver's on-screen behaviour was not recorded).

## Additional data needed
- More real tunnels with exit fixes and a standstill before them (same route repeated), for candidate (2)/(3) under
  the pre-registered criterion; the trip4 A/B re-run with candidate (2).
- Whole-outage motion in the journal: the raw window (60 s pre, 30 s post) overwrote the middle of the 111 s
  outage; a window that stays open until the return would remove the hole.
- Naver's screen during an engaged tunnel (whether a 120 m cross-track offset breaks map matching).

## Limitations
One car, one firmware, one drive for the tunnel errors (three usable episodes); in-sample choices (best Z, seed
window); GPS is not ground truth; not tested on the vehicle, phone, DHU, exact ARM, QEMU or with replay_beta.
