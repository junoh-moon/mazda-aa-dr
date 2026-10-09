# DHU experiment 7, Tier 1 result (2026-10-09)

Plan: validation/DHU_NAVER_EXP7_PLAN_2026-10-09/ pinned at dc6717eaefc9bae27fbefb8e7c6ed8d0d72741df. Executed by the owner's notebook agent
(S25 + USB + Google DHU, Naver Map; synthetic coordinates, not a vehicle, not the wireless dongle). Data (59 MB, 231 photos) stays private; this record
is a summary. Tier 2 and Tier 3 had not been run at the time of this record.

## Result (operational labels from screen reading, 7 valid runs, 0 excluded)
| Condition | Input in the tunnel (accuracy 40 m) | Label | Repeats |
| --- | --- | --- | --- |
| H0 | correct bearing (control) | FOLLOWS-ON-ROUTE, CLEAN-EXIT | r1 and r2 agree (CONFIRMED) |
| H3 | constant +90 deg bearing, correct position | FOLLOWS-ON-ROUTE, CLEAN-EXIT | SINGLE |
| H4 | constant 180 deg bearing, correct position | FOLLOWS-ON-ROUTE, CLEAN-EXIT | SINGLE |
| R1 | yaw-zero error +0.15 deg/s, bearing and position drift together (end: +25.6 deg, 527 m lateral, exit jump 537 m) | FOLLOWS-ON-ROUTE, CLEAN-EXIT | SINGLE |
| R2 | -0.15 deg/s (end -25.6 deg, -527 m) | FOLLOWS-ON-ROUTE, CLEAN-EXIT | SINGLE |
| R3 | +0.30 deg/s (end +51.3 deg, 1,002 m lateral, exit jump 1,054 m) | FOLLOWS-ON-ROUTE, CLEAN-EXIT | SINGLE |

No harmful label (REROUTE, STOPPED-NAVIGATION, ROAD-SNAP-WRONG, WRONG-DIRECTION-ARROW, JUMP-AT-EXIT) in any of the 231 photos. The arrow stayed blue and
aligned with the route (visually < 15 deg), the map was not rotated, the remaining distance and the next-guidance distance followed the TRUE route progress
(within +-100 m) rather than the drifted sent position, and the exit (GPS return with the true position) showed no visible jump. I opened the H4 (t=100) and
R3 (t=190) photos myself: consistent with the report.

## What this does and does not say
- Naver kept guiding for reported bearings wrong by 90 and 180 deg and for sideways drifts of 0.5-1 km at accuracy 40 m. The displayed progress follows the true
  route, so on this evidence the benefit is mostly that the navigation keeps running, not that Naver uses our position or bearing. It does not show that
  Naver ignores them in other situations (parallel roads, junctions inside the displayed route, re-route triggers).
- Pre-registered decision rows 3-4 (harm at 20 deg / at 45 deg or more) did not occur for the tested conditions. Rows 5-7 need Tier 2 (H1, H2, H5, H6, H7, H8,
  R0) to be complete; the tested conditions are the extreme ones, so a monotonic expectation says the milder ones are normal, but that is not measured.
- Not tested yet and relevant to traffic jams in tunnels: the stopped forms (H5 wrong bearing + standstill, H8 correct bearing + standstill), the first-run
  effect (H6) and the entrance burst (H7).
- Caveats from the executing agent: one repeat per condition except H0; Google Play services 26.39.63 differs from experiment 6 (26.37.65); the first recent
  destination was not the planned one and the destination was searched via the DHU keyboard (preparation UI only); photos every 10 s (2 s at entrance/exit);
  bearing angles are visual estimates; not the real wireless path.

## Product consequence (no code change)
No evidence for a heading-confidence limit, a time cap or a speed-only fallback on the tested range; v1.0.0-beta.6 engagement policy stays. Re-evaluate after
Tier 2 and, above all, after the first real drive.
