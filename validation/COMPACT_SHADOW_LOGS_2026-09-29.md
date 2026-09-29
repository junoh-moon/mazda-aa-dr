# Compact SHADOW journal verification — 2026-09-29

Base: `fba225982364250ead4aee8d36604d68dadc3652` (merged PR #12 plus the release procedure). This record covers the compact-journal diff, not a published installation release. No physical CMU, OEM executable, phone or map app was used.

## Behavior checked

The worker still feeds every channel-accepted RawEvent into the same navigation pipeline. The new representation preserves ten per-event integer fields and the epoch, including rejected model inputs, zero counts, signed source-time values and maximum-width integers. It does not sample inputs or qualify receipt time. Pending batches are written before channel-reset markers and before leaving each bounded receive turn.

The analyzer understands old, compact and mixed records; validates batches atomically; tracks sequence high-water marks without rewinding; and preserves SHADOW fault information in reports. MODEL and ASSIST remain distinct.

## Executed checks

| Check | Result and scope |
| --- | --- |
| Host `make test` | Exit 0. Includes 14 new journal/decoder tests, existing 20 analyzer cases, runtime journal fault/ordering cases, navigation 604, raw-to-adapter 815 / 7 sends, core 1,425, adapter 11, runtime 38, loader 29 scenarios, guard 22 and collector 10 |
| Stock packaging fixtures | 30 discovered, **20 skipped** because private stock files are absent; remaining 10 passed. No new exact-stock installation validation claimed |
| Kernel Unix socket test | **Skipped (EPERM)** on host and under QEMU. Pure codec/cursor assertions ran; this change does not modify the channel |
| Pinned ARM production build | `make arm` passed with m3-toolchain `61ec0343de84f6fc7c46840056df1d600d44be8a`, GCC 4.9.1, ARMv7 softfp; four artifacts built |
| ARM/QEMU suite | `tests/run_arm_all.sh` passed, including the new max-width encoder fixture, actual runtime journal helpers, navigation/adapter/core/veneer and production preload non-target smoke. Socket skip as above |
| ARM encoder → Python decoder | Same 14 journal tests passed with the encoder fixture executed by qemu-arm; all integer values matched host expectations |
| Host ASan/UBSan | Encoder empty/full/epoch/max-width fixture passed. Leak detection disabled for this sandbox; production batching performs no heap allocation |
| Independent Astra review | Reviewed ordering, bounds and analyzer behavior; independently ran all 14 new tests. No remaining substantive finding after the correction below |

The new tests cover 0/1/31/32/33/256/1,000-event batches; 32 maximum-width rows; a 9,000-event volume/round-trip comparison; legacy/mixed input; malformed-batch atomic rejection; source restarts versus runtime boots; rotation; replay/gap handling; health coverage; MODEL/preview consistency; and SHADOW fault/numeric validation.

The runtime journal fixture exercises production `journal_motion`/`flush_motion` helpers at full/epoch/fault boundaries, duplicate empty flushes and `/dev/full` failure, alongside existing open/rotation/size/queue failure paths.

## Review correction

Initial analyzer support could ignore OVERFLOW and positive resets/rejected counters; malformed negative counters also passed. Astra demonstrated this with synthetic input. The final analyzer validates ranges, reports status distributions and counter maxima, marks observed faults inconclusive after recovery too, and requires an encoded preview to be model-valid. Two regressions were added; the final 14-case suite passed on host and ARM/QEMU.

## Synthetic storage comparison

9,000 mixed raw records, eight events per line:

| Representation | Bytes including newlines |
| --- | ---: |
| Previous per-event JSON | 1,744,442 |
| Compact v1 batches | 585,692 |
| Reduction | 66.4% |

Every field and its order were recovered. This is deterministic synthetic input, not measured CMU cadence. Other trace records and collector output are unchanged. It does not establish full-drive retention or remove rotation/power-loss limitations.

## Reproduce

```sh
make test-motion-journal test-runtime test-tools
```

For the pinned ARM build/full suite, see [the release procedure](../docs/RELEASING_KO.md). After `tests/run_arm_all.sh` has built its fixture:

```sh
MX5DR_MOTION_FIXTURE="qemu-arm -L $QEMU_SYSROOT $(pwd)/build/arm-full-tests/motion-batch-test" \
  python3 -m unittest discover -s tests/journal -v
```

Set QEMU_SYSROOT to the pinned sysroot and put qemu-arm on PATH. Sandbox dependency provisioning is not a project change. Private stock fixtures were not re-downloaded for this journal-only PR; skips remain explicit. Actual VBS receipt, CMU filesystem performance, sensor calibration, location error and phone acceptance remain unverified.
