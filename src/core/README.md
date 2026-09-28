# Portable DR core (C99)

This directory is an implemented, host-tested numerical/state core. It is not a
vehicle sensor adapter or permission to enable live ASSIST. `dr_core.h` is the
API contract. No clocks, D-Bus, device files, CAN writes, hooks, allocation,
threads, or external libraries (apart from C libm) are used.

One worker owns a `mx5_dr_core`. The runtime must synchronize snapshot publication
and independently invalidate its ready latch on control transitions, queue loss,
source changes, profile changes, and teardown. A copied snapshot never acquires
new measurement time: its UTC and frontier remain those of the prediction.

## Calling sequence

1. `mx5_dr_init` with a valid source/session/generation context and profile.
2. `mx5_dr_seed` with a *previously validated* GNSS anchor, body heading, calibration
   claim and source time. Consecutive-fix validation and timing mapping are the
   adapter's responsibility; the core checks flags, quality, ordering and ranges.
3. While READY, `mx5_dr_step` may advance a shadow prediction using fully covered,
   time-aligned intervals. No output is authorized.
4. A real mode0 transition calls `mx5_dr_control(GAP, newer_generation, control_seq)`.
   It preserves only a READY seed. Step through subsequent intervals; query
   `mx5_dr_get_snapshot(now, expected_context)` before use.
5. The first GPS return calls `GPS_RETURN` and discards seed authority immediately.
   Another GAP before a fresh validated seed cannot resurrect the old prediction.
6. Owner/session/sensor reconnect calls reset using the new runtime epoch; no
   anchor or input history is retained.

Control sequence and accepted seed position sequence share an increasing position
sequence namespace. Interval sequence is independent. Old queued contexts return
E_CONTEXT without modifying a newer core. A changed source identity inside the
current context is a reconnect error and invalidates it. Same-sequence producer
records can be held only when *all* evidence and value fields remain identical.
Only an exact duplicate of the most recent interval is ignored. Other overlaps,
reorders, changed duplicates and gaps invalidate the estimate.

## Normalization boundary and explicit assumptions

- Separate speed/yaw/reverse producer evidence and source leases are required.
  Poll timestamps/UNKNOWN quality never become live-valid in this core.
- Speed is nonnegative m/s magnitude. Yaw is calibrated body rad/s, north-zero,
  clockwise-positive. Reverse is explicitly 0 or 1. The core does not infer units,
  signs, quality-code meanings, source freshness, or current profile from raw data.
- Normalized yaw still carries averaged raw yaw and a nonzero count. Values
  4094/4095 and count zero are rejected even if an upstream adapter claims VALID.
  This redundant guard is not proof that other raw values are usable.
- The adapter supplies non-overlapping, completed [start,end] intervals, already
  split at speed/yaw/reverse events. Alignment and source measurement uncertainty
  must be accounted for by the adapter/profile. It must not fabricate fresh
  records for a held value. Reverse heartbeats need their actual source evidence.
- A mean yaw applies only inside its declared window. Its complete window must
  already have arrived. It may be split into subintervals sharing identical
  evidence; no forward hold outside the mean window is allowed. The adapter must
  ensure the constant-mean approximation is justified and error-budgeted.
- New late anchor acceptance explicitly resets integration. The caller replays
  retained normalized intervals from that anchor exactly once. This core does
  not keep a raw history buffer or reconstruct pre-anchor stop dwell. Upstream
  must supply a moving, qualified seed or explicitly reconstruct the required
  history before publishing readiness. This is the current API boundary, not
  a claim that asynchronous replay ingestion is already implemented.
- No adaptive calibration, map matching or exact stochastic accuracy model is
  implemented. Speed/yaw error rates must conservatively include residual scale,
  bias, noise, timing and hold effects for the allowed interval envelope. Input
  uncertainty is checked against a ceiling; passing that ceiling alone does not
  calibrate these error rates. Budgets are operational metrics, not measured 95%
  coverage or guaranteed physical bounds.

## Current limits and output

Hard ceilings: 60 seconds from anchor, 1500 meters accumulated absolute measured
travel, 100 meters internal position budget. Profiles may lower these, not raise
them. The task requested a 100m implementation ceiling; the earlier design's 50m
was a provisional candidate and can be selected by `config.error_max_m=50`.
Other defaults (0.3m/s speed error, 0.002rad/s yaw error, 0.02rad/s stop consistency,
100m/s and 2rad/s physical guards) are explicit test/development assumptions.
They are not Mazda calibration values and cannot enable ASSIST without a verified
profile. `mx5_dr_default_config()` itself confers no source validity.

Arc integration uses stable sinc and at most 50ms substeps. WGS84 uses midpoint
meridian/prime-vertical curvature, with |latitude| <85 degrees. This is a local
short-step approximation, not a global geodesic solver. Diagnostic accumulated
E/N is the sum of local displacements, not a fixed ENU datum.

Stop entry uses continuous low-speed/yaw-consistent dwell; interruption resets it.
A stopped estimate fixes position/body heading, clears bearing and reports zero
speed. Ignored measured crawl/rotation increases error budgets; distance and time
limits continue. Large yaw while stopped invalidates; stop-exit speed releases
stop before normal yaw integration. No bias is learned.

Snapshot checks current generation, causal arrival time, age, all source leases,
and time/error limits at supplied `now`. Source leases are additionally capped by
measurement age. Output-age position error is added only to the returned copy.
There is no forward coordinate extrapolation. Callers must honor return status
and `valid`; diagnostic numeric fields can remain populated when invalid.
