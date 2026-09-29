# Live-input SHADOW draft verification — 2026-09-29

Base: merged PR #11, `11b76079d2475d7f709ddbb2ab98eac459d96da9`.
This record applies to the live-shadow-navigation feature diff, not to the
published `v0.2.0-observe.2` release. No proprietary binary was executed and no
physical CMU, vehicle, phone, Naver Map, or Claude session was tested.

## Implemented path

Existing VBS VIMC callback registration -> bounded pre-mutation sensor copy ->
nonblocking local datagram -> worker's sorted timeline -> shared DR core ->
MODEL snapshot -> production LOCATION serializer preview. OEM AA sends remain
original and exactly once. Qualified synthetic inputs additionally exercise the
same navigation pipeline -> bridge -> actual AA adapter replacement path.

A separate VIMC subscription was investigated and rejected: its OEM fanout uses
blocking `mq_send` to per-client queues, creating an orphan-client backpressure
risk. The chosen tap wraps only the existing VBS client and adds no subscription.
See `docs/SENSOR_SOURCE_CONTRACT_KO.md` for the static ABI and identity evidence.

## Checks run

`make test` completed with the supplied private stock fixture and a freshly built
four-artifact bundle supplied via `MX5DR_RELEASE_BUNDLE`:

| Check | Result |
| --- | --- |
| Shared core | 1,425 existing checks; replay fixtures passed |
| New navigation pipeline | 604 checks passed |
| New raw-input-to-adapter integration | 815 checks, 7 exactly-once OEM sends passed |
| Existing core/bridge/adapter integration | 8 exactly-once sends passed |
| Sensor parser and VIMC registration tap | Passed; raw pre-mutation copy, original pointers/returns/errno, repeated/null registrations, bounded route capacity and loss |
| Local motion codec/cursor | Passed; strict record shape, clock representation, sequence gaps, source restart, no high-water rewind |
| Kernel Unix datagram integration | **Skipped**: environment denies `socket()` with EPERM |
| Recovery/editor | 22 tests passed |
| Packaging | 30 tests passed, **no skips**, including fresh ARM payloads, touch coexistence, SHADOW AA+VBS templates, tap integrity and uninstall |
| Collector | 10 tests plus independent journal checks passed |
| Loader, runtime and adapter regression | Passed; runtime config/SHA 38 checks, 11 adapter cases, journal failures and loader interposition |
| Existing PC log analyzer | 20 tests passed |

Host command (substitute the local private fixture and build bundle paths):

```sh
MX5DR_STOCK_ROOT=/path/to/stock_reference \
MX5DR_RELEASE_BUNDLE=/path/to/new-four-artifact-bundle make test
```

`make arm` completed with the pinned m3-toolchain commit
`61ec0343de84f6fc7c46840056df1d600d44be8a` / GCC 4.9.1, ARMv7 Cortex-A9 NEON
softfp. `tests/run_arm_all.sh` then passed in QEMU, including the new 604-check
navigation, 815-check end-to-end path and registration-tap ABI fixture, as well
as the existing veneer, loader, journal and core tests. The kernel socket test
was also explicitly skipped in QEMU because the host denies that syscall.
QEMU executed generated synthetic programs only, not OEM code.

The new production tap has no TEXTREL or dynamic libstdc++ dependency. Its only
application-defined public hook is `VIMC_AddClient` (ELF runtime markers are also
visible); required versioned libc symbols are GLIBC_2.4. These checks establish
build compatibility, not actual OEM loader scope or service startup success.

Supplemental ASan/UBSan runs passed for navigation and end-to-end C++ paths.
Leak checking was disabled because LeakSanitizer could not enumerate `/proc`
in this environment; the reused core object in those supplemental runs was not
instrumented. No memory-leak verification is claimed.

## Cases and review fixes

- Receipt-only sensor messages, explicit absence of physical producer time,
  completed yaw windows, count division, unit/sign conversion, chronological
  ordering and bounded cross-stream jitter.
- Straight motion, turns, direction changes, reverse bearing and stopped
  hasBearing encoding; unknown reverse and stale reverse never silently become
  forward. Wheel/yaw traffic does not renew reverse validity.
- Missing sensor windows, malformed input, queue overflow, sequence gaps and
  duplicates, negative/backward clock values, source epoch changes and stale
  snapshots revoke the estimate and require a new GPS anchor.
- GPS/native return immediately suppresses output even if queued behind missing
  sensor coverage. Qualified anchor replacement revokes ACTIVE output and
  requires explicit newer context; rejected replacement cannot leave old output
  available.
- MODEL values cannot pass the ordinary snapshot/bridge, even with forged
  qualification flags or a manually set valid bit. Production ASSIST remains
  disabled independently in configuration and exact-request provenance.
- Independent Astra reviews found and resolved (1) source cursor rewind after a
  replay, (2) stale ACTIVE output surviving a new qualified anchor, and (3)
  loader/registry lock inversion during VIMC registration. No remaining concrete
  blocker was reported in the bounded reviewed code. This is not vehicle LGTM.

## Build identities (test artifacts, not a release)

| Artifact | SHA-256 |
| --- | --- |
| libmx5dr.so | `93a18ca1091cd763bb92472d58974b078e5e1f927714bcea443c8033bd4e8a31` |
| libmx5dr-vimtap.so | `f0fdb213525e7b5dab240cf5d0cc1dc491098a32b0a4ce067b73b0a229148e3c` |
| mx5dr-guard | `ba77b78a1aced130456a563e8b0249dedcc974c62e1a08f14c864f97353dbf6f` |
| mx5dr-collector | `a3b5c6b95478a36cd18d562534688fc375c12b038388f7a169d665586116b42e` |

The collector is unchanged. Test binaries and the install bundle are not
committed to the public source repository.

## Unverified target conditions

Actual VBS preload binding and registration, datagram permission/credentials,
sensor availability without SD, physical sensor cadence (including 6MT reverse),
latency bound, gyro bias/calibration, positional error, AA/touch coexistence and
next guarded boot recovery remain target checks. Recovery covers the next
external-guard invocation, not retries inside the same running SM.

Live ASSIST additionally needs verified source quality and timing, exact LDS
request provenance, AA timestamp semantics and phone/app acceptance. Receipt-time
MODEL computation supplies none of these guarantees. All configuration and log
collection are performed while parked; calculation and logging are automatic.
