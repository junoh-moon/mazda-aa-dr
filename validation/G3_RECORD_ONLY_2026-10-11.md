# G3 first-connection heading, RECORD-ONLY build - 2026-10-11 (v3b)

Status: implemented and tested offline only (host build and the touched host and packaging tests, one exact-ARM object
compile of runtime.cpp and pipeline.cpp with the pinned GCC 4.9.1, synthetic drives, byte-identical BETA replay of three
private recordings with the tracker off, on and with a slow store). Not run on the vehicle, with a phone, in the DHU, under
qemu-arm or in a firmware VM. No release, vehicle visit or drive is requested or authorized by this record. It implements
recommendation 1 of validation/G3_FIRST_CONNECTION_HEADING_2026-10-11.md (log-only) and nothing of recommendation 2 (injection).

v2 (same day) answered an independent read-only review of v1; v3 (same day) answers two further independent reviews of v2
(Opus and Codex, converging). Both are summarized under "Review findings and fixes". v3b answers a third review of v3 (conditionally
shippable, default on acceptable): fsync only for long stops, sharper cancel/stop tests, the pre-registered rule. Store persistence stays DEFAULT ON in
BETA (owner switch: menu 8); the reason is given under "Residual risk".

## What changes and what does not

The BETA product (config mode 5) persists its best body heading when the car has been parked for 15 s, restores it at the
next boot and journals the heading it WOULD send at the first moments of a session and its error against the first
policy-M-accepted GPS course. **Nothing that is sent changes**: no adapter choice, no payload byte, no OEM contract, errno or
timing in a tap.

| Part | File | Thread / path |
| --- | --- | --- |
| Store codec, atomic file write, chain token, off switch, idle rule, one-slot mailbox and store thread loop, `G3Tracker` | src/runtime/g3_heading.h | worker (MODEL tick); file write on the dedicated store thread |
| Boot-lifetime motion observation `G3Motion` and read-only views of the BETA estimate | src/navigation/pipeline.h/.cpp | worker (pipeline drain) |
| Restore at worker start, `shadow_boot` field, idle rule inputs, mailbox hand-off, detached store thread | src/runtime/runtime.cpp | worker startup and tick; store thread |
| `--g3 off|on|slow`, `--g3-store`, `--g3-out` | tools/replay_beta.cpp | replay only |
| G3 section | tools/analyze_logs.py | PC |
| Menu 8 (owner switch), `g3_marker` in menu 2, reinstall clears the marker | packaging/trial, trial_status.sh, install.sh, USB_START_KO.md | parked shell |

The pipeline addition writes only its own `g3_` member from drained SPEED/YAW events (BETA-enabled pipelines only); no core,
gate, anchor, lease or publication reads it, and no reset or fence clears it.

## Review findings and fixes

### v1 -> v2 (H1, M1-M4, LOW)

v1 wrote the store on the shared journal writer thread with two fsyncs. A slow fsync could push the journal lag past 1.5 s,
so `journal_lag_guard` would withdraw BETA provenance in a tunnel (trip6a already shows three 1500-1529 ms "lagging" events
in ENGAGED episodes after a GPS-loss RAW-window drain, without any product fsync). v2 moved the write to a dedicated thread,
allowed only a parked write under an idle rule, added the heading-change trigger (M1), menu 8 (M2), the off/on/slow replay
identity (M3), the start-up drift note and `unc_d` (M4), and `post_failed()` (LOW).

### v2 -> v3 (two reviews)

1. Bounded store I/O. The store thread re-checks a gate before opening the temporary file, before the file fsync and before
   the rename. The worker bumps `cancel_gen` on every tick on which a write is in flight and any cancel condition holds:
   BETA in GPS_LOST/ENGAGED/NO_FIX/SPEED_ENGAGED, `journal_current` 0, a RAW-window drain or queued bulk rows, writer lag
   > 300 ms, a wheel above exactly zero, a stale wheel stream; capture stop and worker exit set `stop`. A cancelled or
   stopped write removes its temporary file and never renames. A request picked up more than `G3_STORE_STALE_NS` = 500 ms
   after it was posted is dropped without I/O. Each result row carries `lat_ms`, `start_ns`, `end_ns` and, when cancelled,
   `reason` (cancelled, stale, stopped) and `at` (open, fsync, rename) (v3b: also `why`, the worker's cancel condition).
   The directory fsync is removed: exactly one file fsync remains (v3b: only for stops of >= 60 s) (record-only data; a rename lost to a power cut only costs the next boot's carried heading; the repo's relfs
   notes say nothing about rename durability).
2. Exact park detection. Parked = all four wheels EXACTLY zero (the beta.8 stop-latch definition), a gap-free run of such
   wheel events (consecutive events at most `G3_WHEEL_GAP_NS` = 250 ms apart; a gap or any moving wheel ends the run, a
   single zero sample after a gap starts a new one) lasting >= 15 s, and the newest drained wheel event no older than 250 ms
   + the pipeline reorder delay. A crawl never parks.
3. Shutdown/capture contract. Capture stop (menu 3) and every worker exit set `stop`; the thread checks it immediately
   before the rename (and before the other steps), is never joined, and accepts no new request. A write that already passed
   the rename gate finishes, and its result row may be lost with the worker. Test: `g3_stop` stops the capture 1 s into a
   3 s fsync step; the worker stopped in 272-304 ms without waiting (store runs 0 at that moment), the write then ended
   `stopped` at the rename gate, no file and no temporary file remained.
4. Evidence without I/O (Opus MEDIUM-2): `g3_since` rows. `unstored`: a park of >= 15 s that needed a record and never got
   one (reason: the last blocking condition). `left`: the start of motion after a stored park. Fields: `m` distance, `rot`
   net body rotation and `arot` |rotation| (0.1 deg) integrated while moving since the last record, `age_s` of that record.
5. ABA: every hand-off carries the worker epoch (process counter, one per worker start); a result is consumed only if seq
   and epoch match. Stale header comments fixed.
6. ARM and packaging: `g3-test` and `worker-beta-test g3_park/g3_cancel/g3_move/g3_stop` are registered in
   tests/run_arm_all.sh as timing class (tests/run_arm_timing.sh; docs/RELEASING_KO.md lists them). The stock-BusyBox
   harness (tests/packaging/cmu_emulation.py) checks menu 8 on/off and `g3_marker=present`.
7. Tests: an injected 3 s sleep inside the fsync step (`g3_fsync_hook`) with GPS lost during it (`g3_cancel`) and with the
   car driving off during it (`g3_move`); mutations below.
8. Fallback policy: no unbounded HIGH coupling remains (see "Residual risk"), so persistence stays default on.

### v3 -> v3b (third review)

The third review found a wrong premise: "parked" (all wheels exactly zero for >= 15 s) includes red lights and traffic
jams, so a drive can produce many store writes, and the store fsync is the only fsync the runtime does while driving (the
journal writer fsyncs only at capture stop). v3b:

1. fsync only for long stops (`G3_FSYNC_MIN_STOP_S` = 60 s). A stop of 15-60 s is written as temp write + rename WITHOUT
   fsync: still atomic (the rename replaces the record or not at all), durability left to the kernel/relfs writeback. When
   the same stop passes 60 s the record is written once more WITH the single file fsync. If the 15 s write was blocked by
   the idle rule until 60 s, one write with fsync is made. Both writes obey every cancel rule and all three gates (the fsync
   step is simply skipped without sync). `g3_store` rows carry `sync` 0/1.
2. Tests: the writer runs no fsync step (hook not called) with sync 0 and runs it with sync 1; the tracker requests sync 0
   at 15 s, sync 1 at 60 s (also after a cancelled fsync write) and a single sync write after a block until 60 s;
   `g3_park` shows two writes (15 s without hook, 60 s with the 3 s hook). New mutation: fsync for every stop -> killed
   (test_g3 and g3_park).
3. `g3_cancel` now asserts the cancel reason it injects (`"why":"beta_replacing"`; `g3_move`: `wheels_moving`); `g3_stop`
   asserts a join-free exit explicitly: at the moment the worker was joined the store thread was still inside the blocked
   fsync step (hook active, that write not yet counted), then it ended `stopped` at the rename gate and only the earlier
   15 s record remained.
4. Pre-registered rule and analyzer check (below).

v3b test evidence (host, then pinned GCC 4.9.1 link + qemu-arm, load 0.8-1.5, every test at its first attempt):
`test_g3` 807 checks; `test_worker_beta` all 15 scenarios (`g3_park`: 2 writes, 1 with fsync, hook calls 1, 3003 ms;
`g3_cancel`: cancelled at rename, why beta_replacing, only the 15 s record stored; `g3_move`: why wheels_moving; `g3_stop`:
store write still in its fsync step when the worker was joined, then stopped at rename; summary gaps <= 1025 ms, no lag
rows); `test_log_profile`, `test-navigation`, `test-replay-beta` (incl. private chain), `test-tools` (incl. the overlap
check) passed. Mutations (11, incl. "fsync for every stop" in test_g3 and g3_park) all killed. The optional deferred
unlink of a failed temporary file (review item 5) was not done.

### How many writes per drive (measured from real logs)

Stops after the first motion of each boot (private recordings; trip2/trip3 from every wheel event with exact zero and
gaps <= 250 ms; trip6 from the persistent `yaw_stop` rows, >= 1 s with all wheels zero, stops less than 5 s apart merged,
so its durations can be longer than the exact rule):

| Boot | Source | Stops >= 15 s (writes without fsync) | Stops >= 60 s (writes with fsync) | Longest |
| --- | --- | --- | --- | --- |
| trip2 07987d33 (shadow.4) | wheel events | 6 | 0 | 49 s |
| trip3 843d1bbf (shadow.5) | wheel events | 2 | 1 | 148 s |
| trip6a 82d37f8a (beta.7) | yaw_stop | 9 | 3 | 120 s |
| trip6b 322800ef (beta.7) | yaw_stop | 15 | 4 | 190 s |

Upper bound per drive: one write per such stop that followed >= 2 m of motion or a >= 1 deg heading change (red lights and
jams included), plus one fsync write per stop of >= 60 s: in these boots 2-15 writes without fsync and 0-4 with fsync. A
stop that never became idle produces no write (`g3_since unstored`).

### Which record survives a power cut

The record on disk is the last stop of >= 15 s that was stored. The final stop before a power cut is not stored if it is
shorter than 15 s (or never idle); the carried record is then the last longer stop, and what happened after it (distance,
rotation) is only in the `g3_since` rows and the next boot's `checked`/`course` rows. The analysis must read `g3_since`
before interpreting a carried heading. A record written without fsync can be lost or be the previous record after a power
cut shortly after the write; that costs only the carried heading.

### Pre-registered rule (decided before the first drive)

After the first drive with this build, if any `g3_store` row has `lat_ms` > 500, or its window [`start_ns`, `end_ns` + 2 s]
contains a `beta_journal_lag` "lagging" event or a BETA transition to GPS_LOST/ENGAGED, the next build defaults the store
OFF (menu 8 opt-in) while keeping the in-memory tracker and its rows. `tools/analyze_logs.py` prints
`G3 write overlap check: n writes, max lat_ms X, overlaps with beta_journal_lag/GPS_LOST: k` and appends the rule outcome
(unit-tested).

## Store file

`/data_persist/mx5-aa-dr/logs/g3-heading.v1` (the directory the runtime already writes and menu 3 exports; temporary
`.g3-heading.v1.tmp`). One text line, at most 256 bytes:

```
mx5g3 <version=1> <seq> <boot_id> <S|M> <flags> <heading_cdeg> <unc_cdeg> <lat_e7> <lon_e7> <pos_err_dm> <utc_s> <rt_s> <mono_ms> <chain> <crc32>
```

flags: 1 heading valid, 2 position valid. v2/v3 write only `S`; `M` is still decoded (and rejected as `moving_at_end`).
heading: the BETA core body heading (north 0, clockwise); unc: core heading budget + 0.10 x rotation since the last anchor.
utc_s: GPS wall time from the newest FIX (0 if none), rt_s: CLOCK_REALTIME, mono_ms: the writing boot's monotonic clock,
chain: the guard decline token at the writing boot. CRC32 (IEEE) over every byte before its separating space. The decoder is
strict and reports the version before the checksum.

## Rules and constants (g3_heading.h)

| Rule | Constant | Reason |
| --- | --- | --- |
| Parked write only: all wheels exactly zero, gap-free (<= 250 ms) for >= 15 s, fresh stream, once per stop, after >= 2 m since the last record or a >= 1 deg heading change; no fsync | `G3_PARKED_NS`, `G3_WHEEL_GAP_NS`, `G3_MOVING_MARK_M`, `G3_HEADING_CHANGE_DEG` | reviews H1/M1, Codex park rule; parks include red lights and jams |
| The same stop at >= 60 s: one more write with the single file fsync | `G3_FSYNC_MIN_STOP_S` | third review: the only runtime fsync while driving is limited to long stops |
| Idle rule before a hand-off (strict: also no queued journal row) | `G3_WRITER_IDLE_LAG_NS` | no store I/O while BETA provenance depends on the journal or the journal is busy |
| Cancel rule while a write is in flight (gate before open, fsync, rename): replacing BETA state, journal lagging, RAW drain or bulk rows, writer lag > 300 ms, a moving wheel, stale wheels; stop flag at capture stop / worker exit; drop at pickup after 500 ms | `G3_STORE_STALE_NS` | review v2 item 1/3 |
| At most one write per 5 s; 3 consecutive failures end writes for the boot | `G3_WRITE_MIN_NS`, `G3_WRITE_FAILURES_MAX` | bounded eMMC traffic |
| Restore: valid magic/version/CRC, state S, heading flag, uncertainty <= 30 deg | `G3_UNC_MAX_DEG` | design section 5; the spiral home garage (~50 deg) is refused |
| Boot chain: guard decline token unchanged (else `chain_broken`); unreadable = unknown; same kernel boot = `same_boot` | - | the guard rewrites `guard/last-decision` at every decline only |
| Age <= 7 days (CLOCK_REALTIME at start, GPS utc at the first fix) | `G3_AGE_MAX_S` | bounds unknown events |
| CARRIED needs chain ok and a known age; otherwise CARRIED_UNVERIFIED | - | the request |
| First fix farther than 50 m + stored position budget + wheel distance since boot = REJECTED(`moved`) | `G3_MOVED_MIN_M` | 11-13 m standstill wander observed |
| Propagation: stored body + yaw integrated while moving; rule-5 growth; > 30 deg = REJECTED(`unc_grew`) | BETA rule 5 | standstill yaw never integrated |
| Travel = body + 180 while reverse is latched; unknown journaled as forward | - | core convention |
| First M course within 30 deg of the propagated travel heading = CONFIRMED, else REJECTED(`course_error`) | `G3_CONFIRM_MAX_DEG` | error recorded either way |
| Owner switch `g3-off`, menu 8, read once at worker start | `G3_OFF_NAME` | review M2 |

## Uncertainty (review M4)

The propagated uncertainty uses the BETA rule-5 terms (0.002 rad/s, 0.11 deg/s, plus 0.10 x rotation). The yaw zero drifts
much faster right after engine start: trip6a showed -2.5 deg/s mean over 10 s with all wheels at zero. The model uncertainty
is therefore below the known start-up drift. `err_p` (propagated travel heading minus the first M course) is the
measurement; `unc` and `punc` must not be used for an injection decision. `g3_first` rows also journal `unc_d` = propagated
uncertainty + 2.5 deg/s x the first 60 s of motion (`G3_STARTUP_DRIFT_DEG_S`, `G3_STARTUP_DRIFT_WINDOW_S`), as a pessimistic
reference that changes no state.

## Journal rows (diagnostic class, kept by the persistent profile)

- `g3_store`: `loaded` / `rejected` / `missing` / `off`, `checked` (first fix, `unc_grew`), `written` / `failed` / `cancelled` (seq,
  heading, uncertainty, wheel distance, `wait_s` since the stop became parked, `lat_ms`, `start_ns`, `end_ns`; failed: stage
  and errno; cancelled: `reason` and `at`), `unavailable` (no store thread).
- `g3_since`: `unstored` (a park of >= 15 s that needed a record and never got one, with the reason) and `left` (start of
  motion after a stored park): `m`, `rot`, `arot`, `age_s` since the last record.
- `g3_first`: `start`, `t30`, `course` with state, times, distances, `rev`, `carried`, `body`, `unc_d`, `travel`, stock
  bearing and counts, and the signed errors `err_c`, `err_p`, `err_s` (or `d_sp` before a course).
- `shadow_boot` gains `"g3":{"state":..,"reason":..}` in BETA only.

Rows never call `j.fail()`; an oversized row is dropped and counted. They are not evidence rows.

## Tests (2026-10-11, v3, host unless noted)

| Check | Result |
| --- | --- |
| `test_g3`: codec, corruptions, file I/O failures, chain token, off switch, restore rules, first-fix checks, course, reverse start, `unc_grew`, session rows, idle rule, parked-write policy (never while moving or creeping, 15 s, stale stream, idle-gated, once per stop, M1, `g3_since unstored/left`, cancelled/stale/stopped results, failures and retry, refused post, no store thread), every gate (open/fsync/rename x cancelled/stopped: temporary removed, old record kept), store thread with a 3 s fsync step (post < 50 ms, polling < 5 ms, cancel and stop during the step prevent the rename, stale drop, epoch mismatch ignored), two-boot roundtrip, pipeline `G3Motion` incl. exact-zero runs (crawl 0.02 m/s never a stop, a 20 s wheel gap then one zero sample restarts the run, stale stream) | 792 checks passed |
| `test_worker_beta`, all 15 scenarios: the 11 existing ones (send assertions unchanged; `g3_cancel` exempts only its own parked replacements from the moving-speed check) + `g3_park` (written once, 3 s, after BETA left SPEED_ENGAGED), `g3_cancel` (GPS lost during the 3 s fsync step: cancelled at the rename, nothing stored), `g3_move` (car drives off during it: cancelled at the rename), `g3_stop` (capture stop during it: worker stopped in < 0.4 s, stopped at the rename) | passed; no `beta_journal_lag`, `beta_summary` at most 1036 ms apart in the G3 scenarios |
| ARM: `g3-test` and `worker-beta-test g3_park/g3_cancel/g3_move/g3_stop` compiled AND LINKED with the pinned GCC 4.9.1 (`-Werror`) and RUN under qemu-arm, one at a time, host load 3.4-6.4 | all passed at the first attempt (no retry); g3_park written 3001 ms, g3_cancel cancelled at rename 3001 ms, summary gaps <= 1036 ms, no lag rows |
| `test_log_profile` (G3 retention incl. `g3_since`) | passed |
| tests/packaging touched (`test_trial_menu`, `test_trial_status`; full suite below) | passed |
| `test-tools` (incl. tests/tools/test_analyze_g3.py: `g3_since`, `cancelled`), `test-navigation`, `test-replay-beta` (incl. tests/replay/test_replay_g3.py: off/on/slow byte-identical and the private chain) | passed |
| Mutations, each killed: (v1/v2 set) CRC check, standstill yaw, reverse travel, boot chain, moved check, age limit, analyzer `err_p`, parked 15 s -> 1.5 s, tracker ignores idle rule, `journal_current` check, runtime passes no idle rule, direct write from the worker, write on the journal writer thread; (v3) no gate before fsync, no gate before rename, no cancel on ENGAGED/GPS_LOST, rename after the stop flag, loose park detection (0.05 m/s), no epoch in the result, stale request not dropped, no cancel when a wheel moves | all killed |

Private chain (informational, replay has no wall clock): trip2 `missing`, 5 parked writes, first course stock error 0;
trip3 restored 69.0 +- 23.8 deg CARRIED_UNVERIFIED, then `unc_grew` in the spiral garage, no GPS course; trip4 `no_heading`.

Not run: full `make test`, the rest of the ARM suite, the stock-BusyBox harness itself (`cmu_emulation.py` needs an unpacked
release bundle with these scripts; the menu 8 check was added, not executed), `test-runtime` as a whole, `test-lds`,
`test-adapter`, `test-collector`, `test-recovery`.

## What the first drive with this build should show (menu 3 export, `analyze_logs.py`)

1. First boot: `g3_store missing`; `written` rows only at parks of >= 15 s with all wheels exactly zero; check `lat_ms`
   (the real one-fsync cost) and `wait_s`.
2. `cancelled` rows: `reason`/`at` tell which condition stopped a write at which step (a car driving off or a GPS loss at a
   parked spot); `stale` means the store thread picked the request up late.
3. The analyzer's `G3 write overlap check` line: overlaps 0 and max `lat_ms` <= 500, otherwise the pre-registered rule makes
   the next build default the store OFF. `sync` 1 rows only at stops of >= 60 s.
4. `g3_since unstored` rows name parks that were never stored and what happened to the car since the last record
   (`m`, `rot`, `arot`, `age_s`); `left` rows the start after a stored park.
5. Next boot: `loaded` (CARRIED / CARRIED_UNVERIFIED), `checked` at the first fix, `g3_first course` `err_p` (the metric),
   `err_s`/`stock_zero` (stock baseline), `rev[0]=1` (reverse start).
6. `logs/g3-heading.v1` itself is in the export.

## Residual risk (plainly)

- v3b: the fsync runs only for stops of >= 60 s (0-4 per drive in the logs above); shorter stops write without fsync.
- An fsync that has already started cannot be interrupted. It can overlap a GPS loss or a start of motion that happens
  while it runs; the following steps (close, rename) are then cancelled, but the started fsync itself may take as long as
  the device needs and may, on relfs, commit other unflushed data. This is bounded by the preconditions: only after 15 s of
  exact standstill with fresh wheels, only when BETA is not replacing, the journal is current, no RAW drain or queued row
  exists and the writer is idle; at most one write per stop and per 5 s; one ~110-byte file. It does not run on the journal
  writer thread, so it can delay journal rows only through the device itself. This is judged bounded, so persistence stays
  default on; menu 8 turns it (and the whole tracker) off.
- A write that passed the rename gate before a capture stop completes; its result row may be lost.

## Limits

- Record-only: whether Naver uses a car bearing at standstill or session start is unmeasured (DHU experiment 10).
- No MOVING record: a reset or power cut while driving after a parked write leaves that record valid until the next boot's
  first fix (`moved`) or first course (`course_error`) rejects it; before the first fix such a heading is reported as CARRIED.
- A park shorter than 15 s (the final stop before a quick power cut) is not stored; a park spent in a BETA replacing state
  (tunnel, underground garage with GPS lost, the NO_FIX overlay) is not stored (`g3_since unstored`).
- Continuity: only guard declines, the 7-day age and the first-fix position check; reinstalls, other builds, towing or a
  turntable before the first fix are not detected. A root-only guard directory leaves restores at CARRIED_UNVERIFIED until
  the first fix. The CMU wall clock at boot is unverified.
- The store thread no longer shares the journal writer, but its one fsync still hits the same relfs device (see "Residual
  risk"); measured by `lat_ms`/`start_ns`/`end_ns`.
- Exact park detection needs the wheel stream to stay gap-free (<= 250 ms between events); a vehicle cadence with longer
  gaps restarts the 15 s timer and can prevent parked writes (visible as no `written` rows).
- The model uncertainty is below the start-up yaw drift (M4); GPS course is a reference, not ground truth.
- The stock-BusyBox harness was not executed here (it needs a release bundle); the menu 8 text was checked by the host menu
  tests.
