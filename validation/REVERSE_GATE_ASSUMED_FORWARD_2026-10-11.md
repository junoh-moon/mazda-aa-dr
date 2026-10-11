# Rule F: unknown reverse assumed forward for the BETA gates (2026-10-11)

Status: v2 (after the Opus/Codex reviews of v1), implemented in a scratch build on master 77e8d2b (the G3
validity commit).
Host tests, mutation checks and offline replays of the private trips only. Not run on the vehicle, phone,
DHU, exact ARM or QEMU. Not released. No coordinates or raw payloads are reproduced here; times are
CLOCK_MONOTONIC seconds of the boot. GPS fixes are the reference, not ground truth.

## Problem (from validation/G1_FIRST_TUNNELS_2026-10-11.md and G3_FIRST_DRIVE_2026-10-11.md)
- A BETA seed (the start of the heading/position carry that tunnel DR needs) requires a known forward reverse
  state: `reverse_at()` must return a latched 0 and `reverse_exit_seen_` must be true (`evaluate_beta_tracking`).
- 0x118 (REVERSE) is change-only. Messages before the AA session/bus admission are excluded by the runtime
  (`exclude_reverse`), and every reset that clears the latch (BAD_INPUT, MISSING_SENSOR, source epoch, rejected or
  excluded REVERSE, input gap) leaves it unknown until the next message.
- beta.10 drive (trip8 `5a9f9b2b`): no 0x118 until 638.3 s, BETA unseeded for the first ~10 min, the 3 s GPS loss
  (a) at 132.2 s not engaged. Over six continuous-anchor boots 1721 s of 8212 s moving time (21 %) was unseeded,
  1276 s of it (74 %) by the REVERSE gate (G1 coverage table).

## Rule as implemented
Activation (exact definition in the code): the MODEL reverse latch is unknown, i.e. `latch_valid_` is false.
This is the state at boot and after every latch clear: source epoch change, reset/fault (`reset_state`), a
rejected or excluded REVERSE message, an input gap above the keep limit. In the boot case it equals "no
0x118 drained since the AA session/bus admission in this source epoch"; after a reset it lets the rule re-derive
(required by the task and by the G1 counterfactual: trip8 reset at 2026.7 s). The legacy gate (`beta_profile()`)
is unchanged (`assume_forward=false`); only the continuous tunnel policy (`beta_profile_tunnel()`) enables it.

Qualifying fix (all must hold, `Pipeline::beta_assume_forward`, evaluated for every mode 1/2 fix before the BETA
gate):
- data-valid as in policy M: valid GPS position, finite course, HDOP `0 < h <= anchor_hdop_max` (3.0);
- GPS speed `>= BETA_ASSUME_FORWARD_MIN_KMH` = 20 km/h (the existing legacy anchor minimum; recorded reverse
  manoeuvres reached at most 6.0 km/h wheel speed in trip3/4/6/7/8, see below);
- `|GPS speed - causal mean wheel speed| <= wheel_gps_speed_max_diff_kmh` = 4 km/h (the existing wheel/GPS check
  of the legacy gate and of the continuous blend weight);
- continuation of the run (fixes 2 and 3): the policy-M pair test with the previous fix (`beta_pair_continues`:
  strictly increasing utc, step <= 2 s, receipt gap <= 2.5 s, utc/receipt agreement 1 s), the policy-M displacement
  test (`|d - v*dt| <= position_slack_m + 0.5 v*dt`), displacement >= `BETA_HEADING_BASELINE_M` (3 m), course along
  the fix-to-fix displacement within `BETA_COURSE_CHORD_MAX_DEG` (30 deg), and the course change equal to the closed
  yaw integral within `BETA_COURSE_TURN_MAX_DEG + BETA_COURSE_TURN_FRACTION x |turn|` (8 deg + 0.5 |turn|, the M1
  check). A repeated utc second (OEM re-poll) neither counts nor breaks the run; any other failure restarts it.
- `BETA_ASSUME_FORWARD_FIXES` = 3 consecutive qualifying fixes (about 2 s of evidence at 1 Hz).

Effect: the BETA gates (`beta_reverse_at`, the H1 chord reset, `evaluate_beta_tracking`) and a BETA-only
integration (`advance`/`latch_evidence(allow_assumed)`) treat reverse as 0 from the third fix on. The third fix
becomes the first course reference (COURSE), the next fix seeds (about +1 s). The MODEL core, the MODEL latch,
`reverse_latched()`, G3's `reverse` (-1 unknown) and the legacy gate never see the assumption.
Override and clearing: a drained real REVERSE message ends the assumption and its run. A real 0 keeps a seed
made under the assumption (consistent; first value 0 also sets `reverse_exit_seen_`). A real 1 while the
assumption is active CONTRADICTS it (review HIGH, v2): the BETA anchor state is reset at once (`fence_beta()`:
the core re-initialized unseeded, pair baseline, course reference and resync run cleared), so no seed made
under the assumed forward state survives with the latch now saying reverse; BETA re-qualifies only through the
normal path (with latch 1 nothing seeds until a 1->0 transition). Discard was chosen over a 180 deg flip:
the code cannot tell a contradiction (the car was reversing at the seed) from a real engagement of reverse
after a correct assumption, and a flip would be wrong in the second case; the discard is right in both
(it only costs the carry). Mitigation (v2): when a valid real latch is cleared while it says 1
(`drop_latch`: reset/fault, source epoch, rejected/excluded message, input gap), rule F stays off until a real
REVERSE message is latched again (`reverse_assumption_blocked()`); a latch cleared at 0 (the trip8 2026.7 s
reset) does not block. `clear_latch()` (every latch clear above)
ends it; a cadence fence keeps it like the real latch; a different raw epoch ends it. It is re-derived by the
same rule. The assumed interval evidence uses the raw source epoch and the shared local sequence, so a later
real message continues the core's evidence sequence (tested).
Journal: `beta_reverse_latch` row, `domain` "beta", `event` "assumed_forward", `reason` "reverse_assumed_forward",
`fix_ns`, running total `assumed`, `fixes` = 3 x [age s before the last fix, GPS km/h, wheel km/h, |course change -
yaw turn| deg or null], `min_kmh`, `wheel_tol_kmh`, BETA state; a contradiction adds `event` "contradicted",
`reason` "assumed_forward_contradicted", `message_ns`, running total `contradicted`, `discarded` (a seeded core
was reset). Every `beta_anchor` row evaluated under the assumption carries `"rev_assumed":true`. Written by `BetaController::tick` before the anchor
rows (always-kept class). No change to LOCATION content/format/timing, OEM forwarding, errno, send timing or the
taps; BETA can only seed (and then engage) earlier.

## Safety analysis
1. Reversing at >= 20 km/h with an unknown latch (reverse engaged before the AA session, or a reverse=1 message
   lost by a reset; the latter is now excluded by the mitigation when the cleared latch said 1). The rule
   cannot see it: the wheels are unsigned, the course is the body heading + 180 deg and turns with the yaw, so
   the chord and M1 tests pass. BETA seeds 180 deg wrong. Three ways out:
   a. a real reverse=1 message (change-only producer resend, or any later 1): contradiction -> the seed is
      discarded at once (v2). Test `contradicting_reverse_message_discards_the_seed`: reversing at 25 km/h,
      assumed seed, slowing to 10 km/h, reverse 1 at 6.5 s, GPS lost at 7.62 s: no replacement is published
      (v1 published a bearing opposite to the motion: with GPS lost right after, about 333 m error on a 60 s
      straight per the review).
   b. no message: after the stop and forward driving the policy-M resync reseeds the heading after
      `BETA_RESYNC_FIXES` = 5 consecutive chord-confirmed courses disagreeing by more than `BETA_RESYNC_MIN_DEG`
      = 20 deg, mutually within `BETA_RESYNC_CONSISTENT_DEG` = 10 deg, at most `BETA_RESYNC_GAP_NS` = 2.5 s apart,
      with quiet yaw and the M1 motion check (kept as is). Test `reverse_start_is_corrected_by_resync`: resync
      5.4 s after forward motion began; residual window about 5-7 s of forward driving with fixes.
   c. the shift out of reverse sends a 0 message: consistent with the latch, seed kept (case b applies).
   A tunnel entered inside window b integrates backwards (error about 2 x distance). Exposure: needs >= 2 s at
   >= 20 km/h in reverse with no reverse message since the session start. Max recorded reverse wheel speed:
   6.0 km/h (trip3/4), 3.6 (trip8), 2.0 (trip6/7) km/h.
2. Slow reverse with matching wheel/GPS speed: below 20 km/h no fix qualifies. A course flip within a run fails
   the M1 turn test (run restarts).
3. beta.8 stopped-accuracy rule and the E_FRAME fix: unchanged code paths; under the assumption the interval
   has `reverse_active` 0 as with a real forward latch. Private E_FRAME replay passes.
4. G3: a rule F seed makes `heading_valid` true earlier, as intended; a contradiction discard makes it false
   at once (BETA unseeded). G3 still reads the latch as unknown (-1, treated as forward).
5. Cost of the discard: a correct assumption followed by a real engagement of reverse also loses the carry
   (trip4 4209.4 s below: the reverse-entry loss at 4212.6 s is no longer engaged).
- MODEL isolation: the MODEL core still needs a real message.

## Tests (host, scratch build on 77e8d2b)
- `make test-navigation`: `test_beta` 71975 checks (65580 before rule F; existing tests unchanged), test_navigation
  3977, test_live_pipeline 819, gyro_bias, gps_wheel, holdout, channel tests pass. Rule F tests: v1 set (seeding,
  2 fixes, 19.9/20 km/h, 4.5/3.5 km/h wheel mismatch, repeated utc, course step/across displacement/yaw turn,
  early real 0/1 = records identical to rule F off, forward message continuity, reverse start + resync, course
  flip, reset/fence/exclusion) plus v2: contradiction discards the seed (and a consistent 0 keeps it); the
  existing override test now also asserts the discard; latch cleared at 1 blocks rule F (reset and excluded
  message), at 0 does not, a real message lifts the block; raw epoch change clears the assumption after a
  latch-keeping reset.
- `test_g3` 834 checks: new `yaw_chain_breaks` (a 500 ms yaw silence while moving: the MISSING_SENSOR reset plus
  the hole after it = 2 breaks; stationary = 1). The window > 1 s and non-finite-rate checks cannot be reached
  from raw input (the core limits sample_age_max to 250 ms, which faults first; rates are finite by
  construction): defensive, untested.
- `test_worker_beta`: all 16 cases incl. `main_no_reverse` (one assumption row before the first seed row).
- `test-replay-beta` (17 tests incl. the private E_FRAME trip6 replay; 1 private G3 skip) with the new
  `test_replay_reverse_fence.py`; tools tests 164 (2 skips) incl. the analyzer rule F test.
- Mutations (all fail the named test; each restored): discard removed (published opposite bearing), mitigation
  removed, raw-epoch clear removed, G3 hole check removed (`test_g3`), replay boundary sort removed (fence test),
  analyzer reason from an earlier row instead of the seed row; v1 set re-run: profile off, 20 km/h bound, M1
  turn, wheel check, reset keeps the assumption. v1-only results kept: chord check, 3->2 fixes, override paths.
  Removing only the drain-side clear of the assumption is not detected (the `latch_valid_` guards make it
  redundant).
- Not run: full `make test`, ARM suite, QEMU, DHU, vehicle.

## Replay (tools/replay_beta, --real-only, product tunnel profile)
Options: `--assume-forward on|off` (off = beta.10 counterfactual), `--seed-report` (adds `beta_seed`),
`--reverse-fence feed|runtime`. `runtime` (v2) applies the recorded `shadow_session`/`shadow_bus` rows sorted by
time across trace files (v1 kept file order, trace.0 before trace.1, and could stop early), admits a REVERSE
message only while both fences are open and its receipt time and positive transport time are not before the
newer `raw_since_ns`, otherwise `exclude_reverse()`, and resets the pipeline at a recorded CHANGED boundary
(new source/session epoch). NOT reproduced: the same fence for wheel/yaw events and POSITION callbacks
(including the request-level `ModelSession`/`ModelBus::reject` checks), the worker's turn timing of boundary
reads, audit faults; motion outside persistent raw windows (MISSING_SENSOR resets in the replay clear the latch
where the vehicle did not, so unseeded time is over-stated there: trip6 losses were ENGAGED on the vehicle).
Default (feed) feeds every recorded message. trip4 is a beta.3 recording replayed with current logic.

| Boot | First seed off -> on (s) | Moving seeded off -> on (s, of moving) | Assumptions | Losses engaged off -> on |
| --- | --- | --- | --- | --- |
| trip2 07987d33 (SHADOW, polls) | 953.5 -> 953.5 | 336.8 -> 336.8 (448.7) | 0 | 1 -> 1 |
| trip3 843d1bbf | none -> none | 0 -> 0 (407.1) | 0 | 0 -> 0 |
| trip4 65258c5f | 2126.1 -> 1828.9 | 61.0 -> 188.9 (328.0) | 2, 1 contradicted | 1 -> 2 (1879.9 new) |
| trip6 82d37f8a | none -> 810.8 | 0 -> 174.6 (233.8) | 4 | 0 -> 2 (vehicle: 2 engaged) |
| trip6 322800ef | none -> 1493.8 | 0 -> 141.9 (221.9) | 2 | 0 -> 2 (vehicle: 2 engaged) |
| trip7 e5584f62 | none -> none | 0 -> 0 (59.7) | 0 | 0 -> 0 |
| trip8 1c1de788 | 119.5 -> 119.5 | 1.8 -> 1.8 (6.1) | 0 | 0 -> 0 |
| trip8 0914e18f | none -> 186.4 (runtime); 182.3 both (feed) | 0 -> 10.2 (40.1) | 1 (runtime) | 0 -> 0 |
| trip8 5a9f9b2b | 639.7 -> 83.2 | 230.7 -> 516.8 (652.1) | 6 | 4 -> 5 ((a) 132.2 new) |

- Changed against v1: trip4 only. The real reverse 1 at 4209.4 s contradicted the assumption of 4160.5 s, the
  seed was discarded, and the reverse-entry loss at 4212.6 s is no longer engaged (v1: engaged through the
  reverse refresh path); replaced sends 69 -> 58, moving seeded 194.2 -> 188.9 s. The runtime fence gives the
  same numbers as feed except trip8 0914e18f (its only early message was received before the bus admitted input).
- Byte identity (new build default vs base build, report and journal) holds only for the non-applicable boots
  trip2, trip3, trip7, trip8 1c1de788 and 0914e18f. Elsewhere rule F applied; note that an early real message
  does not prevent it: after a later reset/gap the latch is unknown again and rule F can fire (trip8 794.9 s
  after the 638.3 s message, trip6 82d37f8a after the 72 s messages), and every send after the first applied
  assumption can differ (different carry). With `--assume-forward off` the report equals the base report (minus
  `beta_seed`) in all nine boots. 0 violations everywhere.
- trip8 (a): engaged 132.3 s, 2 replaced sends, return jump 4.3 m; assumption at 82.2 s from 20/21/21 km/h fixes
  (wheel 21.6/21.6/21.5, turn errors 0.8/0.6 deg); reassumed at 594.4, 794.9, 1973.9, 2037.1 (11 s after the
  BAD_INPUT reset at 2026.7, whose latch was 0: not blocked; vehicle reseed 2382.0) and 2330.8 s. Later return
  jumps: (c) 47.5 -> 50.3 m, (e) 32.3 -> 30.8 m, (d) 38.1 m (unchanged from v1).
- Vehicle-comparable new engagement: (a) only.

## Analyzer
`tools/analyze_logs.py` accepts the beta-domain `assumed_forward` and `contradicted` latch rows (other
beta-domain latch rows stay a violation), reports `beta.seeds` and `beta.reverse_assumed_forward` (rows and
reasons, incl. `assumed_forward_contradicted`) and prints per boot
`BETA seed: boot N first seed at t s (reason reverse_assumed_forward|reverse_latch)`. v2: the reason comes from
the seed row itself (`"rev_assumed":true`), not from an earlier assumption row (a reset in between and a seed
after a real forward message is `reverse_latch`).

## Limits
- Fast reverse travel without any real message is not detected (failure mode 1b): 5-7 s residual window.
- The discard also drops a correct carry when reverse is really engaged after a correct assumption.
- Thresholds are existing constants; 3 fixes is a choice, not derived from data.
- Replay coverage is window-limited, not vehicle coverage; the runtime fence is reproduced for REVERSE only.
- Offline only; one car, one firmware.

## First-drive checklist
- `beta_reverse_latch` `assumed_forward`/`reverse_assumed_forward` within a few seconds of the first >= 20 km/h
  driving with GPS after the session start (and after a BAD_INPUT reset whose latch was 0), three fixes with
  wheel/GPS within 4 km/h; the first seed row (`h`[0] = 1, `"rev_assumed":true`) about 1 s later.
- Analyzer: `BETA seed: boot N first seed at t s (reason reverse_assumed_forward)` near the first fast driving.
- Any `contradicted` row: check `discarded`, the wheel speed at `message_ns` and whether it was a parking
  reverse (expected cost) or a fast-reverse seed (the protected case).
- No assumption row after a reset whose cleared latch was 1 until a real message (mitigation).
- A `resync` heading source shortly after a boot-time seed: candidate failure-mode-1b event.
