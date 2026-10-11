# G3 record-only: first vehicle drive (2026-10-11)

Scope: the first real data of the G3 record-only build (v1.0.0-beta.10, commit 4ab9498), boot `5a9f9b2b`
(trip8-20261011, last boot of `trace.0.jsonl`, 44 min, 5 GPS-loss episodes), and the stored `g3-heading.v1`.
Offline analysis of the journal only. No coordinates are reproduced here. Nothing below is phone, DHU or
next-boot evidence; the next boot of this installation has not been seen.

## Observation (measured)

- 12 `g3_store` write attempts (11 `written`, 1 `cancelled`), all at exact-zero stops of 15 s or more, in 8 distinct
  stops. Only seq 3/4/5 (69.0 deg, unc 38.8 / 51.0 / 52.3) and seq 9 (27.3 deg, unc 20.6) had `hv=1`.
- The stored file holds seq 12: `S`, flags 0 (no heading and no position). The next boot would be UNKNOWN
  (`no_heading`) and no first-course comparison is possible.
- `g3_first` start/t30/course: UNKNOWN `missing` (no store before this boot, expected). First accepted course at
  t_s 560.1 / m_s 4688.6 (source 1, 322.0 deg, 22 km/h; stock bearing equal, `err_s` 0).

## Why `hv=0` (code + log)

`hv` is `G3Sample::heading_valid`: BETA core seeded, snapshot reason OK, finite heading and budget
(`g3_sample`, g3_heading.h). Nothing else (no uncertainty limit, no reverse or FIX condition) is applied at
store time; the 30 deg limit (`G3_UNC_MAX_DEG`) applies only at restore. So `hv=0` = BETA unseeded at the
store tick. BETA was unseeded for three different reasons:

| stop (yaw_stop start, s) | dur s | seq | BETA at the store | cause (measured) |
|---|---|---|---|---|
| 199.6 | 27.8 | 1 | never seeded | anchors 2..555 rejected by gate REVERSE: no reverse message at all until 638.3 s |
| 309.6 | 27.6 | 2 | never seeded | same |
| 970.6 | 87.2 | 3,4,5 | 69.0, unc 38.8 -> 52.3 | valid (`hv=1`) |
| 2164.8 | 21.9 | 6 | unseeded | pipeline reset at 2026.7 s (BAD_INPUT, yaw raw mean >= 4094, five resets) clears BETA and the reverse latch; REVERSE gate again until the next reverse message at 2380.0; reseeded at 2382.0 |
| 2217.4 | 99.4 | 7,8 | unseeded | same |
| 2467.8 | 19.6 | 9 | 27.3, unc 20.6 | valid (`hv=1`) |
| 2511.1 | 43.7 | 10 | unseeded | `beta_cadence_fence` at 2520.5 s (position/send gap 3044 ms; the last position and anchor are near 2517 s, none after) -> `fence_beta()` -> unseeded; no fix ever reseeds it |
| 2567.6 | 70.5 (open at capture stop) | 11,12 | unseeded | same; the car drove 35 m (+6.9 deg yaw) after the fence |

Assumed (not proven): the position stream ended because the AA session ended at parking. If this is the usual
end of a drive, the current rule loses the heading of exactly the stop that matters (the last one).

## Was the heading really unknown? (measured)

Yaw reconstruction: `log_digest` `y1` (1 s sums of moving yaw samples) with the BETA profile zero 2047 and
-0.000658615 rad/count. Check against GPS: 1003 pairs of GPS courses 10-15 s apart at >= 30 km/h, residual
(course change minus integrated yaw) median +1.4 deg, mean -0.2 deg, p90 |residual| 7.6 deg.

Carry error = heading at the end of the stop + moving-yaw integral up to the first GPS course with >= 20 km/h
after departure, minus that course (integer degrees, a GPS course is not ground truth):

| stop s | BETA heading, unc | proposed rule heading, unc | next course (s after stop, km/h) | err BETA | err rule |
|---|---|---|---|---|---|
| 970.6 (stored seq 3/5) | 69.0, 58.3 | 66.3, 4.9 | 4.7 s, 22 | -0.5 | -3.2 |
| 1212.0 (5.4 s, not stored) | 292.6, 34.6 | 290.0, 5.9 | 100.9 s, 20 | -0.1 | -2.7 |
| 1223.7 (4.8 s, not stored) | 293.6, 44.8 | 290.3, 6.7 | 89.8 s, 20 | +0.6 | -2.7 |
| 1270.6 (2.1 s, not stored) | 293.4, 69.7 | 290.9, 11.9 | 45.5 s, 20 | -0.2 | -2.7 |
| 2136.4 / 2164.8 / 2217.4 (across the 2026.7 reset) | unseeded | none (chain broken) | 13.0 / 6.7 / 10.1 s | - | naive propagation: +70.6 / +72.1 / +73.9 |
| 2467.8, 2511.1, 2567.6 | 27.3 / 325.5 / unseeded | 28.4, 13.5 / 324.7, 26.3 / 331.6, 29.3 | none (no fix after ~2517 s) | - | - |

Reading: when the yaw chain is intact the heading at a stop was good to about 3 deg; BETA's honest budget
(35-70 deg) overstated it 50-100x. Two growth terms that do not describe a parked car: BETA keeps adding
k*t and 0.10*|yaw| at a standstill (seq 3 -> 5: +13.5 deg in 50 s while the heading stayed 69.0; yaw noise
sd ~56-75 counts), and low-speed GPS blends raise it (3.0 deg at 957 s to 33.3 deg at 969.5 s while braking).
Across the reset the heading was really lost: the integrated yaw misses a sharp low-speed turn
(about -180 deg of GPS course in 8 s) and errs by about 72 deg. Invalidating there is correct.

n is tiny (4 intact-chain stops, one >= 15 s stop with a following course, 3 broken), one drive, one car.

## Why the first course came at 560 s (measured)

GPS FIX from 80 s (HDOP 1, 20-40 km/h). Policy M (speed, consistency, HDOP) was not the limiter: anchors 2..555
were rejected only by `BETA_GATE_REVERSE` (no reverse message yet; the producer is change-only). The boot's 6
reverse messages (all 0) came at 638.3, 754.7, 842.4, 905.3, 2380.0, 2396.1 s, each 2-10 s before a GPS
loss/return edge (647.5, 758.8, 848.0, 908.2, 2384.0, 2398.0). Assumed: the frame changes with something
tunnel-related (for example automatic headlights). In the older-build boots of the same file the first
reverse message came 10-17 s after BETA armed in two boots and 115 s in a third; this boot waited 594 s. The 198 s NO_FIX start is boot `e5584f62` of an older build (no G3
rows). Gate COURSE once at 638.6, then ACCEPTED at 639.5. The MODEL core stayed `E_NO_SEED` for the whole drive.

## Proposed rule (implemented in the accompanying diff)

1. G3 keeps its own reference: the BETA body heading/uncertainty at a reference tick, carried by the G3Motion
   yaw integral while a wheel moves with the existing rule-5 growth (k = 0.002 rad/s of moving time,
   0.10 per rad of |rotation|), exactly what `propagate()` already does for a restored heading.
2. A valid BETA sample replaces the reference when its uncertainty is not larger than the carried one, or when
   the two disagree beyond the sum of both bounds; otherwise the carried value is kept (no standstill growth).
3. The reference is dropped at a yaw-chain break: `G3Motion::breaks` counts every pipeline `reset_state` and
   every yaw window lost while moving (hole > 250 ms, window > 1 s, non-finite rate). A cadence fence is not a
   break (it does not touch the yaw stream).
4. No heading is stored only when never seeded in this boot or broken since the last valid BETA sample.
5. Unchanged: the 30 deg restore limit, the store triggers, every I/O rule, every send. The `written` row gains
   `ref_s` (age of the reference) and `beta` (BETA's own [heading, unc] in 0.1 deg, or null).

Effect on this drive (anchor-rate simulation of rule 2/3; the product evaluates it every MODEL tick): seq 1, 2,
6, 7, 8 unchanged (no heading); seq 3/4/5 66.3 +- 4.9; seq 9 28.4 +- 13.5; seq 10 324.7 +- 26.3; seq 11/12
331.6 +- 29.3, so the next boot would restore CARRIED (under 30 deg) instead of UNKNOWN. Errors where measurable:
see the table (|err| <= 3.2 deg, inside the stated uncertainty).

Rejected alternative: store BETA's heading with min(BETA unc, carried unc + |difference|) (also an honest bound).
It reproduced BETA's smaller errors (-0.5 .. +0.6) but the |difference| term ratchets over many ticks of
parking manoeuvres (final park 42.0 deg, over the restore limit).

## Other observations (measured)

- Cancelled seq 4: fsync write at 45 s, cancelled at the rename gate, `why` `writer_busy` (journal writer lag
  over 300 ms during the 109.7 ms write). The 5 s retry (seq 5, 70.2 ms with fsync) succeeded.
- `lat_ms`: no fsync 3.5 / 6.6 / 6.7 / 9.5 / 18.3 / 22.0 / 25.3 / 3.9 ms; with fsync 38.6 / 70.2 / 91.8 ms and
  the cancelled 109.7 ms. All far below 500 ms.
- `g3_since` `left` rows carry m 0 and rot 0 (seq 9: rot -0.4 deg): the reference is the record itself and the
  row is written at the first moving tick, so it carries no information. Writing the row at the next parked
  stop (or at the next record) would measure the motion between records.
- seq 12 also lost the position flag (position validity also requires a seeded BETA core).
- Yaw at the stops 2467.8 and 2493.3 s had sd 448 and 683 counts (normal 47-75) with all wheels at exact zero;
  the cause is unknown. G3Motion does not integrate at a standstill; BETA does.
- The 2026.7 s reset followed yaw windows with mean around 3877 counts (about 69 deg/s) and then a window with
  mean >= 4094 during a sharp low-speed turn. Whether that is sensor saturation or an invalid marker is not
  resolved; a tight garage turn may break the chain the same way.

## Verification of the diff

- `make build/test_g3` (host, g++ -std=c++11 -Werror) and `build/test_g3`: 828 checks passed (827 before the
  last added continuity check). New `carried_reference()` cases: never seeded -> no heading; BETA valid then
  unseeded + 30 deg turn -> 130.0 +- 9.29 deg; a larger agreeing BETA budget is not adopted; a disagreeing one
  is; a smaller one is; a break -> no heading until BETA again. `pipeline_motion`: contiguous windows add no
  break; `reset()` adds one. The existing "no heading" store case now sets a break.
- Not run: `make test`, test_worker_beta, the replay and analyzer Python tests, exact-ARM build, QEMU. Not
  vehicle-validated.
