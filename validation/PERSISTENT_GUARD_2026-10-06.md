# Persistent BETA guard — verification (2026-10-06)

Design: [PERSISTENT_GUARD_DESIGN_2026-10-06](PERSISTENT_GUARD_DESIGN_2026-10-06.md). All results below are
offline (host, QEMU user mode, stock BusyBox emulation). Nothing ran on a CMU or in the vehicle; this
record approves no vehicle visit or drive.

## Host

- `python3 tests/recovery/test_guard.py`: 36 tests OK — the one-boot suite, unchanged.
- `python3 tests/recovery/test_guard_persistent.py`: 28 tests OK, two builds:
  - rule OFF (shipping default, 22 tests): manifest/binding rows; the enabling boot never selects and
    every later boot does; same-boot double selection (also with a lost `last-boot`); 8 concurrent
    selectors → 1; one failure then a good boot resets the counter; two consecutive failures trip,
    stay tripped for 5 boots, re-enable restores; reset detection with equal size and equal 1970
    mtime (content only), mtime-only change, new and removed report names; non-`.out` and
    directories ignored; `/data → /mnt/data → /tmp/mnt/data` alias, foreign alias and missing
    directory decline without state change; > 64 reports decline; report symlinks not followed;
    `disable-next-start` trips; stale capture freeze cleared only on selection, never followed or
    recursed; healthy marker read but rule off never fails; nine malformed/forged/oversized/NUL/wrong-boot
    markers, a symlinked marker, a FIFO marker and a symlinked `logs` are "not healthy"; payload, VBS
    tap, LDS tap, touch baseline, template and config changes decline without state change; non-BETA
    config never enables/selects; `arm` and `persist` exclusive; damaged, symlinked, loose-mode
    state/manifest and foreign rule/mode/schema manifests decline; fsync failure at `trial-dir`,
    `persist-state`, `last-boot` never prints a path, unconfirmed rollback exits 3; failed `enable`
    publication revokes; a lost trip record is re-derived from the same evidence.
  - rule ON (`-DMX5DR_GUARD_HEALTHY_RULE=1`, 6 tests): healthy sequence; 3 attempts without the marker
    fail and the next one trips (`boot_loop`); unhealthy-but-no-reset keeps the counter, healthy resets
    it; a reset after a healthy marker still fails; an `off` manifest declines in an `on` build.
- `tests/packaging/test_persistent_beta.py`: 6 tests OK — BETA install is persistent (no arm/armed-boot),
  SHADOW stays one-boot, `--one-boot` BETA; re-install clears a stale `persist` and acknowledges
  `disable-next-start` (BETA only); uninstall/arm.sh remove `persist`; a multi-boot host run of the real
  guard source against the installed fixture tree (install boot → 3 product boots → reset → reset →
  trip, NO-GO lines, menu 3 export with the persistent lines and a clean guard inventory → menu 1 →
  product); invalid state is not reported as selected. Every verdict line ≤ 40 columns.
- Focused packaging suites (`test_trial_menu`, `test_trial_status`, `test_synthetic_install`,
  `test_install_defaults`, `test_packaging`): 210 tests OK, 3 skipped (pre-existing optional
  release/private fixtures). `test_startup_diagnostics`, `test_shell_bundle`: OK.
  One assertion changed: the BETA menu line is now `1 Install or re-enable BETA, every boot`
  (requested behavior change; the test now also requires `policy=persistent`).
- Full host suite `make -k test` with `MX5DR_STOCK_ROOT` (on the packaging commit plus these docs): exit 0;
  packaging discovery 333 tests OK, 4 skipped (release-bundle/private inputs not set); replay of the private
  trip skipped (`MX5DR_TRIP_DIR` unset).

## Exact ARM

Pinned GCC 4.9.1 (`tools/m3-toolchain`, cortex-a9 softfp) built the guard; `qemu-arm -L <sysroot>`
(`/tmp/qbin/qemu-arm`) ran it under both suites: `test_guard.py` 36 OK, `test_guard_persistent.py` 28 OK
(both rule builds). `tools/build_arm.py` produced a verified ARM build of commit `1343e0b`.

## Stock BusyBox 1.19.2 multi-boot simulation

`tools/make_usb_zip.py --default-mode BETA` from that build (self-contained: mp3/js entry files,
`build-info.json` `install_policy=persistent`, `source_modified=false`, `SHA256SUMS` all OK). The
unpacked ZIP was driven with the proot+qemu-arm copy of `tests/packaging/cmu_emulation.py` (stock
BusyBox, stock dynamic loader, real ARM guard); boots are authored `boot_id` changes, SM resets are
authored rewrites of `/tmp/mnt/data/*.out` behind the stock `/data` alias, and the SM configuration
chosen per boot comes from executing the owned block extracted from the *installed* `/usr/bin/autostart`
(collector launch path disabled because it would outlive the guest). 56/56 checks passed:

1. Menu 1 in boot 0: persistent install text, `policy=persistent`, real ARM `persist` (582 bytes), no arm,
   both SM configs free of our token; menu 2 `NO BOOT installed; choose 5`; SM restart in boot 0 → stock;
   menu 5 witness.
2. Boots 1, 2 → `/tmp/mx5dr-trial-*/sm.conf` with AA/VBS/LDS preloads; menu 2 `PERSIST enabled (every
   boot)`, `this boot selected yes`, `ok BOOT product this boot`; second run in boot 2 → stock.
3. Reset during boot 2 → boot 3 still product, `fail_count=1 previous=failed_reset`, `fail count 1 of 2`.
4. Reset during boot 3 → boot 4 stock, `tripped=reset_reports`, `PERSIST tripped (reset_reports)`,
   `NO-GO: tripped, stock runs, BETA off.` / `Find the cause, then run menu 1 again.`; boot 5 stock.
5. Menu 3 export (`export_exit=0`, SM reports in the archive, tripped state in `trial-result.txt`);
   menu 1 re-enable (state reset, fenced to boot 5); boot 6 product again.
6. Menu 4: `persist` removed, autostart byte-identical to stock; boot 7 stock; the guard declines.

Every verdict line ≤ 40 columns; no awk/shell error text. Not covered: OEM PID 1, the real SM and its
report writer, AA/VBS/LDS processes, the collector at boot, real reboots, power loss.

## Not verified

- Any CMU execution; whether every SM-supervised reset writes new `/data/*.out` (one vehicle observation
  that normal boots do not); resets that leave no report; real storage fsync behaviour.
- The boot-loop rule in production: compiled off until the runtime writes `healthy`.
- `tests/packaging/cmu_emulation.py` (chroot, UID 0 container) was updated for BETA but only its proot copy
  ran here.
