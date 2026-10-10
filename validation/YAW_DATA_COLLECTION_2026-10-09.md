# Yaw-zero data collection rows - logging only, 2026-10-09

Status: implemented and tested offline (host build, ARM cross-compile, synthetic drives, replays of the owner's private
recordings through the product `PersistentLog`). This is a logging-only change. Dead reckoning, anchors, BETA decisions, the
adapter output and the OEM forwarding contract are unchanged: the non-yaw journal rows are byte-identical (details below). Not
run on the vehicle, with a phone, in the DHU or in QEMU. The installed v1.0.0-beta.6 does not contain these rows. They reach a
drive only with a later build, and publishing that build is a separate action that has to be requested explicitly. GPS courses
are a reference, not ground truth. This record contains no coordinates.

Revision (same date, after an independent review): the first yaw window of a standstill and windows after its last zero wheel
event are excluded; `pre` gets a validity check; the yaw digest fields now give way before the digest is lost; a late edge close
is flagged; wheel differences use the model wheel scale; dead merge code was removed; the tests were extended (below).

## Purpose
The yaw zero dominates tunnel heading error (validation/YAW_ZERO_ONLINE_2026-10-09.md). The open questions need continuous
data:
- Is the stationary zero the same as the driving zero?
- Does the zero step at stops or after a sensor reinit?
- Does it drift with time since boot?
- Did tunnel B's +6 count shift happen before the tunnel or inside it?

The persistent profile writes raw motion rows only in event windows, so only 4.6 % of trip4 had raw rows
(TRIP_BETA3_2026-10-08). The next owner drive with a build that contains these rows is the only source of real data. The journal
therefore now carries compact yaw statistics for the whole drive.

The CMU VIM tap provides only three signals: wheel speeds (4 wheels, 0x100), yaw window sums/counts (0x116) and the reverse lamp
(0x118). There is no acceleration, steering, temperature, rpm or A/C signal to log.

## Rows (src/runtime/yaw_study_log.h, owned by PersistentLog in src/runtime/log_profile.h)
Units and times:
- Yaw values in rows are integer 0.01 counts relative to 2048 (`-125` = 2046.75), except fields named as sums.
- Times are CLOCK_MONOTONIC. `boot_s`/`at_s` are seconds since kernel boot, at 0.1 s resolution.
- Events are binned by their producer receipt time.

| Row | When | Fields | Size |
| --- | --- | --- | --- |
| `yaw_stop` | A standstill episode ends and the 5 s merge window (+0.3 s) has passed. A standstill is all four wheels at exactly 0 km/h for >= 1 s; a wheel gap > 0.5 s ends it; episodes < 5 s apart merge. At capture stop an ongoing one is written with `"open":1`. | See "yaw_stop fields" below. | ~155 B |
| `yaw_edge` | 5.3 s after every GPS loss (FIX -> any other decoded class) and return (-> FIX). The next edge or the capture stop cuts it short (`"cut":1`). | `ev`, `from`/`to` class. Before window: `yb`, `nb` samples, `vb` km/h, `cb`. After window: `ya`, `na`, `va`, `ca`. `cb`/`ca` = [first course, last course, span 0.1 s, fixes] of new fixes. `"hist_lost":1` when the after window was closed too late (more than the 6.4 s history after the edge) and lost its front. | ~200 B |
| `yaw_reinit` | A yaw receipt gap > 1 s, or an invalid window (count 0, unrepresentable sum, mean >= 4094: the 4095 marker). Written when the next standstill episode completes; also on a 30 min timeout and at capture stop. | `cause` (1 gap, 2 invalid, 3 both), `n` coalesced triggers, `gap_ms`, `still`, `before` (last standstill mean) and `before_age_s`, `after` (first standstill after it) and `after_delay_s`. | ~180 B |
| `log_digest` (+fields, every 10 s) | The existing 10 s digest. | See "log_digest fields" below. | +105-190 B |

`yaw_stop` fields:
- `boot_s`: start of the episode.
- `dur_ms`: duration.
- `ys`/`yn`: stationary yaw sum (sum - 2048*count) and sample count.
- `sd`: standard deviation of the per-window means.
- `first`/`last`: mean of the first and last second.
- `pre`/`pre_kmh`: yaw mean and wheel speed of the 2 s before the stop, without the bin of the first zero wheel event. Both are
  null, with `pre_bad`, when that window holds a wheel or yaw gap (bit 1), a reinit trigger (2), another zero-speed period (4),
  or no samples (8).
- `bad`: invalid windows plus yaw gaps > 250 ms. Omitted when 0.
- `merged`: number of merged episodes. Omitted when 1.

Which yaw windows count toward `ys`, `yn`, `sd` and `first`:
- Windows received in the first 0.2 s after the first zero wheel event are excluded. They still hold pre-stop samples; the study
  also discarded 0.2 s.
- A window received after a zero wheel event counts only once the next zero wheel event confirms the standstill. The window
  between the last zero event and motion is dropped.
- `first` covers start+0.2 s to start+1.2 s. `last` is the last second up to the last zero wheel event (from the newest 64 run
  windows).

`log_digest` fields:
- `y1`: ten 1 s sums of MOVING yaw samples (sum - 2048*count), from the digest start. `y1n`: their sample counts.
- `yst`/`ystn`: stationary sum and count.
- `gc`: each new GPS fix (FIX class, new utc/position/heading) as tenth-of-second offset*1000 + integer course. Up to 12 per
  digest, then `gc_more`.
- `gq`: worst accuracy of those fixes (m, rounded up). `gv`: slowest fix speed (km/h, rounded down).
- `dw01`/`dw23`: mean left-right wheel difference (km/h, model wheel scale) of slot pairs 0-1 and 2-3 while moving.
- When the fields do not fit the digest buffer, `"yaw_dropped":1` (or nothing) is written instead. The digest itself is kept.

Other rules:
- A reinit trigger ends an ongoing standstill, so `before` and `after` are always separate episodes.
- These rows treat a standstill as all four wheels at exactly zero (the study's definition). The product's stationary rule
  (navigation/gyro_bias.h) accepts up to 0.05 m/s (0.18 km/h) per wheel. A car creeping at 0.1 km/h is therefore stationary for
  the product but not here (tested).

Queueing and bounds:
- Every yaw row goes out as `ROW_RAW`, i.e. journal ring DIAGNOSTIC class, never evidence. The `yaw_` kinds are not in
  `journal_evidence_row`.
- The rows are written directly, never through the RAW window. Under a writer backlog the ring drops the oldest diagnostic rows
  with its existing `journal_dropped` counters.
- Per-kind token buckets: `yaw_stop` burst 3 + 1 per 10 s; `yaw_edge` and `yaw_reinit` burst 4 + 1 per 30 s. Rows over the
  limit are counted in the digest's `suppressed` map (`"yaw_stop": n`).
- Worst-case added rate: digest ~19 + stops 16 + edges 7 + reinits 6 = ~48 B/s (hard cap 60).
- Worker thread only. No allocation, no I/O, bounded work (64 history bins of 100 ms, 16 fixes, 64 run windows).
- `sizeof(YawStudyLog)` = 6528 B and `sizeof(PersistentLog)` = 7552 B, on a 4 MiB worker stack.
- Longest row with extreme inputs: 214 B (buffer 400).
- `PersistentLog::set_yaw_rows(false)` turns everything off. This is a test hook; the product default is on.

## Unchanged product output
- With `--yaw-rows off`, the output equals the output of the HEAD e746394 binary byte for byte, except the process id.
- With the rows on, every non-yaw row equals HEAD's after the new digest fields are removed (standard drive, full and persistent
  profiles).
- replay_beta on trip2: report and CSV are byte-identical before and after.
- In tests/journal/test_log_profile.py, evidence rows and all non-yaw rows are identical with the rows on and off, in four
  scenarios.

## Measured log rate (build/log_rate, persistent profile, synthetic)
| Scenario (1 h unless noted) | Rows off B/s | Rows on B/s | Added |
| --- | --- | --- | --- |
| Standard drive (existing check; yaw count 1) | 1942.0 | 1961.8 | +19.8 |
| Same, vehicle-like yaw (count 5, +-3 count noise) | 1947.6 | 1968.4 | +20.8 |
| Permanent stop-and-go (2 s stop every 8 s; stop rows rate-limited) | 1949.9 | 1985.5 | +35.6 |
| Vehicle-like, 4 h | 1775.2 | 1796.0 | +20.8 |
| Stop-and-go, 4 h | 1776.5 | 1812.3 | +35.8 |

A run's average rate is a steady rate plus a fixed burst:
- The burst is about 830 KB per run: the capture-stop RAW-window flush (about 375 KB of window rows plus their tags, ~400 KB)
  plus the boot period (NO_FIX raw period and first windows, the remaining ~430 KB).
- From the 1 h and 4 h runs, the steady rate is 1717.7 -> 1738.5 B/s (vehicle-like drive) and 1718.8 -> 1754.5 B/s
  (stop-and-go).
- The burst is 828 KB in both drives and 832 KB in stop-and-go, with or without the yaw rows.
- The average is therefore steady + 830 KB / run length. 30 min runs read about 2190 B/s with or without the rows. This is not
  caused by this change.

The budget is 2048 B/s, or 1986.6 B/s with a 3 % margin:
- The existing 1 h check asserts the margin: 4.2 % left.
- A new check judges the margin on the steady state (14-15 % left) and requires the burst unchanged (within 5 KB).
- The 1 h stop-and-go average keeps 3.05 %.

## Replay of the private recordings and agreement with the study (all stops)
build/yaw_replay replays the last boot session of each recording. The comparison is against every >= 1 s stop of the study's
`stops.py` (`stops.json`; times below are seconds since kernel boot).

trip2, 12 study stops, 9 rows:
- Stops 761.6/795.4, 879.1, 928.8, 1042.0, 1121.9, 1187.2, 1249.5 and 1357.6: mean and sd equal the study's to 0.01 count. This
  includes the short ones: 879.1 s (1.8 s) gives 2051.27 / sd 4.35, and 1357.6 s (2.1 s) gives 2047.06 / sd 0.85.
- Before this revision the 879.1 s stop gave 2052.78 / sd 7.25 and the 1357.6 s stop 2046.86. The 0.2 s settling exclusion fixed
  both.
- The parked start (study stops at 45.3, 66.7, 105.8 and 761.6 s; 2048.60, 2048.60, 2048.69 and 2048.90) merges into one
  730.7 s row of 2048.68 (`merged` 4): the stops are < 5 s apart. Against the individual stops this differs by -0.22 to +0.09.

trip4, 8 study stops, 6 rows:
- 50.7, 4209.5 (1.4 s: 2043.13 / sd 3.81; before the revision 2044.04), 4223.0 and 4357.8 s: identical to the study's.
- 147.4 + 181.3 s merge into one row of 2046.87 (study 2046.85 / 2046.88).
- 4236.4 + 4240.3 s merge into one row of 2051.83 (study 2051.34 for the 2.9 s stop, 2051.88 for the 24.7 s stop).
- The differences come only from merging.

`pre` on the replays:
- The trip4 value of 3685 counts (since_prev 0.8 s) is now null (`pre_bad` 7).
- trip4 4209.4 s keeps `pre` = 2418.99 at 3.3 km/h: a clean window with a real ~14 deg/s low-speed turn.

Other replay results:
- trip4's reinit rows are artifacts of its window-only raw recording (gaps between the windows).
- trip2 has no POSITION rows; it adds 107 B per digest and about 11.7 B/s. trip4 adds 130 B per digest.

## What each row answers
- `yaw_stop`:
  - the stationary zero at every stop;
  - its drift within a stop (`first` vs `last`);
  - the step between stops, against time since boot and since the previous stop (trip2/trip4 steps up to 7.5 counts);
  - whether the moving yaw just before a stop (`pre`, when valid) agrees with the stationary value.
- Digest fields:
  - the driving zero along the whole drive, from GPS course rate vs integrated moving yaw (analyzer fit; lags 0 and 1.3 s;
    windows of at most 60 s of digests whose fixes are all >= 12 km/h and <= 5 m);
  - the stationary zero even without a >= 1 s standstill;
  - a wheel-difference turn proxy.
- `yaw_edge`: the yaw level and course rate right before each loss and right after each return.
- `yaw_reinit`: whether the zero steps after a stream gap or a 4095 marker.

## Analyzer (tools/analyze_logs.py)
- Validates the three rows (`malformed_yaw_row`) and the digest fields (`malformed_digest_yaw`). Digests without the fields stay
  valid, and unknown fields are tolerated.
- Reports `yaw_data` with four tables:
  - standstills: mean, sd, first/last/pre, `pre_bad`, step vs the previous standstill, seconds since it, time since boot;
  - the digest-based driving zero fits;
  - the GPS edge comparison, with a rough course-rate zero per side and `history_lost`;
  - the reinit events.
- Adds a text section, `Yaw zero data (diagnostic; ...)`. `yaw_data` is listed in `lower_bounds`.
- Compatibility: an older analyzer (v1.0.0-beta.6 or earlier) reading logs with these rows reports `unknown_record_kind` for
  `yaw_stop`/`yaw_edge`/`yaw_reinit` and an `inconclusive` status. Use the analyzer shipped in the same release ZIP as the
  runtime.

## Tests (host)
tests/runtime/test_yaw_rows.cpp covers:
- episode statistics, with the 0.2 s settling and tail exclusion;
- the merge wait, including the 0.3 s slack;
- the 1 s minimum;
- merging on stationary samples only, also without polls between standstills;
- digest bins, the stationary split, fix courses and wheel differences;
- edges: full, cut, and a late close (`hist_lost`);
- reinit by gap and by a 4095 window;
- `pre` validity (boot, an earlier zero-speed period, a yaw gap, a 4095 window, the start bin with the cadence off the
  bins) and the creeping case;
- the model wheel scale;
- the exact rate-limit count, suppressed counts and the capture-stop flush;
- the digest keeping itself when the fields do not fit;
- the off switch, the diagnostic class and the size bounds.

tests/runtime/test_log_profile.cpp: the exact-count lag-guard test keeps the rows on and filters `yaw_` kinds.

tests/journal/test_log_profile.py covers:
- the 3 % margin;
- rows on vs off in four scenarios (1 h and 4 h, drive and stop-and-go): identical evidence and non-yaw rows, added rate
  10-60 B/s;
- the steady-state margin and the unchanged burst;
- the stop-and-go rate limit;
- the analyzer reading the rows; the fit finds 2048 within 0.5 count.

tests/tools/test_analyze_yaw.py covers:
- the tables and `pre_bad`/`hist_lost`;
- malformed rows;
- edges and reinits;
- the text section;
- a known zero recovered at the matching GPS delay (0 and 1.3 s);
- the window gates;
- an optional private-recording smoke test (MX5DR_YAW_REPLAY, MX5DR_YAW_TRIP_DIR), run on trip2 and trip4.

Mutation checks: a rule is removed or changed, and a test assertion has to fail. 32 of 32 mutations were killed by
assertions; none of them failed to build.

Writer rules:
- stop minimum, merge window, merge slack, merge window without polls;
- pre window length, all four `pre_bad` bits, start-bin exclusion;
- 0.2 s settling, unconfirmed tail windows;
- stationary split, 1 s binning;
- rate limit, stop burst;
- 4095 detection, reinit gap, edge window, late-edge flag;
- fix dedupe, exact-zero wheel rule, model wheel scale;
- row class, digest fields, digest fallback, capture flush.

Product isolation: a collector that perturbs product rows.

Analyzer: fit sign, step sign, row validation, digest gate.

## Not collected, and why
- Acceleration, steering, temperature, rpm, A/C: not in the VIM tap. Correction (2026-10-10): this was wrong for acceleration,
  brake pressure and rpm. The VIP forwards them in SPI 0x116 (+4..+9), 0x169 and 0x15B (validation/VIP_CAN_MAP_2026-10-03.md);
  the motion path only ignores them. They are now logged as raw `chan_digest` rows (validation/VIM_CHANNEL_CAPTURE_2026-10-10.md).
- CMU SoC temperature: out of scope; its firmware path is unverified.
- Per-fix speed and accuracy: only the digest's worst accuracy and slowest speed.
- Raw yaw outside event windows: not within the log budget.
- Coordinates: only courses are logged.
- The wheel slot-to-corner mapping is not known, so `dw01`/`dw23` are named by slot.

## Unverified
- Vehicle, phone, DHU, QEMU, exact-ARM execution (compile only) and real CMU timing.
- Behaviour under a real writer backlog.
- Whether real stop-and-go reaches the stop rate limit.
- The analyzer fit on real full-drive digests (only synthetic digests and replays of two recordings).
- The wheel slot mapping.
- Whether a 1 s receipt gap on the vehicle is a sensor reinit or only a transport gap.
- Whether the 0.2 s settling time fits every yaw window length.
