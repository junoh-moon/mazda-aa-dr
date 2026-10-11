# Rule F: unknown reverse assumed forward for the BETA gates (2026-10-11)

Status: v5 (incremental on v3+v4 after the Codex check of v4: one G3 MEDIUM), scratch build on master 25c6025
+ v3 + v4.
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
### v3: seed origin tracking (Codex re-review HIGH)
v2 tied the discard to the assumption flag, but the flag and the seed it produced are decoupled: an excluded
REVERSE message (e.g. received before the bus boundary) cleared the flag and kept the seed, and a later real
reverse 1 whose transport time equalled the BETA frontier was drained after `advance()` returned early at
`end <= frontier` (before the unknown-latch check), so the discard was skipped and the seed stayed with latch 1
(integration and travel bearing opposite to the motion). v3:
- `seed_from_assumption_`: set when an accepted fix takes its heading from a GPS course (seed or resync) while
  the reverse state is assumed (a refresh under the assumption also marks it); cleared only by a real 0 (the
  confirmation) or by a BETA reset (`reset_beta`: reset/fault, cadence fence, discard).
- A real reverse 1 while `seed_from_assumption_` is set discards the seed (`discard_assumed_seed`, cause
  `reverse_message`), however the assumption flag was cleared before.
- Any loss of the assumption without a confirmation that does not reset BETA anyway (an excluded or rejected
  REVERSE message, whose value may be 1; a raw epoch change) discards an assumption-derived seed at once (cause
  `assumption_lost`). Chosen over keeping the origin armed: it leaves no interval in which an unconfirmed
  seed lives with an unknown latch (and so no reliance on the next message arriving), and the cost is the same
  carry loss as a contradiction. A seed confirmed by a real 0 is never discarded (flag already clear).
- `advance()` disables a BETA-only core whose reverse state is unknown and not assumed before the
  `end <= frontier` early return, so a real message drained at the frontier never finds it seeded.
- The discard also breaks the G3 yaw chain and counts `G3Motion::discards` (see G3 below); the worker revokes
  the stored candidate in the same turn (see journal).
Remaining paths that clear the assumption: resets (reset_beta drops the seed anyway) and real messages.

### v4: G3 taint rule (Codex final re-review)
v3 broke the G3 chain only inside `discard_assumed_seed`, so (a) a cadence fence (or any reset) that dropped an
assumption-derived seed cleared the origin without telling G3, and G3 kept carrying the reference into a parked
store; (b) a rejected REVERSE message faults the pipeline, and `fault()` resets BETA before `assumption_lost()`
runs, so the later check saw a cleared origin and skipped G3; (c) a write already in flight was not cancelled.
v4:
- Taint rule: while `seed_from_assumption_` is set, every BETA heading, and every G3 reference adopted or
  carried from it, is tainted (derived from an unconfirmed assumption). The origin ends in exactly one place,
  `Pipeline::end_assumed_origin(confirmed)`: a real 0 confirms it (no effect); every other end (discard on a real
  1 or a lost assumption, BETA reset/fault via `reset_beta`, cadence fence, a new non-assumed course seed) counts
  `G3Motion::discards` and breaks the yaw chain, independent of the order of the clearing paths.
- G3 then drops its reference (break), drops a pending request with a heading built before the taint end,
  cancels an in-flight write before its next gated step (`G3Tracker::discard_cancel`, added to the worker's
  cancel reasons in runtime.cpp `g3_pump` and in the replay), and rewrites a record written in this boot with
  such a heading at the next parked tick (>= 15 s exactly-zero stop, idle rule, 5 s rate limit) with the current,
  normally invalid, heading. Over-approximation: any record with a heading written before the taint end is
  treated as tainted (records from a confirmed reference are rewritten too; harmless, the car has moved).
- Windows (record-only, never sent): a tainted heading stays on disk from its write until the next parked tick
  after the taint end; a write past its last gated step (renamed) completes and is rewritten later; if the boot
  ends before that parked tick (or the origin is neither confirmed nor ended, e.g. power-off with an
  unconfirmed seed) the next boot can restore the tainted heading as CARRIED (journaled only; the first M
  course check REJECTS a wrong one).
- LOW: the stored candidate is now revoked BEFORE the `contradicted` row is written (then the ENGAGED
  withdrawal). `adapter::invalidate()` is a lock-free generation bump: a send that starts after it cannot select
  the old candidate. Residual race, stated precisely: the discard happens inside `Pipeline::drain` and the
  revocation in the `BetaController::tick` that follows in the same worker turn; an OEM send that selects the
  candidate in that interval (sub-millisecond on the host) can still go out with the old seed within its 500 ms
  lease, as for every other worker-side withdrawal. Making it atomic would need the pipeline to touch the
  adapter, which the layering forbids.

### v5: G3 store stop on the permanent fault (Codex check of v4)
A permanent fault (journal failure or audit fault, runtime.cpp worker) resets the pipeline, which ends an assumed
seed origin (a G3 taint end), and then sets `shadow=false`; the G3 block is guarded by `shadow`, so v4's
`discard_cancel()` and the pump never ran again and a write already past its fsync start kept the old cancel
generation and could rename a tainted heading that was never rewritten. v5 delivers the end to the store
thread unconditionally, outside the shadow-guarded block, whenever `j.failed` or `audit_fault` is seen: the
cancel generation is bumped and the stop flag set, so the in-flight write stops before its next gated step
(fsync or rename) and no later write is accepted. Other `shadow=false` paths: capture stop already sets the stop
flag before it; the initial `shadow` (startup) never starts G3. Worker test `g3_fault` (no REVERSE message: the
G3 headings are tainted; the 60 s write blocked 3 s in its fsync step; audit fault at 63 s): the write ends
STOPPED at the rename, the store keeps seq 1. Residual (record-only, as for a power-off): seq 1, written at the
15 s stop before the taint end, holds a tainted heading and cannot be rewritten after the fault (no further G3
I/O in this boot); the next boot may restore it as CARRIED (journaled only; the first M course check rejects a
wrong one).

Journal: `beta_reverse_latch` row, `domain` "beta", `event` "assumed_forward", `reason` "reverse_assumed_forward",
`fix_ns`, running total `assumed`, `fixes` = 3 x [age s before the last fix, GPS km/h, wheel km/h, |course change -
yaw turn| deg or null], `min_kmh`, `wheel_tol_kmh`, BETA state; a discard adds `event` "contradicted",
`reason` "assumed_forward_contradicted", `cause` (`reverse_message`|`assumption_lost`), `message_ns`, running total
`contradicted`, `discarded` (a seeded core was reset). In the same `BetaController::tick` (which follows the
drain in the same worker turn, before any publish of that tick) the stored candidate is revoked FIRST
(`adapter::invalidate()`, a lock-free generation bump, and an empty candidate; a speed overlay is left alone: it
does not use the heading), THEN the `contradicted` row is written, THEN an ENGAGED episode is withdrawn
(`ENGAGED>WITHDRAWN:assumed_forward_contradicted`). Residual race: an OEM send that had already selected the old
candidate before the revocation (between the discard inside the drain and the revocation in this tick, or
concurrently with the generation bump) can still go out with the old seed within its 500 ms lease, as for every
other worker-side withdrawal (e.g. model_not_ready); a send that starts after the revocation cannot. Worker test `contradict`: no replaced send after the row. Every `beta_anchor` row evaluated under the assumption carries `"rev_assumed":true`. Written by `BetaController::tick` before the anchor
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
4. G3: a rule F seed makes `heading_valid` true earlier, as intended. v3: a discard is a yaw-chain break
   (`G3Motion::breaks`), so G3's carried reference is dropped (v2's `fence_beta()` alone kept it, and G3 carried
   a possibly 180 deg wrong heading into a parked store). G3 also counts `G3Motion::discards`: a pending request
   whose heading was built before a discard is dropped and requested again; a record written in this boot with
   such a heading is rewritten at the next parked tick (rate limit 5 s) with the current, normally invalid,
   heading. Record-only risk: if the boot ends before that parked tick (no 15 s stop after the discard), the
   store keeps the wrong heading and the next boot restores it as CARRIED (journaled only, never sent; the first
   M course would REJECT it). A record from a previous boot is never touched. G3 reads the latch as unknown (-1,
   treated as forward).
5. Cost of the discard: a normal parking reverse after a correct assumption (no real 0 seen yet) also discards
   the carry; a tunnel entered before BETA re-seeds (after the 1->0 transition and a course with fixes) then
   gets the stock behaviour (trip4 4209.4 s below: the reverse-entry loss at 4212.6 s is no longer engaged).
6. The 5-7 s resync figure (1b) comes from one synthetic test case (straight, steady 25 km/h, 1 Hz fixes, quiet
   yaw); it is not a recovery bound. Turning, slow driving, fix gaps or HDOP rejections lengthen it.
- MODEL isolation: the MODEL core still needs a real message.

## Tests (host, scratch build on 7c8829e, v3)
- `make test-navigation`: `test_beta` 72345 checks (65580 before rule F; existing tests unchanged), test_navigation
  3977, test_live_pipeline 819, gyro_bias, gps_wheel, holdout, channel tests pass. Rule F tests: v1 set (seeding,
  2 fixes, 19.9/20 km/h, 4.5/3.5 km/h wheel mismatch, repeated utc, course step/across displacement/yaw turn,
  early real 0/1 = records identical to rule F off, forward message continuity, reverse start + resync, course
  flip, reset/fence/exclusion), v2 set (contradiction discard, consistent 0 keeps the seed, mitigation, raw epoch)
  and v3: `excluded_message_then_reverse_at_the_frontier_discards` (the review path exactly: 25 km/h reverse seed,
  10 km/h, an excluded message, then a valid reverse 1 whose transport time equals the BETA frontier: the seed
  is discarded at the exclusion (`assumption_lost`), the message is accepted, no replacement after the GPS loss,
  one G3 discard) and `unknown_latch_disables_beta_even_at_the_frontier` (a confirmed seed, an excluded
  message, a drain that integrates nothing: BETA disabled, no discard).
- `test_g3` 843 checks: `yaw_chain_breaks` (v2) and v3 `rule_f_discard` (a record written with a heading is
  rewritten without it at the same stop after a discard; a pending request with such a heading is dropped and
  re-requested without it).
- `test_worker_beta`: 17 cases incl. `main_no_reverse` and v3 `contradict` (assumed seed, ENGAGED outage, real
  reverse 1: `contradicted` row (cause `reverse_message`, discarded) immediately followed by
  `ENGAGED>WITHDRAWN:assumed_forward_contradicted`; 6 replaced sends before the row, 0 after).
- `test-replay-beta` 17 tests (private E_FRAME trip6 replay included; 1 private G3 skip); tools tests 175
  (2 skips) incl. the analyzer rule F test.
- v4 tests: `g3_taint_ends_on_every_unconfirmed_origin_loss` (test_beta: cadence fence, rejected REVERSE message
  with the fault-before-notification order, reset: one taint end and a break each; a real 0 first: none);
  `rule_f_discard` / `rule_f_discard_in_flight` (test_g3: rewrite of a stored record, drop of a pending request,
  a taint end inside the fsync step cancels the rename of the in-flight write). test_beta 73051, test_g3 854.
  v4 mutations (each fails, each restored): taint count removed from `end_assumed_origin`; v3 placement
  (counted only in `discard_assumed_seed`, order-dependent) -> fence/reject/reset paths fail; `discard_cancel`
  disabled -> in-flight test (rename not cancelled); stored-record rewrite disabled -> `rule_f_discard`. The
  revoke-before-row order (LOW) has no deterministic test (the race is between threads); it is a code-order
  guarantee. v4 runs: test-navigation, test_worker_beta 17 cases, test_g3 854, test-replay-beta 17 (1 skip),
  tools 177 (2 skips) pass; the nine private replays give byte-identical reports to v3 (54 reports; v4 changes
  only G3 state and the order of the revocation).
- Mutations (each fails the named test, each restored): v3 origin tracking removed (v2 flag check, no
  `assumption_lost`) -> boundary test; early-return order restored -> frontier test; pipeline G3 break/discard
  removed -> boundary test; G3 tracker pending/stale handling removed -> `rule_f_discard`; worker
  withdrawal/revocation removed -> worker `contradict`; discard on a real 1 removed -> override test; mitigation
  removed; 20 km/h bound; M1 turn test. Earlier runs (v1/v2, code unchanged there): profile off, wheel check, chord
  check, 3->2 fixes, reset keeps the assumption, raw-epoch clear, G3 hole check, replay boundary sort, analyzer
  reason. Removing only the drain-side clear of the assumption flag is not detected (redundant with the
  `latch_valid_` guards).
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

- Which mode: every row of the table is `feed` except the 0914e18f "runtime" cell; `runtime` gave the same
  numbers as `feed` in the other eight boots. v3 changed no report against v2 (54 reports compared
  byte for byte); only the trip4 journal differs (the `cause` field of its `contradicted` row and the following
  summaries). Changed against v1: trip4 only (the real reverse 1 at 4209.4 s contradicted the assumption of
  4160.5 s; the reverse-entry loss at 4212.6 s is no longer engaged; replaced sends 69 -> 58).
- Byte identity is an execution result per drive, not a general property: the new build's default (`feed`)
  report and journal equalled the pre-rule-F base build (77e8d2b logic) for trip2 07987d33, trip3 843d1bbf,
  trip7 e5584f62, trip8 1c1de788 and trip8 0914e18f, where rule F never applied. In the other four boots it
  applied; an early real message does not prevent it (after a later reset/gap the latch is unknown again: trip8
  794.9 s after the 638.3 s message, trip6 82d37f8a after the 72 s messages), and every send after the first
  applied assumption can differ (different carry). With `--assume-forward off` the report equalled the base
  report (minus `beta_seed`) in all nine boots. 0 violations everywhere.
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
- Fast reverse travel without any real message is not detected (failure mode 1b); the 5-7 s resync seen in one
  synthetic case is not a recovery bound. A late real reverse 1, an excluded or rejected message corrects it at
  once (discard).
- The discard also drops a correct carry when reverse is really engaged (normal parking) after a correct
  assumption that no real 0 had confirmed: a tunnel entered before BETA re-seeds gets the stock behaviour.
- The candidate revocation happens in the worker turn of the discard, after the drain; a concurrent OEM send in
  between can still use the old candidate (500 ms lease), as for other worker-side withdrawals.
- G3 (record-only): a heading stored before a discard stays on disk if the boot ends before the next parked
  tick, and the next boot would restore it (journaled only).
- Thresholds are existing constants; 3 fixes is a choice, not derived from data.
- Replay coverage is window-limited, not vehicle coverage; the runtime fence is reproduced for REVERSE only.
- Offline only; one car, one firmware.

## First-drive checklist
- `beta_reverse_latch` `assumed_forward`/`reverse_assumed_forward` within a few seconds of the first >= 20 km/h
  driving with GPS after the session start (and after a BAD_INPUT reset whose latch was 0), three fixes with
  wheel/GPS within 4 km/h; the first seed row (`h`[0] = 1, `"rev_assumed":true`) about 1 s later.
- Analyzer: `BETA seed: boot N first seed at t s (reason reverse_assumed_forward)` near the first fast driving.
- Any `contradicted` row: check `cause`, `discarded`, the wheel speed at `message_ns` and whether it was a parking
  reverse (expected cost) or a fast-reverse seed (the protected case); when ENGAGED, the next row must be
  `ENGAGED>WITHDRAWN:assumed_forward_contradicted` and no replaced send may follow it in that outage.
- After a discard, a `g3_store` `written` row with `hv` 0 at the next parked stop if a heading had been stored.
- No assumption row after a reset whose cleared latch was 1 until a real message (mitigation).
- A `resync` heading source shortly after a boot-time seed: candidate failure-mode-1b event.
