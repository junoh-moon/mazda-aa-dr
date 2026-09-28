> **2026-09-28: installation on hold.** Historical 0.1 analysis/instructions follow. [Current status](STATUS_KO.md) and [review corrections](REVIEW_2026-09-28_KO.md) supersede installation GO statements. No vehicle or phone validation has been performed.

# Native DR follow-up: concrete eligibility and acquisition gates

Status: offline static analysis, 2026-09-27. No CMU intervention, firmware changes, or execution of extracted ARM code. Binary: supplied NA74.00.324A NNG, SHA-256 `8d6a128464aae0b325df08ac5a0c6234714f2696649e65dab7e11eb7d47a03ed`. All addresses are ELF virtual addresses in the named binary.

## Implementation decision

**Go:** retain native NNG reuse as an implemented, conditional provider. Build automatic observation and offline replay around source validity, actual acquisition evidence, selected native strategy, and exported LDS/AA data.

**Stop:** do not ship a native-DR force-enable/configuration patch, promise SD-free NNG operation or out-of-region-map DR, or integrate SMDB-polled yaw as if every successful poll were a new measurement. These are specific unproved contracts, not an absence of a native DR implementation.

The most relevant new gate is resolved: DISTANCE_GYRO_2 eligibility itself checks sensor-bundle timing/validity, not map coverage. A separate, concrete profile gate can suppress registering that strategy entirely.

## 1. Eligibility is a sensor interval check

NNG `DR_STRATEGY_DISTANCE_GYRO_2` vtable+0x0c points to `0xde1b90`:

```c
// Names describe observed roles; they are not recovered debug names.
if (strategy.byte_at_1510 == 0)
    return false;
return strategy.int_at_2f34 >
       (strategy.int_at_1504 - strategy.int_at_150c);
```

ARM uses a signed comparison at `0xde1bc8`; do not silently replace this with an unsigned or wall-clock-age API. The original code does not add an explicit nonnegative-delta test here.

`0xe07cc4–0xe07d0c` resolves the limit:

- section string `dr.distance_gyro` at `0x119fe6c`;
- key `sensordata_maxintervalms` at `0x119fe80`;
- compiled default 5000 at `0xe07ce4`;
- negative result clamped to zero at `0xe07cf8`;
- stored in strategy+0x2f34 at `0xe07d0c`.

The supplied `config/gps_profiles/jci_cmu_gps_and_sensor.ini` overrides this value to **10000 ms**. That value applies only if that profile wins actual configuration selection.

There is **no map/geography call in this eligibility function**. This rules out claiming that its immediate boolean test is “has a matching road/map.” It does not rule out map, license, or initialization conditions elsewhere in native navigation or its position calculator.

## 2. Timestamp/validity are copied from input bundles

The concrete strategy input method at `0xeeb240` (vtable+0x24) copies:

| Incoming bundle field | Strategy field | Instruction |
| --- | --- | --- |
| 32-bit `+0x14` | `+0x1504` | `0xeeb248–250` |
| byte `+0x18` | `+0x1508` | `0xeeb258–260` |

The processing path advances previous timestamp/validity using current timestamp/validity at `0xed9924–0xed9940`. The excessive-interval branch similarly copies them at `0xed8ad4–0xed8ae0`.

This is **input-bundle timing**, not the time a new logger polls SMDB. The source epoch, wrap/reset contract and relation to physical sensor sampling versus transport reception are not established by these copies. The byte is treated as a valid/present flag by the observed time code; it is not evidence of a sensor accuracy/quality classification.

Also, a comparison between two bundle times is not by itself a guarantee that wall-clock silence will immediately invalidate the value. Do not borrow this predicate as a complete freshness watchdog without tracing the scheduler and clock updates.

## 3. A real profile gate can remove the strategy

The DISTANCE_GYRO_2 constructor at `0xecca54` initially seeds priority 350 (`0x15e`) at strategy+8. It forms a configuration key from its strategy name and reads the UTF-32LE section `dr.mode_priority` (string at `0x109a66c`) at `0xeccb40`, replacing the priority at `0xeccb44`.

Fusion registration `0xa6cb9c` checks strategy+8 at `0xa6cbb0`. If zero, it calls the strategy destructor at `0xa6cbc8` and returns. If nonzero, it inserts the strategy into fusion collection+0x2b40 (`0xa6cbf4`) and invokes selection (`0xa6cc1c`).

The supplied `config/gps_profiles/jci_ublox_m8.ini` explicitly contains:

```ini
[dr.mode_priority]
distance_gyro_2=0
wheel_integrator=0

[dr.jci]
enabled=0
```

Therefore **if this profile applies**, distance_gyro_2=0 is a genuine registration barrier. It is not merely a descriptive setting. It must not be flipped speculatively: another receiver/DR architecture and different input availability may be intended. The legacy GPS/sensor profile and the u-blox profile must remain distinct cases in the design.

## 4. Map coverage, offroad behavior, and SD boundary

The stock legacy GPS/sensor profile includes map-match feedback parameters and `offroad_handling=2`, `offroad_distance=15`, and `offroad_garage_mode` under strapon settings. These are evidence that offroad behavior is configurable. The numeric meanings, full map-free initialization path, and successful out-of-region operation are **not established**. Do not translate `offroad_handling=2` into “works without maps.”

The already recovered `svcjcinavi.so` lifecycle remains the governing stock boundary:

- No-SD idle path `0x28cc8` checks NNG state and requests LDS START at `0x28d68–0x28d78` (or remembers START while waiting for READ_READY).
- `SDCardSeemsOK` startup requests LDS STOP at `0x292f8`; further startup branches contain `LDS_CONTROL_ReadControl(LCC_READ_STOP) before startNNGProcess()` at `0x29a74` / `0x29d64`.
- `NNG_PROCESS_STOPPED` requests LDS START at `0x27f14`.

Consequently, the proven ordinary no-SD position provider is LDS; these paths do not launch a standalone NNG DR-only provider. We found no proven supported/default SD-free NNG DR path in this scoped analysis. This is not a proof that engineering such a path is impossible. Do not make obtaining a different SD a promised fix; map/license/startup behavior remains separate from the resolved DR output route.

## 5. SMDB yaw loses source timing and count

The existing evidence permits a firmer acquisition contract than “poll faster”:

- `libjcimod_can.so!VBS_BUS_CAN_YawRate_BrakeControl` `0x1bbe4` builds public message `0x0116` from selector `0xd900`. Its 16-byte message contains ID at+0, header word at+2, raw uint16 at+4, count byte at+6, and two copied timestamp words at+8/+0x0c (`0x1bc10–0x1bc20`). The selector is not proven to be a physical CAN arbitration ID.
- `libjcimod_vdt.so` at `0x1adac–0x1adc0` divides raw by the count when nonzero. **Count is an averaging divisor, not an established sequence counter.** If count is zero, it logs and still proceeds.
- At `0x1ae04–0x1ae08`, VDT passes only the resulting uint16 into `VBS_VDT_SendSignal_YawRate` (`0x10dd0`). The original count and timestamp are not passed along this call.
- `libjcivdm.so!VDM_ReadCVD_YawRate` `0x6871c` uses `SMDB_ReadIntData("YawRate")` at `0x68778` and writes only a uint16 to the caller at `0x68790`. Its local fallback is 4094. The getter does not return the original message timestamp/count or a source sequence number.
- VDT's missing-CAN path supplies 4094 at `0x19d40–0x19d44`. Reject that demonstrated sentinel; acceptance by the storage setter is not physical validity.

**Logger contract:** for this SMDB getter, retain poll receipt time as `observed_at`, mark source time/sequence unavailable, and do not infer freshness from value changes or repeated read success. Constant yaw may be a legitimate straight/stationary sample; a changed number alone does not supply the producer timestamp contract.

An event observer before the VDT reduction can preserve more evidence (timestamp words, divisor count, message type and receipt time), but the callback registration ABI, timestamp origin/reset behavior, threading and guaranteed delivery still need exact validation before enabling an estimator. This document does not prescribe a guessed hook ABI or a raw socket takeover.

## 6. What to implement now

1. Keep source identity explicit: LDS ordinary provider versus NNG SENSOR_FUSION selected strategy. Do not conflate GPS receiver selection with the algorithm providing location.
2. In traces/replay distinguish sensor source time, source validity and logger receipt time. Represent unavailable fields as unavailable.
3. Permit native provider analysis when NNG is already running and producing mode3; the existing fusion→GetPosition→OEM AA route is implemented.
4. Keep any new yaw integration disabled when its only evidence is SMDB polling without a validated freshness mechanism. Offline synthetic/replay development can proceed independently.
5. Do not force strategy priority, SD state, position mode, accuracy, or timestamp to conceal an unresolved producer contract.

## Evidence files

The original analysis used `gates.asm`, `provenance.json`, disassembly scripts,
and stock binaries in a private evidence bundle. Those extracted OEM artifacts
are not published here. Addresses, hashes, and interpreted contracts above are
retained; independently reproducing binary claims requires matching stock files.
Their omission is not a claim that firmware analysis was re-run during import.

No full map-coverage proof or source-clock provenance was claimed. The resolved eligibility gate and explicit profile rejection are enough to refine the implementation without another request for vehicle data.
