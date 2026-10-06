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

## Revision G2 verification (2026-10-06)

After the G2 fixes ([design, revision G2](PERSISTENT_GUARD_DESIGN_2026-10-06.md#revision-g2-2026-10-06-after-two-independent-reviews)).
The results above are kept as the record of the first round.

### Host

- `tests/recovery/test_guard.py`: 37 OK. One new test: the autostart block backgrounds `confirm` only
  after a selection with `persist` present and never before the SM line. The branch test now also
  runs through a stub of the BusyBox `timeout -t 15 -s KILL` syntax, a hanging helper (stock config
  within 10 s) and the fallback without `timeout`; its earlier cases are unchanged.
- `tests/recovery/test_guard_persistent.py`: 42 OK (rule OFF 36, rule ON 6). New: probation trip on an
  unconfirmed first attempt; a no-report loop stops after 3 unconfirmed product boots; only a
  confirmed attempt resets the counters; alternating reset/confirmed boots trip at 3 resets in 10;
  window expiry; confirm rules (installing boot, SM absent, SM on another config, idempotent, a
  later boot id, missing commit marker); a confirm that never ran counts as unconfirmed; a tripped
  state cannot be confirmed; guard deadline at start, report scan and fsync (SIGALRM, no path);
  stale temporaries and an exec-preserved PID collision; decline reasons in `last-decision`;
  `verify` read-only under a held lock; one-boot select writes no decision. Changed expectations:
  `ok` is now `confirmed`; the power-loss test now ends in `tripped=unconfirmed` (undelivered
  attempts count as unconfirmed); the clock-free reset test re-enables before its last part
  because its R/C alternation now hits the cumulative cap.
- `tests/packaging/test_persistent_beta.py`: 11 OK. New: status/guard parser parity over eight
  corrupted states (never shown as enabled; the guard's select declines the same states), counter
  parity, unavailable guard, an edited `sm.conf` visible in the same boot and as the next boot's
  reason, pending confirmation waits, re-enable keeps trip/disable evidence (3 copies kept), upgrade
  from a v1.0.0-beta.1 one-boot BETA state.
- Full `make -k test` with `MX5DR_STOCK_ROOT`: exit 0; packaging discovery 338 OK, 4 skipped
  (release-bundle/private inputs not set); private trip replay skipped.

### Exact ARM

Pinned GCC 4.9.1 build of commit `5031cc4` verified by `tools/build_arm.py`; both guard suites with the
guard under `qemu-arm`: 37 OK and 42 OK (including the PID collision through `exec qemu-arm`).

### Stock BusyBox 1.19.2 multi-boot simulation (extended)

Self-contained BETA ZIP from that build (`source_modified=false`, `install_policy=persistent`), driven
as before with the real ARM guard, stock BusyBox and the stock `/usr/bin/timeout`. Each boot runs the
owned block extracted from the installed `/usr/bin/autostart`. Deviations: collector path disabled,
`/bin/sleep 90` shortened to 3 s, an authored SM launch writes `/proc/266/cmdline` in the vehicle `ps`
form (omitted to model an SM that rejects its config or a boot that ends early). 71/71 checks passed:

1. install (timeout wrapper and background confirm present in the installed autostart), installing
   boot stock, menu 5;
2. boots 1-2 product and confirmed by the ARM guard, probation ended, same-boot repeat stock;
3. two resets trip (`NO BOOT stock this boot` + ` (tripped:reset_reports)`, NO-GO lines);
4. menu 3 export, menu 1 keeps `backups/persist-evidence/1` and advises export;
5. probation: the SM never runs the trial, one product boot, then `tripped=probation`;
6. after re-enable, one confirmed boot and three unconfirmed boots, then `tripped=unconfirmed`;
7. `sm.conf` edited by another tool: `PERSIST enabled but bindings changed`, the folded message and
   `NO PERS bindings changed`; the next boot stock with ` (baseline_edited:sm.conf)`;
8. a guard stub that never returns: stock config after 15.2 s (stock BusyBox `timeout`);
9. menu 4: autostart byte-identical to stock, later boot stock.

Every verdict line ≤ 40 columns; no awk/shell error text.

### Not verified (G2)

CMU execution of any of this; the real SM argv and lifetime on a CMU beyond the one vehicle `ps`
line; the CMU power-down timing at ignition off; real hung storage (uninterruptible sleep); the
chroot variant of `tests/packaging/cmu_emulation.py` (updated, run only as its proot copy).
