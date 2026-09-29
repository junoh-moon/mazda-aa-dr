# Stationary calibration and GPS holdout — 2026-09-29

Base: master `411a8e35bb2ff52a4bd113c95b7a9e8bae95ebc3` (PR #13 merge).
Scope: MODEL navigation algorithms, worker integration, journal/auditor contract.
No OEM executable, target CMU, phone or map application was run. No release was
created. These results do not establish physical calibration or GPS accuracy.

## Behavior verified

- Stationary learning requires all four wheels, a bounded continuous dwell and
  stable integer yaw means; moving wheels, variance/outliers, stale clocks,
  missing/invalid values and source changes reject evidence.
- The learned zero is applied only at a successful fresh GPS anchor. Queued yaw
  windows are converted at consumption, not with the old zero at enqueue time.
- Synthetic fixed-zero straight drift exceeds 1 m while the calibrated fixture
  has cross-track displacement below 1e-9 m over its 5-second case. This is an
  arithmetic fixture with a supplied constant bias, not a road measurement.
- Turning, fresh reverse, stopping, new stationary candidates during an outage,
  next-anchor application and fault resets are exercised.
- The GPS holdout uses the same production Pipeline/core in independent state.
  Changing eligible held-out GPS coordinates, speed and heading leaves computed
  DR position/heading/speed identical. Future wheel changes do not rewrite an
  older reference prediction. Reference comparison requires exactly equal
  reference receipt time and prediction frontier.
- Pending yaw coverage, bounded stale abort, invalid/late/frozen-UTC GPS, real
  GPS loss, native DR, source restart and bounded queue overflow are tested.
- Two comparison windows retain zero 2050/version 1 across normal moving
  cooldown; an audit reset restores nominal 2047/version 0. Pending calibration
  candidates are discarded at normal restart, and source/sequence/time/clock
  bindings continue to detect discontinuities.
- MODEL results stay unqualified, the AA adapter and live ASSIST gates are
  unchanged. Existing exactly-once forwarding and veneer tests pass.

## Review findings resolved

An independent Astra review reproduced an inherited anchor bug: a 500 ms old
forward-state event seeded a north-facing body anchor, then a fresh reverse
event allowed a valid prediction one meter south. The reference was a
reverse-moving northward GPS fix, which requires body heading south and travel
north. GPS seeding now requires reverse evidence already received by the anchor,
inside its original lease and age bound. Refusal revokes the prior seed; later
reverse evidence cannot repair the rejected anchor. The reviewer rebuilt the
same reproduction and observed UNSEEDED / E_NO_SEED / model_valid=false.

The review also checked normal cooldown retaining applied calibration, and
identified missing BEGIN/END chronology checks in the PC auditor. Production-
shaped serializer fixtures and malformed-boundary regressions cover the latter.
No live-source timing or qualification was inferred from these fixes.

## Executed checks

Linux x86-64 host, GCC/G++ with the repository warning gates:

- Full `make test`: exit 0. Navigation 604 checks; raw parser/codec/pipeline/
  adapter 815 checks with 7 original sends; gyro 2,644 checks; holdout 1,180 checks.
- Existing core 1,425 checks, deterministic replay, loader 29 scenarios, guard
  22 tests, collector 10 tests, runtime 38 checks, adapter 11 cases and the
  qualified-bridge integration all passed.
- Final focused rerun after auditor chronology fixes: 37 journal tests
  (23 calibration/holdout + 14 motion) and 20 legacy analyzer tests passed. The
  same 37 journal tests also passed with actual ARM/QEMU C++ encoders feeding
  the Python auditor. The earlier full host suite preceded these two added
  chronology cases; the production navigation/runtime code was unchanged.
- **Skipped:** packaging discovered 30 tests, 20 require unavailable private
  stock firmware fixtures and were skipped (10 passed). Real Unix datagram
  integration returned EPERM and skipped; codec/cursor tests did run. This PR
  does not change packaging or socket transport.

Pinned m3-toolchain GCC 4.9.1, upstream commit
`61ec0343de84f6fc7c46840056df1d600d44be8a`, ARMv7 Cortex-A9 NEON softfp:

- All four production artifacts built with the same warning/link gates.
- `tests/run_arm_all.sh`: exit 0, including the new gyro/holdout cases, actual ARM
  veneer fixture, runtime journal and preload smoke. Kernel socket integration
  has the same explicit permission skip.
- New algorithms also passed host AddressSanitizer + UndefinedBehaviorSanitizer
  with leak detection disabled. The estimator/holdout do not allocate memory.
- Production preload still requests only GLIBC_2.4 symbol versions. No new
  dynamic libstdc++ dependency was added.
- ARM `sizeof(Pipeline)=61,184`, `sizeof(GpsHoldout)=90,984` bytes. Together these
  worker-owned objects occupy 152,168 bytes, excluding other worker locals and
  call frames. This is a storage measurement, not a target CPU/stack-load test.

## Reproduction

Host dependencies: C/C++ compiler, Python 3, pkg-config, D-Bus development files.
Use the pinned toolchain from [toolchain instructions](../docs/toolchain.md).

```sh
make test
MX5_TOOLCHAIN=$(pwd)/tools/m3-toolchain
make arm \
  ARM_PREFIX="$MX5_TOOLCHAIN/bin/arm-cortexa9_neon-linux-gnueabi-" \
  ARM_SYSROOT="$MX5_TOOLCHAIN/arm-cortexa9_neon-linux-gnueabi/sysroot"
CROSS_COMPILE="$MX5_TOOLCHAIN/bin/arm-cortexa9_neon-linux-gnueabi-" \
QEMU_SYSROOT="$MX5_TOOLCHAIN/arm-cortexa9_neon-linux-gnueabi/sysroot" \
  sh tests/run_arm_all.sh
```

The physical wheel/yaw/reverse cadence and delay bounds, installed bias drift,
comparison differences, AA/touch coexistence, recovery behavior and phone/app
acceptance remain unverified. GPS comparisons are receipt-time MODEL differences
and may include GPS error, callback delay and sensor averaging hypotheses.
