# Online in-drive yaw zero estimator - offline study, 2026-10-09

Status: offline study on the owner's private recordings only. NO product change: no candidate met the adoption criterion, so BETA keeps
the fixed yaw zero 2048 (BETA_DECISIONS_2026-10-05 rule 3.5). Not run on the vehicle, with a phone, in the DHU, on exact ARM or in QEMU.
GPS fixes are the reference, not ground truth. Coordinates and raw logs stay in the private scratch directory.

## Question
Heading error inside a tunnel is dominated by the yaw zero (1 count = 0.0377 deg/s; 4 counts = 9 deg/min). The stock navigation and any
normal INS estimate the gyro bias online while the GPS is good. Does an online in-drive zero estimate, frozen at the GPS loss, beat the
fixed 2048 on the owner's data? Adoption criterion set before the runs: heading p90 at 60 s at least 30 % lower than fixed 2048, at both
GPS delays, without a clearly worse case.

## Data and method
- trip2 (`trip2`, 2026-10-04 drive): continuous wheel/yaw with 514 s of 1 Hz GPS (930-1460 s), urban stop-and-go, no real outage.
- trip4 (beta.3 drive, TRIP_BETA3_2026-10-08): raw yaw only in 8 windows; 10 s digests give the exact yaw mean in between. GPS fixes with
  yaw exist only 53 s before tunnel A, 4 s between A and B, 30 s after B and 53 s before the garage C. trip3 is unusable.
- Faithful offline integration (Python, 20 Hz grid, exact sum/count yaw mean as in the product since e05e325, heading frozen after the
  dr_core stop confirmation: v <= 0.2 m/s for 1.5 s, leaves at 0.5 m/s, unscaled wheel speed as in BETA). The product pipeline was not
  used because no candidate reached the implementation stage.
- Leave-future-out: at every fix with a valid course (>= 10 km/h, accuracy <= 5 m) as a pseudo loss time T, each estimator sees only fixes
  received before T. Dead reckoning starts at the fix position with the fix course (identical for all estimators, so differences are due
  to the zero only) and is compared with the fix nearest T+H (>= 15 km/h). GPS delay 0 s and 1.3 s (estimator and reference use the same).
- Candidates: (a) fixed 2048; (b) EWMA of the zero implied by GPS course change minus integrated yaw over non-overlapping qualifying
  intervals (all fixes >= 15 km/h, accuracy <= 5 m, gaps <= 2.5 s, no stop, |course change| <= 10 deg (5 deg variant), interval 10/15/20/30 s,
  |z-2048| <= 20, time constants 30/60/120/300 s, with/without a 2048 prior of 15/60/240 s weight, at least 1-3 intervals, cap +-8);
  (c) stationary zero (all wheels 0 for >= 2 s, steady window), alone and blended with (b); (d) a heading+bias Kalman filter updated by every
  qualifying fix (course sigma 1/1.5/3 deg scaled by 30 km/h / speed, bias random walk 0.0002-0.01 counts^2/s, initial bias sd 1 or 4,
  4-sigma innovation gate, fallback to 2048 while the bias sd > 1.5-3 counts, cap +-8). Scripts: private scratch `yz2.*/ana`.

## Results, trip2 pseudo outages (heading |error| deg p50/p90/max; position m p50/p90)
Windows: 197 (10 s), 122 (30 s), 104 (60 s), 68 (120 s).

| Delay | Estimator | 10 s hdg | 30 s hdg | 60 s hdg | 120 s hdg | 60 s pos | 120 s pos |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 s | (a) fixed 2048 | 1.4/6.2/30.8 | 2.0/8.2/17.2 | 3.1/11.1/24.6 | 5.4/13.5/26.5 | 20.2/43.9 | 21.7/62.2 |
| 0 s | (b) 15 s, tau 120 | 1.1/7.0/30.4 | 2.0/12.6/25.2 | 3.1/22.5/33.5 | 5.4/11.2/21.5 | 21.0/62.9 | 22.8/53.2 |
| 0 s | (b) 30 s, tau 300, prior 60 | 1.4/6.2/30.8 | 2.0/8.1/17.1 | 3.1/11.1/24.9 | 5.4/13.0/26.0 | 20.2/45.0 | 21.1/61.3 |
| 0 s | (c) last stop | 1.6/6.6/29.8 | 4.2/8.2/17.4 | 5.3/9.9/25.8 | 8.0/15.0/23.1 | 24.9/64.4 | 36.2/51.4 |
| 0 s | (b)+(c) | 1.4/6.8/30.2 | 3.0/10.2/22.3 | 4.7/18.5/27.7 | 5.0/12.3/19.6 | 32.3/56.6 | 23.6/50.0 |
| 0 s | (d) KF sigma 1, q 0.002, sd0 1 | 1.3/6.1/30.4 | 1.8/7.8/16.0 | 2.5/8.6/26.4 | 4.0/11.2/24.7 | 17.9/50.2 | 23.0/52.4 |
| 1.3 s | (a) fixed 2048 | 1.4/5.6/52.5 | 1.5/8.5/20.9 | 3.4/13.3/48.7 | 5.1/10.9/23.8 | 19.6/60.3 | 28.4/75.4 |
| 1.3 s | (b) 15 s, tau 120 | 1.5/6.0/52.4 | 2.2/12.6/21.6 | 3.8/25.2/48.3 | 5.6/10.2/23.8 | 26.4/60.3 | 28.6/72.4 |
| 1.3 s | (b) 30 s, tau 300, prior 60 | 1.4/5.7/52.4 | 1.5/8.3/21.1 | 3.4/12.9/48.6 | 5.4/10.2/23.8 | 19.8/60.2 | 28.5/74.5 |
| 1.3 s | (c) last stop | 1.9/6.2/52.2 | 3.9/9.8/24.0 | 6.4/14.9/47.5 | 8.3/16.4/21.8 | 29.0/75.1 | 42.8/76.4 |
| 1.3 s | (d) KF sigma 1, q 0.002, sd0 1 | 1.3/6.2/52.2 | 1.5/8.7/22.7 | 3.4/13.4/47.5 | 4.3/10.4/25.5 | 18.2/65.3 | 27.4/72.9 |

Best case is (d) at delay 0: 60 s heading p90 -23 % (11.1 -> 8.6), but position p90 +6 m, and paired per window 39 better / 29 worse
by more than 1 deg (90th percentile of the per-window change +2.3 deg). At delay 1.3 s the same filter gives no gain (13.3 -> 13.4).
The conservative (b) (30 s intervals, strong prior) changes almost nothing (0 better / 0 worse); the responsive (b) variants double the 60 s
p90 at both delays. (c) is worse at every median: the stationary zero differs from the driving zero (trip2 stops 2045.2 and 2048.8 while
the driving zero is about 2048.6; trip4 stops range 2043.7-2051.7, BETA_DECISIONS rule 3.5).
Even an in-sample constant chosen afterwards (sweep 2045-2050) does not reach -30 % at both delays: the trip2 heading errors at 60 s are
not zero-limited; GPS course noise (integer degrees), turns and the delay assumption dominate.

Zero estimate error against the zero implied by the next 60 s (straight-ish windows, n=47, median/p90 counts): fixed 2048 1.22/4.12
(0 s) and 1.59/4.16 (1.3 s); (b) 1.22/4.36 and 1.64/4.26; (d) 1.15/5.91 and 1.96/4.88. No estimator predicts the next minute's zero
better than the constant.

Qualifying intervals: trip2 has 11 (0 s) / 9 (1.3 s) qualifying 15 s intervals and 3 of 30 s in 514 s of GPS driving, i.e. one per
47-57 s; the Kalman filter has 24-30 qualifying fix updates before each trip4 loss.

## Results, trip4 real tunnels (exit fix; heading error deg / position error m; KF q 0.002, sd0 4)
| Case | Oracle zero (in-sample) | fixed 2048 | (b) 15 s | (d) KF | Pre-loss estimate (b) / (d) |
| --- | --- | --- | --- | --- | --- |
| A1 anchor 28 km/h, 253 s, delay 1.3 | 2047.2 | -7.4 / 80 | -2.7 / 21 | +1.5 / 110 | 2047.5 / 2047.1 |
| A2 anchor 19 km/h, 248 s, delay 1.3 | 2048.3 | +3.0 / 283 | +7.6 / 378 | +11.1 / 449 | 2047.5 / 2047.1 |
| B1 anchor 64 km/h, 98 s, delay 1.3 | 2053.5 | +20.3 / 329 | +22.1 / 355 | +18.3 / 301 | 2047.5 / 2048.5 |
| A1, delay 0 | 2047.0 | -9.3 / 128 | -10.4 / 151 | -12.3 / 193 | 2048.1 / 2048.3 |
| A2, delay 0 | 2048.2 | +1.5 / 252 | +0.4 / 231 | -2.1 / 179 | 2048.1 / 2048.4 |
| B1, delay 0 | 2053.9 | +21.9 / 343 | +21.5 / 338 | +20.2 / 319 | 2048.1 / 2048.5 |

Tunnel A: the pre-loss data (53 s at 17-30 km/h, one qualifying 15 s interval, a GPS course step 108 -> 89 deg at accuracy 1.6 m 5 s before
the loss) gives estimates within 1 count of 2048; results flip sign between the two anchors and the two delays. Tunnel B: the +6 count
shift is real (regression over the 30 s after B: 2051.4-2053.0) but it happened inside or just before the tunnel; the 4 s of GPS between
A and B and the frozen estimate from before A cannot see it. No causal estimator can correct B; this is the largest error in the data.

## Failure cases observed
Turns with an uncertain GPS delay (one trip2 15 s interval gives 2044 at 0 s and 2103 at 1.3 s); integer-degree courses at low speed;
a GPS course step at good reported accuracy before tunnel A (enters the estimate); degraded HDOP 58/78 fixes before the losses (excluded
by the accuracy gate); the stationary zero is not the driving zero; an in-tunnel zero shift (+6 counts) that no pre-loss estimate can see.
Reverse never occurred in a GPS stretch (not tested). A speed-dependent zero (B at 64-78 km/h vs A at 17-30 km/h) is not supported by trip2:
the correlation of interval zero with speed changes sign with the delay (-0.51 at 0 s, +0.30 at 1.3 s).

## Decision and recommendation
No candidate wins: the best gain (-23 % 60 s heading p90, delay 0 only) misses the criterion, is absent at delay 1.3 s, worsens position
p90 and loses in about 30 % of the windows. BETA keeps the fixed zero 2048; nothing is implemented, the journal and the log rate are
unchanged. A future attempt needs drives with long GPS stretches before real tunnels and with exit fixes (ideally several drives on the same
route) and should start from the Kalman form (d), whose state is cheap to bound; it should not be adopted from this data.

## What the data cannot support
Two usable drives of one car; one recording (trip2) without a real outage and one (trip4) with 53 s of pre-tunnel GPS instead of the
whole drive; two real tunnel exits; GPS is not ground truth and its delay is not known (0 and 1.3 s bracket it); results are in-sample for
parameter choice. Not tested: vehicle, phone, DHU, exact ARM, QEMU, the product pipeline (replay_beta), long highway stretches, reverse,
temperature, other cars or firmware.
