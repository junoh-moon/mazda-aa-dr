# Core fixtures and normalized CSV replay

All supplied inputs are **synthetic**. They are not CMU logs, verified calibration,
real hardware signal quality, or evidence of Android Auto/Naver Map acceptance.
The replayer has no live I/O and cannot enable an OEM hook.

Build from project root (C99 and libm only):

```sh
cc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Isrc/core src/core/dr_core.c tests/core/test_core.c -lm -o /tmp/mx5-dr-core-test
/tmp/mx5-dr-core-test
cc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Isrc/core src/core/dr_core.c tests/core/replay.c -lm -o /tmp/mx5-dr-replay
/tmp/mx5-dr-replay tests/core/synthetic_straight.csv
python3 tests/core/test_replay.py /tmp/mx5-dr-replay
```

No installation or device command is part of these tests. The C test checks arc
geometry, WGS84 independent equator conversion, forward/reverse turns, sentinels,
quality, NaN/ranges, generation, lease expiry, duplicates, causal mean windows,
stop hysteresis/crawl budgets, actual 60s/1500m boundaries, reduced error limit,
GPS reacquisition, and deterministic normalized anchor replay.

## CSV version 1 (offline only)

Plain comma-separated ASCII fields, no quoting or whitespace. Blank lines and
lines beginning `#` are ignored. Integers are unsigned decimal; times are exact
nanoseconds in one validated monotonic domain. Finite decimals are used for SI
quantities. Malformed syntax returns exit 2 with a line number. Validly parsed
core rejection events remain in output and the replayer exits 0: inspect result
and valid columns. Output is deterministic for an identical input/config/build.

`RESET,source_epoch,session_epoch,generation`

Resets using the development defaults in `mx5_dr_default_config`. Must be first.

`SEED,source_epoch,session_epoch,generation,anchor_id,position_seq,measured_ns,utc_ns,latitude_deg,longitude_deg,body_heading_rad,position_error_m,heading_error_rad,validated,heading_valid,calibration_verified,quality,REPLAY`

The three flags are explicit caller assertions (0 or 1). The literal REPLAY marks
the format as offline input; it is not a proof or an authority to enable live
ASSIST. `quality`: UNKNOWN=0, VALID=1, INVALID=2.

`CONTROL,source_epoch,session_epoch,new_generation,control_seq,kind`

Kinds: GAP=0, GPS_RETURN=1, NATIVE_POSITION=2, DISABLE=3. New generation and control
sequence must increase. Epoch changes use RESET. State values in output:
UNSEEDED=0, READY=1, ACTIVE=2, REACQUIRING=3, NATIVE=4, INVALID=5, LIMIT_REACHED=6.

`MOTION,source_epoch,session_epoch,generation,interval_seq,start_ns,end_ns,received_ns,speed_mps,yaw_rad_s,reverse_active,raw_yaw,yaw_count,yaw_is_mean,yaw_window_start_ns,yaw_window_end_ns,<speed evidence>,<yaw evidence>,<reverse evidence>`

Exactly 43 fields including MOTION. Each of the three evidence groups has 9:

`source_id,source_epoch,producer_seq,measured_ns,received_ns,lease_until_ns,time_uncertainty_ns,quality,freshness`

Freshness: UNPROVEN_POLL=0, PRODUCER_TIME=1, SEQUENCE_WITH_BOUND=2. Unknown/poll
claims are rejected, not automatically converted. Raw yaw guards always run even
when normalized yaw is zero and quality is marked VALID. Point yaw uses zero
window fields. A mean window must be known/completed and contain the supplied
interval. The same producer record can span multiple covered subintervals.

`SNAPSHOT,source_epoch,session_epoch,generation,now_ns`

Invokes final core output checks and emits its immutable prediction with a valid
flag. Other event rows report the resulting worker state, not final send-time
permission. Runtime OEM/provider/readiness gates are outside this replayer.

The checked-in fixture yields 20 meters north over 1 second, then E_STALE after
an excessive send delay, then E_NO_SEED after GPS return followed by another GAP.
The fixture's source VALID flags are intentionally synthetic test assumptions.
