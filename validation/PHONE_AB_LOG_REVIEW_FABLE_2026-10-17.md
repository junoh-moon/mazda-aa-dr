# Independent adversarial review: phone-side A/B log of OBD yaw versus the phone gyro (commit e2a0d35)

Reviewer: Claude Fable 5.1 (independent review agent), 2026-10-17. Read-only; all runs in a scratch copy of HEAD
(`git archive e2a0d35`) in a scratch directory. No vehicle, phone or adapter.
Host: javac/java 21.0.7 (`--release 11`), python3.12, 4 cores, 1-minute load 3.2-3.4 during the Java runs.

Subject: `feat(phone-imu): phone-side A/B log of OBD yaw versus the phone gyro on one clock...` (12 files:
`AbCompare.java`, `RollingLog.java`, `SenderService.java`, `MainActivity.java`, `AbTests.java`, `test.sh`,
`tools/pimu_ab.py`, `tests/tools/test_pimu_ab.py`, two docs, README, design note section 16).

## 0. Verdict in one paragraph

The RECORD-ONLY claim holds: by reading `SenderService.send` (`SenderService.java:389-395`), `AbCompare.admit`
(`AbCompare.java:54-58`) and `SourceSelector.admit` (`SourceSelector.java:72-85`), the only change on the send path is
that `sel.admit(from, w, now)` is now called inside a static helper that returns its result unchanged and then adds the
producer window's numbers to an accumulator under a second lock; nothing can alter, delay, drop, reorder or reuse a
window, flags or `frame_reset`, and no I/O or allocation sits on the sensor/OBD thread. The wire-identity test passes
(886/0, three runs). I found **no defect on the wire path and no concurrency defect in the Java accumulator**. The real
defects are in the **analyzer and its pre-registered rule** (three CONFIRMED by reproduction): (A) the parked-interval
gate uses the OBD rate itself, so an OBD nominal-zero offset above 0.30 deg/s (about 8 counts, inside the range this
project has measured for 0x078 on the owner's car: stationary medians 2037.6-2053.2 by boot) makes the parked-zero
section silently empty and disables the `unusable_zero_range` check; (B) an OBD receipt lag of 0.3-1 s biases the
estimated scale low by roughly lag/10 s times (1 - row autocorrelation), which is the same size as the 3 % `agree`
tolerance, so `agree`/`scale_disagree` can be reported wrongly in either direction; (C) the analyzer does not
de-duplicate rows, and because the A/B ring is never cleared on export, passing two exports (or the same one twice)
double-counts rows and turned a `not_decidable` case into `agree` in my reproduction. None of these changes what the
car or Naver receives; they affect what the owner will be told about the comparison.

## 1. Wire safety (question 1): nothing found

Read: `SenderService.send` holds `synchronized (sel)` exactly as before; `AbCompare.admit` calls `sel.admit` first and
only then `ab.add(from, w, x != null, now)` with the **producer's** window `w` (not `x`), so the selector's switch
`frame_reset` is neither seen nor counted (test `flags()` checks this). `add()` (`AbCompare.java:72-92`) does field
arithmetic only: no allocation, no I/O, no call back into the selector; it is `synchronized` on the accumulator, which
is held by the main thread only during `poll()`/`take()`/`summary()` (microseconds; `format()` builds one 185 B string).
Lock order is selector -> accumulator on the producer threads and accumulator alone on the main thread (`tick`
takes `sel.update()` and `ab.poll()` sequentially, never nested) - no inversion. `pending` is an `ArrayDeque` capped at
`MAX_PENDING = 8` by `poll()` (`AbCompare.java:102-103`): when the tick does not run no row is created at all (rows are
only created in `poll()`), so there is no unbounded growth; when the tick runs but nobody drains (impossible in the
current wiring, `tick` drains right after `poll`) the ninth row is dropped and counted. If `ab.add` threw, the window
would already have been admitted but not offered, but there is no throwing path (no allocation, `w` is never null).
Overflow: all sums are `long`; 10 s of `Integer.MAX_VALUE` per window is tested (`rowFormat()`); the analyzer uses
Python ints. `CLOCK.nanos()` is evaluated once per window as before. **Conclusion: the send path is byte-identical.**

Note (not a defect): the wire-identity test re-implements the `send()` loop on the host; `SenderService` itself never
ran, as the commit says. The 1-line change in `send()` is simple enough that reading suffices.

## 2. Concurrency and lifecycle (question 2)

- Listener ordering: `Link.loop` (`Link.java:141-157`) calls `listener.connected(s)` and `listener.disconnected(s, ...)`
  sequentially on the single `imu-link` thread, so `connected()`'s `ab.reset(); ab.setEnabled(...)` and
  `disconnected()`'s `ab.reset()` cannot interleave across sessions. Nothing found.
- PLAUSIBLE, cosmetic: after `disconnected()` sets `active = null` and resets the accumulator, one sensor event that
  already read `active` non-null (`onSensorChanged`, `SenderService.java:365-377`) can still call `send()` and open a
  new one-window interval (`enabled` stays true). The tick keeps running (`link != null`), so 10 s later one row
  `g_n=1 ... miss=obd` with `iv_ms` >= 10000 is written; the analyzer excludes it (`low_coverage`). The same event
  also calls `s.offer(x)` on the closed session (pre-existing). No data corruption; at most one junk row per disconnect.
- Service stop (`onDestroy`, `SenderService.java:484-503`): `main.removeCallbacks(tick)` happens first, so up to 8
  pending rows plus the open interval are lost (<= 90 s). Accepted by design ("service stop: the open interval is
  dropped"); the pending queue is not flushed, which the docs do not say. Cosmetic.
- Enabled-but-no-OBD cases: `connected()` enables the accumulator on `Prefs.obdAddress != null` even when `startObd()`
  then falls back to `SourceSelector(false)` (bad address, deferred by the parked check, `SenderService.java:402-429`).
  Result: `miss=obd` rows every 10 s (documented, 16.2). Not a defect, but it means the ring also fills while the
  adapter is unplugged, at the shorter one-sided row length.
- Time base: binning uses `CLOCK.nanos()` = `elapsedRealtimeNanos` (monotonic, counts through sleep) passed into
  `admit`, identical for both producers ("one clock" holds at the receipt level; the gyro window's `tEndNs` is sensor
  time and the OBD window's is receipt time, both ignored for binning). The tick is `postDelayed(1000)` on `uptimeMillis`
  and the partial wake lock is held while connected, so a sleep gap only lengthens one interval (`iv_ms` shows it, the
  analyzer excludes > 15 s) and the next interval opens at the close time. No giant-interval aliasing beyond the one
  excluded row. Rows after a phone reboot restart `t=`; the analyzer's `consecutive()` splits parked runs on `dt <= 0`.
- `lastRowNs` guard (`AbCompare.java:96`): dead code. Because a closed interval reopens at the close time and a reset
  interval reopens at the next `add`, `now - startNs >= INTERVAL_NS` already implies `now - lastRowNs >= INTERVAL_NS`.
  My mutation deleting the line passed all 886 checks (see section 6). Harmless; the comment calls it defensive.
- Duplicate or missing rows inside the app: none found; the thread test conserves every sum and I re-ran it 3 times.

## 3. RollingLog.ab() (question 3)

- Ring: `event()` rotates when `current.length() + b.length > half` with `half = 256 KiB`, deleting the previous half
  first; total <= 512 KiB; a line longer than a half is dropped. Correct; the cap mutation (section 6) is caught.
- PLAUSIBLE, pre-existing pattern: export has no mutual exclusion with the writer. `MainActivity.exportLog`
  (`MainActivity.java:319`) builds **new** `RollingLog` instances, so `copyTo`'s `synchronized` locks different objects
  than the service's `abLog`/`log`. If a rotation lands between the copy of `ab.1.txt` and `ab.txt`, the export misses
  up to one half (the old `ab.txt`, just renamed). Window: a few ms once per ~4 h of rows; the main log has had the same
  exposure since v0.2.1. Not new, worth one sentence in the docs or a shared static lock.
- Kill mid-write: one line is one `write()` of <= 185 B to an append-only stream; a process kill does not truncate a
  completed small write, only power loss can. I checked what the analyzer does with a truncated **last** line: a cut
  inside a numeric token yields a row that parses with a truncated value (reproduced: `g_dth_urad=-1`, OBD side absent);
  a cut that glues the next line fails `int()` and is counted malformed. One row, rare; a trailing-`o_gap=` or
  checksum-free format cannot detect it. PLAUSIBLE, negligible.
- Export order and content: main (`log.1`, `log`) then ab (`ab.1`, `ab`); tested. The `ab` ring is **not cleared** on
  export, so successive exports overlap by up to ~8 h of rows (feeds finding C below).
- Storage/permissions: both rings live in `getFilesDir()` (no permission); `cap()` 1 MiB + 512 KiB = the documented
  1.5 MiB, **plus** the export copy in `cacheDir/export` (now up to 1.5 MiB instead of 1 MiB) and each Downloads copy.
  The "1.5 MiB total" sentence counts the rings only. Cosmetic.
- Privacy: every byte of an `ab` row comes from `format()` (`AbCompare.java:125-141`): numbers and the four `sel`
  words; `summary()` adds four counters to the existing `obd_stats` line. No address, location, name or sample anywhere
  (test `noAddressNoLocation()` fuzzes 200 rows). The analyzer prints only numbers, `sel` and the row's wall-clock
  stamp. Nothing found.
- Growth claim "<= 360 rows/h": holds by construction (an interval cannot close before 10 s of monotonic time) and by
  the 3 h random-schedule test.

## 4. Sign convention (question 4): the expected +1 is justified, with the stated caveats

Re-derived independently:

- Gyro (`Integrator.java:63-69, 87-92`): Android `TYPE_GRAVITY` reports about +9.81 on z for a phone lying face up
  (the apparent-gravity/reaction vector, pointing up), so `down = -g/|g|` is correct. With right-handed phone axes
  (x right, y top, z out of the screen) and gyro rates positive counter-clockwise about each axis, a clockwise turn seen
  from above has omega along -z(up) = +down, so `omega . down > 0`: clockwise (heading increasing) positive. The
  mounting does not enter; only the gravity estimate's tilt does (cos alpha under-read), as the note says.
- OBD (`ObdIntegrator.java:31`): `-(raw12 - 2047) * 658.615 urad/s * 0.020 s`. The same sign and scale as the CMU's
  own 0x078 contract in `src/navigation/pipeline.cpp:28` (`ModelProfile p={2047.0,-0.000658615,...}`) and design note
  section 4 ("count rises in left turns; corr -0.971 with right-positive steering angle, -0.975 with the left-right wheel
  speed difference" on the public ND log, 0x078). The 0x079 sign is **borrowed** from 0x078 via the shared integer
  lattice; the public DBC's `YawRate_Corr` is left-positive (ISO) and coarser, which the note already records.
- Therefore OBD = +1 x gyro (+ zero offsets) is the right expectation under two unverified premises, both written down
  in the note: the Android gravity sign (documented API behaviour) and the 0x079 = 0x078 sign on the owner's car (public
  log of another ND). The analyzer estimates the sign from the data, so a wrong premise is detected, not hidden.
- Minor: `decide()` (`pimu_ab.py:414`) hard-codes the direction (`corr <= -sign_corr`) and uses `EXPECTED_SIGN` only
  in message text. Correct today; if the constant is ever flipped the rule would not follow it. Cosmetic.

## 5. tools/pimu_ab.py (question 5)

### 5A. CONFIRMED: parked detection is gated on the OBD rate itself (circular), so a real OBD zero offset > 0.30 deg/s hides the whole parked section

`parked()` (`pimu_ab.py:284-288`) requires `|o| <= park_dps = 0.30 deg/s` in addition to `|g| <= 0.30` and the
stationary-hint share. The OBD rate is relative to the **nominal** 2047 (not zero-corrected, by design), so its parked
value **is** the unknown zero offset the section is supposed to measure. 0.30 deg/s is 7.96 counts. On the owner's car
the 0x078 stationary median has been measured between 2037.6 and 2053.2 depending on the boot
(`validation/YAW_DEADBAND_2026-10-13.md:19`), i.e. -9.4..+6.2 counts; the public-log 0x079 was within 0.7 counts, but
0x079 on the owner's ND2 is exactly what is unknown. Reproduction (synthetic log from the test fixture generator):
`o_zero=0.25` -> 4 parked intervals; `o_zero=0.5` -> `agree` with **0 parked intervals**, `parked_across.obd = {n: 0}`,
and the `unusable_zero_range` branch of `decide()` (`pimu_ab.py:391`) is skipped because `n < 2`. The report would show
an empty parked list without saying why. Same for a gyro zero of 0.4 deg/s (reproduced, 0 intervals), but the S25 at rest
is 0.01 deg/s so only the OBD side matters in practice. Fix: gate parked on the gyro (`|g|` and `g_st`) only and report
the OBD parked value whatever it is; or gate on `|o - median(o over g_st rows)|`.

### 5B. CONFIRMED mechanism, PLAUSIBLE magnitude: OBD receipt lag biases the scale estimate low by about lag / 10 s x (1 - row autocorrelation); this is the size of the 3 % agree tolerance

The docstring (`pimu_ab.py:35-37`) calls the per-row comparison "lag-free" and budgets 0.1 s. A lag L moves L/iv of
each row's OBD rotation into the next row; for rows whose neighbours have a different rate this is attenuation of the
regression slope, not noise. Reproduction with the fixture's random-turn profile, OBD = (1 - f) x true_i + f x true_{i-1}:

| turn length (rows) | lag 300 ms | lag 500 ms | lag 1000 ms |
|---|---|---|---|
| 1 (rate changes every row) | scale 0.9725, `agree` | scale 0.9542, **`scale_disagree`** | 0.9096, `scale_disagree` |
| 2 | 0.9906, `agree` | 0.9850, `agree` | 0.9717, `agree` |
| 3 | 0.9986, `agree` | 0.9984, `agree` | 0.9987, `poor_correlation` (outlier share) |

and a **true scale of 1.04** (outside tolerance) with a 500 ms lag reported `agree` at 0.992 (1-row turns) and 1.024
(2-row turns). Real driving sits between the rows: a 10 s bin usually holds parts of two manoeuvres. A 0.3-1 s path
delay is plausible for a BT ELM327 clone in monitor mode (the design note itself budgets "tens to hundreds of ms" of BT
delay at section 4/F4, flags receipt gaps > 160 ms as ambiguous, and the stop path returns up to 0.3 s late). The rule
has no lag term, so `agree`/`scale_disagree` is partly a statement about latency. Cheap fix inside the same rows:
regress `o_i` on `g_i` and `g_{i-1}` (two-tap); the sum of the two coefficients is the lag-free scale and the second
coefficient x iv estimates the lag, which the owner also wants to know. At minimum the docstring and note 16.6 should
say that the scale is biased low by the lag fraction.

### 5C. CONFIRMED: no de-duplication, and the ring is cumulative, so repeated exports double-count

`load()` concatenates every file in argument order with no key on `(time, t_ms)`. The A/B ring is never cleared, so
each export contains up to ~8 h of previous rows; an owner who uploads the export after every drive and a reviewer who
passes all of them (the CLI takes `inputs` nargs="+" for exactly this) count each row as many times as it appears.
Reproduction: one log with 20 moving rows -> `not_decidable` (20 < 30); the same file passed twice -> **`agree`** with
40 moving rows and two parked runs of 6 reported as distinct intervals. Duplicates also shrink every robust sd. Fix:
drop exact duplicate `(time, t_ms, row text)` lines in `load()` and report the count; optionally clear or mark the ring
at export (not a wire change). Related: consecutive exports from the same boot are also split into separate parked runs
only by `t_ms` order, which is fine, but a row present in both files is not "consecutive" with itself.

### 5D. Smaller findings in the analyzer

- CONFIRMED, cosmetic: when the OBD side is exactly constant (dead adapter output, or a stuck sentinel that passes
  `decode`) `pearson` returns `None` and `decide()` reports `not_decidable ['moving pair rows 40 < 30']`
  (`pimu_ab.py:412-413`), a self-contradictory reason. Reproduced with 40 moving rows. Should read "no variance on one
  side".
- Statistics checked and found correct: `wls`/`huber` (IRLS with MAD scale, breaks on zero MAD, returns the OLS fit
  when degenerate); outlier limit `max(5 x 1.4826 x MAD, 0.5 deg/s, 10 % |g|)`; geometric-mean scale of the two
  Huber slopes (partial errors-in-variables correction, sign carried from `b`); per-bin median of `(o - a)/g` with
  `g != 0`; `rate_dependence` guarded against `None`/0 slopes; `zero_count = 2047 - rad(zero) x 1e6 / 658.615`
  matches the ObdIntegrator inversion (the test's check is a copy of the same formula, not independent, but the formula
  is right); unit conversion `deg(dth_urad / dur_us)` is rad/s -> deg/s correctly (urad/us = rad/s). Divisions by zero
  are all guarded (`dur > 0`, `g_n > 0` implied by presence, `len(moving)` by `fo`, coverage denominators). Empty and
  malformed logs: exit 2 with a message; glued/truncated lines go to `malformed` except the truncated-last-line case
  above. 30 000 rows (10x a full ring) analyse in 2.0 s; `--json` is 3.3 MiB because `per_row` is included.
- PLAUSIBLE (design, already acknowledged in 16.6): parked detection also needs >= 90 % of gyro windows with the
  stationary hint (`maxRate < 0.005 rad/s = 0.29 deg/s` on the 3-D rate in a 20 ms window). Whether a phone in a car
  mount with the engine idling ever meets that is unmeasured in both directions; if it does not, the parked section is
  empty even with a perfect OBD zero. `PHONE_IMU_STATIONARY_S25_2026-10-13` measured the gyro on a desk, not in a
  running car. The 2026-10-17 CMU journals carry the flags OR per second, so this is answerable offline from existing
  data (share of seconds with bit 1 while wheels are exactly zero) before the next drive.
- Rule table vs code: `test_rule_matches_design_note` compares the 24 numbers with the note's table; I re-read the
  prose table (16.4 rows 1-8) against `decide()` order and found them consistent, including the two `poor_correlation`
  branches and the `projection_min_corr` guard. The gyro side has no gap flag (`Integrator.gaps`/`frame_reset` on the
  gyro are not recorded; only `g_dur_us` shrinks), so a 100-500 ms sensor gap leaves a "usable" row; the rate
  normalisation absorbs it under a constant-rate assumption. Asymmetric with `o_gap`, minor.
- Cases where `agree` could be reported wrongly, summary: (i) lag + true scale off (5B); (ii) duplicated inputs
  reaching decidability (5C); (iii) both sensors wrong alike (stated in the note). Not found: no path reports `agree`
  with a negative slope (the geometric mean carries `b`'s sign and `corr > 0.98` is required), nor with fewer than 30
  distinct moving rows from a single export.

## 6. Test quality (question 6)

AbTests: 886/0 in three consecutive runs (5.8 s, 5.5 s, 5.6 s wall; 1-minute load 3.18-3.21 before each run, below
the 4 limit). `tests/tools/test_pimu_ab.py`: 16 OK (0.25 s). Not run: `make test-phone-imu-interop` (forbidden),
the ARM/CMU side (unchanged), the real APK.

My mutations (each applied to a fresh copy of `core/`, compiled with the shipped tests, `AbTests` run once):

| # | mutation | file:line | caught? |
|---|---|---|---|
| 1 | sign: `oDth -= w.dthetaUrad` | `AbCompare.java:86` | yes, 5 failures (exact-text rows, OBD sum, post-reset, thread conservation) |
| 2a | rate limit: delete the `lastRowNs` guard | `AbCompare.java:96` | **no** (dead code; see section 2; harmless) |
| 2b | rate limit: close after `INTERVAL_NS / 2` | `AbCompare.java:95` | yes, 8 failures |
| 3 | interval reset: `reset()` keeps the open interval | `AbCompare.java:113` | yes, 5 failures |
| 4a | ring cap: rotate at `2 * half` | `RollingLog.java:51` | yes, 1 failure (`ab ring bounded: 1047880`) |
| 4b | ring: do not delete `previous` before rename | `RollingLog.java:52` | **no** (POSIX rename replaces; harmless on Android) |

The 22 hand mutations claimed in the commit were not re-run; mine are independent. Test quality is good for the
Java side (exact strings, conservation under threads, wire identity with switches and resets). On the Python side the
fixture generator produces lag-free, duplicate-free, zero-offset-small data, which is why 5A-5C were not visible to it;
one test each (an OBD zero of 0.5 deg/s must still yield parked intervals; a 500 ms lag must not move the scale beyond
tolerance; the same file twice must not change the outcome) would pin the fixes. No flakiness observed.

## 7. What I did not find

- No way for `AbCompare` to change the bytes, the order, the timing or the count of frames sent to the CMU, including
  across source switches, selector `frame_reset`, accumulator resets, disable/enable and a stalled tick.
- No lock-order inversion, no lock held across I/O on a producer thread, no allocation in `add()`, no unbounded queue.
- No overflow in 10 s or hour-long intervals; no wall-clock dependence in binning.
- No Bluetooth address, location, device name or raw sample in any new format string or analyzer output.
- No analyzer division by zero, crash on empty/malformed/huge input, or regression error in `huber`/`wls`.
- No mismatch between the pre-registered table in 16.4 and `RULE`/`decide()`.

## 8. Ranked list

CONFIRMED (reproduced):
1. 5A parked gate circular on the OBD rate; a real OBD zero offset >= 8 counts hides the parked section and the
   zero-range check (`pimu_ab.py:285`).
2. 5B lag attenuates the scale by ~lag/10 s x (1 - autocorrelation); 300-500 ms flips `agree`/`scale_disagree`
   (`pimu_ab.py:35-37` claim; rule 16.4 rows 6-7).
3. 5C no de-duplication over cumulative, never-cleared ring exports; same file twice turned `not_decidable` into
   `agree` (`pimu_ab.py:132-138`).
4. 5D wrong `not_decidable` reason text when one side has no variance (`pimu_ab.py:412-413`). Cosmetic.
5. Dead `lastRowNs` guard (`AbCompare.java:96`); mutation 2a uncaught. Harmless.

PLAUSIBLE (reasoned, not reproduced on a device):
6. Export/writer race across separate `RollingLog` instances can omit one half of a ring from an export (pre-existing
   pattern; `MainActivity.java:319`, `RollingLog.java:61-80`).
7. One junk one-window row per disconnect from the `active` read race (`SenderService.java:365-377, 292-294`).
8. Stationary hint in an idling car unmeasured; parked section may be empty for the gyro's reason too (16.6 says
   the opposite direction).
9. Truncated last line after power loss parses with a truncated value (one row).
10. "1.5 MiB" counts the rings only, not the cache export copy; pending rows are not flushed at service stop.

Nothing found: wire path, concurrency of the accumulator, sums/overflow, time base, privacy, rule-table consistency,
statistics implementation, test flakiness.

## 9. Evidence of this review (host only)

- `javac --release 11 -Xlint:all -Werror` of `core/*.java` + `tests/java/.../core/*.java`: clean.
- `AbTests`: 886 passed / 0 failed x 3 (5.5-5.8 s each), load 3.18-3.21.
- `python3 tests/tools/test_pimu_ab.py`: 16 OK.
- Edge scripts (scratch `_tmp/edge.py`, `_tmp/lag2.py`): outputs quoted in sections 3, 5A-5D.
- Mutations (scratch `_tmp/mut.sh`): table in section 6.
- No process left running; nothing written outside the scratch copy; `/tmp/jcidlg*` untouched.
