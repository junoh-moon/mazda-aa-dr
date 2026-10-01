# Worker navigation pipeline

`Pipeline` is a bounded, single-owner C++11 worker component. It performs raw
normalization, chronological ordering, interval alignment, GPS anchoring and
core snapshot selection. It does not open a sensor, perform I/O, create threads,
allocate memory, or publish an Android Auto snapshot. `channel.*` is the separate
runtime transport boundary.

## Diagnostic raw path

`init_model()` followed by `enqueue_raw()`, `enqueue_position()`, `drain()` and
`diagnostic(actual_now_ns)` runs the existing C core in its explicit MODEL
domain. Model evidence has `MX5_DR_MODEL` quality and `MX5_DR_MODEL_TIME`
freshness. Model anchors leave all physical-validation flags false. Numeric
output can have `model_valid=1`, but always has `valid=0`; the bridge rejects
the model domain even if a caller supplies qualification flags.

The research profile uses four-wheel average `(raw*0.01-100)/3.6` m/s and
`-(integer_sum/count-2047)*0.000658615` radians/second. Reverse event 0/1 maps
to forward/reverse. These reproduce the researched native consumer's arithmetic,
not a calibrated installed vehicle. Negative wheel magnitudes, sentinel/out-of-
range wheels, count zero, invalid averaged yaw and unknown reverse values reject
the prediction. Reverse requires an actual source event; it is never initialized
from a file, assumed forward, or refreshed by wheel/yaw activity.

The remaining hypotheses are exposed in `Status.uncertainties`:

* A supplied original transport timestamp is modeled as host monotonic time.
  Otherwise the observed callback receipt time is the model clock. Neither is
  asserted to be the physical measurement time.
* The interval between successive yaw event times is modeled as that record's
  mean window. A mean is never integrated outside that window.
* Wheel and reverse events are held within the original sample-age lease,
  normally 250 ms. Successful unrelated traffic cannot extend that lease.
* GPS callback receipt time, travel-to-body heading conversion, affine physical
  scale, offset, and error budget parameters are model assumptions. Stationary zero learning is an explicit MODEL-only opt-in; it is not a
  physical calibration or measured accuracy guarantee.

The runtime enqueues original copied records and calls `drain(now-100ms)` with
the default profile. The fixed 128-event sorted queue splits integration at
every speed, reverse, yaw and position boundary. The 100 ms holdback is an
explicit development envelope; late records revoke the prediction instead of
being silently retimed. Snapshot checks use actual current time and all original
receipt times, so a delayed prediction can correctly be stale despite having
computed numeric coordinates.

Two plausible moving GPS observations are needed for a model anchor. The first
GPS return immediately revokes an active diagnostic and starts a new two-fix
sequence. Native mode 3 also revokes it. Pending GPS/native observations suppress
snapshot validity before their ordering watermark; missing earlier sensor data
cannot prevent that revocation. A missing source, queue overflow, source change,
clock rollback or late record resets history and requires a new anchor.
The default MODEL path with GPS/wheel learning disabled also revokes an older
READY anchor when a GPS pair fails its time/displacement checks. The rejected
endpoint cannot be the next pair's baseline; two new plausible fixes are needed
before another GPS gap can activate a prediction.

## Qualified external adapter path

`init_qualified()` accepts only separately supplied normalized sensor evidence
through `enqueue_speed/yaw/reverse` and an externally verified `enqueue_anchor`.
Raw ingestion cannot enter this path. Evidence is retained exactly and the core
checks its quality, producer identity, sequence, time and lease. Position
observations supply mode controls, not physical qualification. A successful
`qualified_snapshot()` still requires the existing bridge qualification and
adapter send-time provenance/deployment gates. There is no configuration switch
that upgrades raw model assumptions into verified evidence.

The qualified worker must bind a revoker before publication. Faults, resets,
reinitialization and destruction retire an adapter candidate. A point-in-time
`qualified_snapshot()` expires at its requested `now`; future publication uses
`qualified_publication()` and the core-derived bounded lease.

A queued qualified anchor suppresses output immediately. Replacing an ACTIVE or
NATIVE solution requires the caller to supply a newer context generation and
reserve `position_seq-1` for GPS_RETURN; both sequence numbers must exceed the
previous position sequence. The pipeline applies that control and then seeds
the supplied anchor without rewriting its context, evidence or validation
claims. A stale replacement context revokes the old solution and rejects the
anchor. Initial or already-reacquiring anchors can use the current context.

The captured POSITION generation is authoritative for qualified mode controls.
If a verified anchor has already performed GPS_RETURN and established a READY
seed in that generation, its matching GPS mode 1/2 observation consumes the
same return without invalidating the new seed. This also covers an anchor
measured before the observation's receipt time; neither timestamp is rewritten.
An ACTIVE estimate, a different generation, and untagged or MODEL input do not
take this path. A later GAP still needs a newer observed generation. Sensor
windows must arrive before their chronological watermark as usual.
An adapter GPS quality change from mode 1 to 2 or back invalidates the adapter
generation even if the core remains READY. Without a new verified anchor the
old core generation cannot publish; the next observed GAP can advance it to
ACTIVE. Receipt time alone never retags the old READY seed.

`tests/navigation/test_navigation.cpp` exercises the actual core and bridge with
synthetic input: straight/turn/reverse/stop motion, timestamp and queue failures,
receipt-only worker cadence, chronological cross-stream jitter, bounded reverse
expiry, GPS/native handoff, model isolation and an explicitly qualified fixture.
These are host synthetic checks, not OEM execution or vehicle validation.

`tests/navigation/test_live_pipeline.cpp` additionally passes synthetic original
SPI payloads through the production VIM parser and fixed motion wire codec into
the receipt-time model pipeline. It checks straight/reverse/stopped LOCATION
previews and unchanged, exactly-once fake OEM sends. A separate explicitly
qualified fixture exercises the pipeline, bridge and actual adapter replacement
path, including expiry. Despite the test filename, it does not connect to a live
sensor or execute a socket transport; synthetic data is not promoted into live
qualification.

## Stationary zero and GPS holdout

`Pipeline::init_model(..., true)` enables the bounded stationary gyro estimator.
It uses all four wheels, stable yaw and explicit time/gap gates, and applies a
pending zero only at a successful new GPS seed. Raw yaw windows are converted
at integration time so queued windows do not retain the previous zero. Resets
clear the anchor and applied calibration together; qualified ingestion is
unchanged. Default MODEL callers retain the fixed-profile baseline.

`GpsHoldout` owns another MODEL pipeline. After a moving GPS warmup it withholds
GPS updates from DR for a bounded 10-second window, compares only exactly
receipt-aligned frontiers, and requires a new warmup after completion or abort.
References can invalidate a comparison but never correct its prediction.
The primary SHADOW and OEM sends are independent. See
[`docs/SHADOW_CALIBRATION_KO.md`](../../docs/SHADOW_CALIBRATION_KO.md) for thresholds,
logs, limitations and reset semantics.
