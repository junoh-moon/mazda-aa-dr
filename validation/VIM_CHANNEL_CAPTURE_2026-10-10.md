# VIM side-channel capture (acceleration, brake, speed, rpm) - logging only, 2026-10-10

Status: implemented and tested offline (host build and tests, exact-ARM cross-compile with the pinned toolchain and the
touched ARM tests under qemu-arm, synthetic drives). Logging only: the three motion event kinds (wheels 0x100, yaw 0x116,
reverse 0x118), their receive_seq, epoch and datagram format, dead reckoning, anchors, BETA decisions, the adapter output and the
OEM forwarding contract are unchanged (evidence below). Not run on the vehicle, with a phone, in the DHU or in a firmware VM.
The installed v1.0.0-beta.6 does not contain it; it reaches a drive only with a later build, and publishing that build is a
separate action that has to be requested explicitly. This record contains no coordinates and no OEM bytes.

Revision v2 (same date, after an independent safety review, no blocker): tap `lost` baseline (`lost0`), an owner switch
(marker file), the side-channel state in `shadow_boot`, a wrap hint, one channel-name definition, the timestamp and final-batch
limits documented, the stop-and-go margin exception and a first-drive checklist stated, and the pre-existing ARM bug of
tests/runtime/test_yaw_rows.cpp fixed and added to tests/run_arm_all.sh.

## Purpose
validation/VIP_CAN_MAP_2026-10-03.md shows that the VIP forwards more vehicle-motion signals to the CMU than the product reads;
validation/YAW_DATA_COLLECTION_2026-10-09.md wrongly said that no acceleration or rpm signal exists (corrected there). The
owner's public-recording analysis (yaw-decode-2026-10-10) found a stepwise, state-dependent zero offset in the 0x078 yaw
signal, and the owner's stationary yaw zero also steps between stops. Independent channels recorded during real drives are the
data needed to understand that and possibly exploit it: a straightness check (lateral acceleration), stop/start events and
load (longitudinal acceleration, brake pressure, rpm). This change records them in the persistent log without changing any
product decision.

## The channels (offsets relative to the VIMC callback data)
Static analysis of the VIP image (`VIP_APP-MAZ150_10.13.012`, same SHA-256 as the VIP records), SPI packet builders that
store the literal ids 0x116, 0x169 and 0x15B. Byte 0 is the VIP's sub-type byte (0x07, 0x02, 0x2F). M16C stores words little
endian.

| SPI id | Length | Offset | Field (source CAN frame, from the VIP_CAN_MAP bit layout) |
| --- | --- | --- | --- |
| 0x116 | 10 | +1..+2, +3 | yaw sum (LE u16) and count: the existing motion path |
| | | +4 | Qf a: CAN 0x078 byte 0 bits 7-6 (0..3) |
| | | +5..+6 | longitudinal acceleration: 13 bits = (0x078 b0 & 0x3F) << 7 \| b1 >> 1 (LE u16) |
| | | +7..+8 | brake pressure: 13 bits = (0x078 b3 & 0x1F) << 8 \| b4 (LE u16) |
| | | +9 | Qf b: CAN 0x078 byte 5 bits 7-6 (0..3) |
| 0x169 | 4 | +1 | Qf: CAN 0x079 byte 0 bits 7-6 |
| | | +2..+3 | lateral acceleration: 13 bits = (0x079 b0 & 0x3F) << 7 \| b1 >> 1 (LE u16) |
| 0x15B | 12 | +1..+2 | vehicle speed: CAN 0x202 bytes 2-3 (LE u16) |
| | | +3..+4 | engine rpm: 13 bits = 0x202 b0 << 5 \| b1 >> 3 (LE u16) |
| | | +5..+6, +7 | further 0x202 fields (b4/b5, b6 bits): copied, not logged |
| | | +11 | 0x202 byte 1 bits 1-0 (the VIP uses the speed only when this is 3) |

The acceleration and brake values in 0x116 are the latest CAN 0x078 snapshot at the time the yaw window is sent, not a window
average. 0x15B is sent when its values change (VIP_CAN_MAP).

## Do the callback messages include 0x169 and 0x15B? (code evidence, not executed)
- The tap wraps the single VBS registration's `app_event` and sees every message that this callback receives; it filters ids
  only after the call (src/sensors/vim_tap.cpp:47 before this change). No new registration is added.
- vim_app `iuc_Rx_Process_Request` (0xd5e0) dispatches by `id & 0x3C0`. 0x116 (0x100 group), 0x169 and 0x15B (both 0x140
  group) take the same branch (0xdc2c), which ends in `iuc_queueMessage` (0xdd5c -> 0xf0b0) for every id of the group; the
  queue code reads the id only for logging.
- The VBS callback `VBS_BUS_CAN_VIM_Data_Received_Cb` (libjcimod_can.so 0xa8b0) accepts any id with length 1..16 and has a
  generic path (0xad90) for ids other than 0x160, 0x144, 0x200-0x23F and 0x5A.
- Conclusion: by static analysis both ids reach the wrapped callback exactly like 0x116. Not verified by execution or on the
  vehicle; the first drive answers it (`chan_digest` `n`, analyzer `chan_data.summary.reached`).

## Design and what it changes on the OEM thread
The motion decisions are unchanged; the OEM thread is not untouched. Per motion message (0x100/0x116/0x118) the tap now also
takes and releases one try-only flag; per 0x169/0x15B message it takes the flag and copies at most 16 bytes; per batch it makes
one nonblocking `sendto` (8 records or 500 ms: at most about 20 per second at the expected rates). errno is restored before the
OEM callback. That the OEM callback is still called exactly once, with the same pointer and its own errno, was verified in the
test harness only (tests/sensors/test_vim_tap.cpp), not on OEM code.
Tap (inside the stock jciVBS process, src/sensors/vim_tap.cpp):
- The motion block is unchanged: same id check (0x100/0x116/0x118), same sequence increment, decode, try-lock and send. The
  receipt time it already reads is kept in a local (`(received=now_ns())` in the same argument position).
- After that block, `chan_observe` copies (does not parse) 0x116/0x169/0x15B payloads into a static batch: no allocation, no
  extra clock read (0x169/0x15B carry the receipt time of the latest motion message, at most ~100 ms older), no waiting.
  0x169/0x15B never reach `decode_vim_message` and consume no receive_seq.
- A try-only atomic flag (`__sync_lock_test_and_set`, never waited on) guards the batch; a concurrent callback skips the copy and
  counts it.
- One nonblocking datagram per 8 records or 500 ms on a separate socket (`<motion channel>.ch`, format `MDC1`), so its send
  buffer and the receiver's queue are separate from the motion channel's: a full or absent side receiver cannot delay or drop a
  motion datagram. Lost records (busy, no time yet, failed send) are counted and reported in the next batch.
- errno is restored before the OEM callback as before; the OEM callback is still called exactly once with the same pointer.
- Opening the side socket can only fail closed (`chan_enabled=false`); the motion path is then exactly as before.
- Timestamps: a 0x169/0x15B record carries the receipt time of the latest motion message. The ~100 ms bound holds only while
  motion messages keep arriving. No clock is read for them, so when motion stops a pending batch is flushed only when it is full
  (8 records) and its records keep the last motion time; the 500 ms age flush needs a motion message. In practice 0x116 (yaw) is
  itself a motion message, so a silent motion stream means the VIP stopped sending.
- A partial batch (fewer than 8 records, younger than 500 ms) pending when the CMU or jciVBS stops is lost and not counted in
  `lost` (at most 7 records / 500 ms per stop).
- One channel-name definition: `navigation::MOTION_CHANNEL_NAME` (tap, worker, default arguments); the side channel is that
  name + `.ch`.

### Owner switch (off without rebuilding)
While the file `/data_persist/mx5-aa-dr/vimchan-off` exists (any type or content), the tap opens no side-channel socket and
sends nothing, and the worker binds no side receiver (`shadow_boot` `"vim_side_channel":"disabled"`). Both read it once at
start, so it takes effect at the next CMU start; deleting it turns the side channel back on. Absent, or not checkable for
another reason (any lstat error): on.

The owner toggles it with USB `trial` menu item 7 (revision v3; the owner's keyboard has no Shift, so no typed paths): the menu
shows the current state, asks for one more `7`, writes the marker atomically (temporary file then rename) or removes it, syncs,
and says that a parked CMU restart (menu 5) is needed. Because tap and worker read the marker once at start, the toggle has no
effect before that restart; a mismatch in between is harmless (sends nobody receives only count as tap `lost`, and the
analyzer's `tap_silent` can appear transiently for such a boot). Menu 2 reports `vim_side_channel_marker=present/absent` and the
`vim_side_channel` value of this boot's `shadow_boot` row. Menu 1 (reinstall) clears the marker (fresh default on), like
`disable-next-start`; menu 6 removes it with the package directory; menu 4 keeps it. Existing menu numbers 0-6 and their
behaviour are unchanged; the menu text stays English like the other items (40-column CMU console; the Korean explanation is in
packaging/USB_START_KO.md). Menu, status and reinstall cases run in tests/packaging (test_trial_menu.py, test_trial_status.py)
and, on a bundle built from this source, under the stock CMU BusyBox 1.19.2 (proot + qemu-arm, scratch harness on the private
stock tree; 14 of 14 cases passed). It is deliberately not a `mx5dr.conf` key: the guard
binds that file's hash in its manifest (`binding_changed:mx5dr.conf`), so editing the config declines the whole product until a
reinstall, and the strict config parsers reject unknown keys. Tested: with the marker present `chan_start` opens nothing
(`chan_sender` stays null), no datagram is sent and the motion stream equals the run without the side channel.

### Side-channel state in the journal
`shadow_boot` gains `"vim_side_channel"`: `open` (worker receiver bound), `disabled` (marker), `unavailable:<errno>` (bind
failed), `not_persistent` (full profile: no receiver) or `no_capture`. The tap's own socket state cannot be journaled (the tap
has no output besides its sockets), so it is inferred: `open` with motion events but no `chan_digest` row means the tap's side
socket failed or the tap declined (0x116 always arrives with motion); the analyzer reports this per boot as
`chan_data.summary.tap_silent_sessions` (`tap_silent`), only for a boot with `open`, motion events > 0 in its log digests and at
least 40 s of digests.
Rows present with `n[1]`/`n[2]` = 0 mean the ids do not arrive. This adds one field to the existing `shadow_boot` row; the
full-profile worker test asserts it.

Why a separate side channel instead of a new motion kind: a new kind in the motion datagram would consume receive_seq values or
need a second sequence, and the worker's `decode_motion` rejects unknown kinds as `RECEIVE_DECODE` faults, which reset the
MODEL input. A separate lossy socket is invisible to `drain_motion`, the cursor and the Pipeline.

Worker (src/runtime/runtime.cpp): only with the persistent profile, it binds the side socket and drains at most 32 datagrams per
turn after `drain_motion`; batches go to `PersistentLog::channels()` only. ChanDigestLog (src/runtime/chan_digest_log.h) also
reads the accepted wheel events for its stationary flag. Nothing returns to the Pipeline, holdout, BETA or adapter.

## The chan_digest row (every 20 s, diagnostic class ROW_RAW, never evidence)
`{"kind":"chan_digest","schema":1,"mono_ns":..,"n":[n116,n169,n15b],"ax":[min,max,mean,stationary mean,stationary n],
"bp":[min,max,nonzero],"ay":[min,max,mean,stationary mean,stationary n],"q":[mask a,mask b,mask lat],"v":[last speed,changes,
status],"rpm":[last,changes]}` plus `"bad":[short,odd,invalid,rejected]`, `"wrap":bits` and `"lost":n` when nonzero, and
`"lost0":n` once.
- Raw integers only. A channel without samples is null, a mean without samples is null. `q` masks: bit v set when Qf v was
  seen (8 = only 3). `v`/`rpm` keep the last value across periods (0x15B arrives only on change).
- Stationary: the newest accepted wheel event at or before the sample time has all four wheels at exactly 0 km/h, is at most
  500 ms older, and its zero run began at least 200 ms before the sample (the yaw rows' definition and settling time; the first
  samples after a stop still hold the deceleration).
- `odd`: fields present but impossible for the VIP builder (Qf > 3, value wider than 13 bits, 0x15B status > 3): counted and
  excluded. `short`: fewer bytes than the fields need. `invalid`: no data, more than 16 bytes, or an unexpected id.
- `lost`/`lost0`: the tap counts dropped records since its own start, including sends before the worker bound the socket or
  across a worker restart. The first cumulative value (and a smaller value after a tap restart) is a baseline written once as
  `lost0` in the next row; `lost` carries only later increases. A second baseline inside the same 20 s period (tap restarted
  twice) overwrites the first; only the last `lost0` is written.
- `wrap` (hint): bit 0 ax, bit 1 ay when min < 200 and max > 7900 (a signed 13-bit value may have wrapped 8191 <-> 0; min, max
  and mean are then not meaningful). Brake pressure is not flagged (it spans from 0 upward). The analyzer applies the same rule
  to rows without the flag and skips wrapped rows in the straight-stretch test.
- Rows start with the first side-channel batch; without that input the profile is byte-identical to before.
- Longest synthetic row 283 B (buffer 400); `sizeof(ChanDigestLog)` 1056 B and `sizeof(PersistentLog)` 8616 B on the x86-64
  host (7552 B before); on the ARM target (GCC 4.9.1, qemu-arm run) `sizeof(PersistentLog)` is 8600 B.

Candidate scales (NOT applied anywhere in the product): accelerations 13 bits with zero ~8000 and ~1952 counts/g (public DBC
comment only, VIP_CAN_MAP); speed 0x202 raw/360 m/s = 0.01 km/h; signs unknown; brake-pressure zero and unit unknown (the
`nonzero` count assumes 0 when released, unverified).

## Measured log rates (build/log_rate, persistent profile, synthetic side channel, --chan on/off)
| Scenario | off B/s | on B/s | added |
| --- | --- | --- | --- |
| Standard 1 h (existing PersistentProfile check) | 1961.8 | 1971.5 | +9.7 |
| Vehicle-like yaw 1 h | 1968.4 | 1978.1 | +9.7 |
| Stop-and-go 1 h | 1985.5 | 1995.7 | +10.2 |
| Vehicle-like 4 h | 1796.0 | 1805.6 | +9.6 |
| Stop-and-go 4 h | 1812.3 | 1822.5 | +10.2 |

Steady state (from the 1 h and 4 h runs): 1738.5 -> 1748.2 B/s (vehicle-like), 1754.5 -> 1764.7 B/s (stop-and-go); fixed burst
827.8 KB -> 827.6 KB (unchanged). Budget 2048 B/s, 1986.6 B/s with the 3 % margin: the standard and vehicle-like 1 h averages
(1971.5, 1978.1) and every steady state keep the margin (3.4-14.6 %). The 1 h stop-and-go average (1995.7) is below 2048 but
inside the 3 % margin (2.6 % left). Exception to the 3 % rule, stated explicitly: for stop-and-go the margin is judged on the
steady state (1764.7 B/s, 13.8 % left); the excess of the 1 h average is the fixed ~832 KB burst of a run (boot raw period and
capture-stop window flush), which this change does not alter (831.7 -> 831.6 KB). The yaw patch already asserted only < 2048 for
this average (its margin was 3.05 % with 1.1 B/s of room, so any addition crosses it). The 20 s cadence was chosen to stay near +10 B/s; a 10 s cadence would add ~20 B/s and break
the vehicle-like 1 h margin.

## First-drive verification checklist (reviewer)
a. log_digest per-kind motion event counts and the wheel/yaw rate (about 10 Hz each) equal those of a v1.0.0-beta.6 drive.
b. receive_seq gaps (`seq_gaps`) and late/gap diagnostics (`motion_late_accepted`, `max_receipt_gap_ms`) not increased.
c. `chan_digest` `n[0]` (0x116) equals the yaw event count of the same period (within one batch at the period edges).
d. From the second row on, `lost` absent (0) and no `bad`; `lost0` only in the first row.
e. `n[1]` and `n[2]` nonzero or zero: whether 0x169/0x15B arrive at this callback.
f. boot row and pid unchanged during the drive, no CMU reset and no guard fallback marker.
g. Measured journal rate below 2,048 B/s.

## What each channel could answer
- Lateral acceleration: straight stretches (lateral mean near its stationary value, small spread, > 30 km/h) give a driving
  yaw zero without GPS; whether the yaw zero steps relate to lateral bias/tilt at stops.
- Longitudinal acceleration and brake pressure: stop and start moments, braking state at stops (does the yaw zero step
  correlate with the brake held or released?), pitch/grade at a stop.
- rpm: idle load (A/C, alternator) at stops, engine running vs stopped (idle stop is not fitted to the 6MT but rpm still shows
  load changes).
- Speed (0x202 raw): an independent speed against the wheel average; the change count shows how often 0x15B arrives.
- Qf bits: whether the sensor ever reports degraded quality.

## Analyzer (tools/analyze_logs.py)
- `chan_digest` is validated (`malformed_chan_row`), no longer counted as a log digest kind, and reported as `chan_data`:
  summary (messages per id, `reached` per id, bad counters, tap lost, Qf values, stationary ax/ay means), per-minute
  summaries, a standstill table (each `yaw_stop` mean against the stationary ax/ay means of the overlapping 20 s periods;
  `shared` when a period touches another standstill) and straight stretches (lateral mean within 20 raw counts of the session's
  stationary lateral reference, spread <= 120, every log_digest of the period above 30 km/h; yaw excess = moving yaw mean of
  those digests minus the latest standstill mean). Thresholds are raw counts chosen without vehicle data (20 counts ~ 0.01 g on
  the candidate scale) and are to be revisited with the first drive. `chan_data` is listed in `lower_bounds`.
- An older analyzer reports `chan_digest` as an unknown `_digest` kind (tolerated, no new finding).

## Tests
Host (`make test` in a scratch copy of d6a3b16): see the results section below.
- tests/sensors/test_vim_tap.cpp: a mixed message sequence (all three motion kinds, valid/short/oversized/null side-channel
  messages, an unrelated id, a malformed wheel message) gives the identical motion event stream (kind, raw values, count,
  reverse, epoch, receive_seq, source time) with the side channel on and off; 0x169/0x15B/0x144 consume no receive_seq; the
  copy precedes the OEM mutation; OEM callback exactly once with its errno; busy flag, failed send, age flush and "no time yet"
  loss accounting.
- tests/navigation/test_chan_channel.cpp: MDC1 round trip for 1..8 records, every truncation/extension, header/record/padding
  mutations, encoder input checks, name derivation; real sockets: event, empty, fault and continue, credentials, oversized
  send, no receiver, full queue (EAGAIN, no blocking).
- tests/runtime/test_chan_rows.cpp: parsers (offsets, short/oversized/null, 13-bit and Qf limits); exact chan_digest rows
  (stationary rule incl. 50 ms boundary, 200 ms settling, 600 ms gap, odd/short/invalid ids, change counts, Qf masks, tap lost
  delta incl. a restarted tap, persistence of the last speed/rpm, nulls); in PersistentLog: no input and rows off are
  byte-identical, rows are ROW_RAW, 20 s cadence, the open period before the final log_digest at capture stop.
- tests/journal/test_log_profile.py ChanRows: 1 h vehicle-like, 1 h stop-and-go and 4 h runs with --chan on/off: evidence rows
  and every non-chan row identical, BETA state identical, rate bounds above, burst unchanged, analyzer reads the rows (18
  standstills, straight stretches with |yaw excess| < 0.5 count on the synthetic zero), malformed rows rejected.

## Results (scratch copy of d6a3b16, 2026-10-10, re-run for revision v2)
- `make -k -j2 test`: every target passed except the three collector targets (`test_collector`,
  `mx5dr-collector-host`, `test_collector_journal`), which do not compile on this host because the dbus-1 development headers
  are missing (environment; the collector is untouched). One skip: tests/tools RecordingSmoke (needs MX5DR_YAW_REPLAY); the
  private trip replay of test-replay-beta is skipped there and run separately below.
- Private recording trip2 through `replay_beta --sweep 0:100000:2 --check`: report and CSV byte-identical between the d6a3b16
  binary and this build.
- build/log_rate journals: d6a3b16 binary vs this build with `--chan off` (persistent 1 h) and with the full profile (10 min):
  byte-identical except the boot row's process id (the harness writes its own shadow_boot row; the worker's shadow_boot gains
  `vim_side_channel`, asserted in tests/runtime/test_worker_beta.cpp).
- Log rates re-measured: identical to the table above (on: 1971.5 / 1978.1 / 1995.7 / 1805.6 / 1822.5 B/s; `lost0` adds 10 B
  once per run).
- Mutation checks (each rule removed, its test must fail): settling rule, wheel-gap rule, odd exclusion, speed change count,
  lost delta, lost baseline, lost0 written once, wrap hint, owner switch in the tap, marker lstat semantics, no rows before
  input, 13-bit limit, 0x116 short length, 0x169 into the motion path, tap copy length guard, tap loss on failed send, side copy
  before the motion send, decoder padding check, decoder reserved byte, analyzer min <= max, analyzer wrap rule, analyzer
  tap_silent: 22 of 22 killed by a failing test.
- Exact ARM (pinned GCC 4.9.1 toolchain): `make arm` builds all six artifacts from scratch; libmx5dr-vimtap.so still exports
  only `VIMC_AddClient`, no TEXTREL, GLIBC_2.4 only. Under qemu-arm (commands of tests/run_arm_all.sh), 40 runs passed: vim
  parser, vim tap (incl. side channel and owner switch), motion channel, side channel, log profile, yaw rows, chan rows,
  journal (default, writer, five storage scenarios), worker session (15 scenarios), worker BETA (11 cases). No timing test
  failed in this run.
- tests/runtime/test_yaw_rows.cpp (pre-existing at d6a3b16): its `field()` helper returned a 32-bit `long` on ARM, so the
  `mono_ns` check at line 96 overflowed (and -Werror type-limits stopped the ARM build). Fixed (long long via strtoll,
  23200000000LL) and added to tests/run_arm_all.sh; it now builds with -Werror and passes under qemu-arm.
- The full tests/run_arm_all.sh (release preload, AA install probe) was not run.

## Unverified
- Vehicle, phone, DHU and firmware-VM execution; real CMU timing and callback concurrency.
- Whether 0x169 and 0x15B really arrive at this callback on the vehicle (static analysis only), their real rates, and whether
  this ND2 sends CAN 0x078/0x079 at all.
- Scales, signs, zeros of the accelerations and brake pressure; which Qf value means good; the 0x15B status meaning.
- Behaviour of the extra socket in jciVBS (one more file descriptor; nonblocking sends) under a real stalled worker.
- The straight-stretch and standstill-table thresholds on real data.
- The analyzer on real chan_digest rows (synthetic only).
