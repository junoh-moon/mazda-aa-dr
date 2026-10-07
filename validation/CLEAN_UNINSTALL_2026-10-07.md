# Menu 6 complete removal ("clean uninstall") (2026-10-07)

Scope: host fixtures and an offline stock-BusyBox replica only. No vehicle, CMU, phone or
dongle was used. Nothing here was published or released.

## Problem

The owner wants the head unit to look as if the package had never been installed. Menu 4
(`uninstall.sh`) deliberately keeps `/data_persist/mx5-aa-dr` (library, tools, logs,
backups) for mapped-code lifetime and diagnosis, and the owner's keyboard cannot type
`rm -rf /data_persist/mx5-aa-dr` (underscore needs Shift). The removal therefore has to be
a numbered menu entry.

## Changed behaviour

`packaging/purge.sh`, menu `6 Delete everything this package left on the CMU (run 4, then
5, first)`. Menu 4 now ends with `After a CMU reboot (menu 5), menu 6 deletes them all.`

1. Read-only preconditions. Each failure prints `Refused, nothing deleted: <reason>` and
   exits without deleting or writing anything (the vehicle path may create the tmpfs mount
   lock `/tmp/.mx5dr-mount.lock`, which every installer/export already uses):

   | exit | condition |
   | --- | --- |
   | 3 | `guard/arm` or `guard/persist` present; `/usr/bin/autostart` contains `MX5DR ONE-BOOT` or `/mx5-aa-dr/`; `sm.conf` or `sm_WCP.conf` contains `libmx5dr` or `/mx5-aa-dr/` |
   | 4 | a `/proc/[0-9]*` process has the package directory, `/tmp/mx5dr-trial-*`, `/tmp/mx5dr-hash.*` or `/tmp/mx5-lds-association-*` in `maps`, `fd` (link targets) or `cmdline`; any of these entries unreadable for a process that still exists ("cannot prove"); no readable process entry at all |
   | 5 | the collector holds the kernel `flock` on `logs/collector.lock` (checked read-only, in a subshell) |
   | 6 | `.mx5dr-install-lock` present and not provably stale |
   | 7 | unexpected type (package directory or collector lock is a symlink, unreadable autostart) |
   | 1 | another installer/export holds the mount lock (existing `lock_mounts` message) |

   A leftover install lock is stale only on the vehicle path, where menu 6 already holds the
   kernel mount lock that every installer, rearm, uninstall, export and purge keeps until it
   has removed the lock directory (also true for v0.3.12-shadow.5); it must contain only
   `pid`. It is then reclaimed and reported (`stale_install_lock_reclaimed=yes`). Fixtures
   always refuse.
2. Comparison, before anything is deleted. Each `backups/<YYYYMMDDTHHMMSS-pid>` set whose
   three `.before` files match their `.sha256` and contain no owned block or token is a
   pre-install record. The basis is the lowest name among them; the report gives the set,
   the number of complete records, whether all agree, and the sets that were incomplete or
   not originals. The set names use the CMU clock, which restarts at 1970 on this vehicle
   (exported names `19700101T000140-5752` ... `19700101T002334-12846`), so "oldest" is
   nominal; on the vehicle export all four complete records agree byte for byte. Each of
   `/usr/bin/autostart`, `/jci/sm/sm.conf`, `/jci/sm/sm_WCP.conf` is reported
   `identical` or `differs`; a difference (for example a later touch-mod edit) is reported
   and never restored.
3. `purge-result.txt` on the USB is written (tmp + rename + sync) with `status=started`, the
   comparison, the full delete list (`kind bytes path`) and the byte total before the first
   deletion, and rewritten after it with `after_package_directory=absent|present`, the
   install lock and any remaining leftover. An earlier report is kept below
   `---- previous purge-result.txt ----` (first 32 KiB), so a verdict written before a power
   cut survives a second run.
4. Deletion, under the persistent-storage remount and the install lock: interrupted staging
   copies next to the OEM files (`autostart.mx5dr-new|remove.<pid>`,
   `sm.conf|sm_WCP.conf.mx5dr-remove.<pid>[.tap|.lds.<pid>]`, the root file system is
   remounted rw only if one exists), `/tmp/mx5dr-trial-??????` (only `sm.conf` inside),
   `/tmp/mx5dr-hash.??????` (only `hash` inside), `/tmp/mx5-lds-association-??????`, then
   `guard/` + sync, then the whole package directory + sync, then the install lock. Names that
   do not match exactly or directories with unexpected content are listed under
   `not_deleted_unexpected` and left. Never touched: `/jci/*` and the autostart content,
   `/data/*.out`, `oem-aa-mod`, `/tmp/.mx5dr-mount.lock` (other invocations may hold its
   inode; tmpfs drops it at boot). Mounts are restored by the existing `cleanup` trap.
5. Power loss: autostart and both service configs no longer reference the package
   (precondition, re-checked under the lock), so every intermediate state is inert; running
   menu 6 again completes the removal. The second run prints `Nothing to delete` and writes
   nothing.

Inventory of what the package creates outside its directory was taken from `install.sh`,
`common.sh`, `uninstall.sh`, `arm.sh`, `export_logs.sh`, `reboot_cmu.sh`, `guard.cpp`
(`atomic_file` temporaries `.<name>.<pid>` stay inside `guard/`; `/tmp/mx5dr-trial-XXXXXX`),
`runtime/lds_association_channel.cpp` (`/tmp/mx5-lds-association-XXXXXX`, unlinked after
setup unless interrupted), `collector.cpp` and `runtime.cpp` (everything under `logs/`).
USB files (`mx5dr-logs-*.tar`, `mx5dr-diagnostics-*`, results) are not CMU state and stay.

Only BusyBox 1.19.2 applets of the stock tree are used (`awk cat find flock grep head ls
mkdir mktemp mv rm rmdir sync tr wc`, plus the shipped `mx5dr-sha256` helper because the
stock tree has no `sha256sum`).

## Test evidence

- `tests/packaging/test_purge.py` (host fixtures, 16 tests, pass): every refusal (installed,
  `persist`, `arm`, token in either config, mapped `(deleted)` library, open log fd, SM
  cmdline on a `/tmp` trial, unreadable `maps`, empty `/proc`, collector flock held,
  install lock, symlinked package directory) leaves the file tree and the USB unchanged;
  menu text; totals; idempotence; touch-mod edit reported and kept; v0.3.12-shadow.5
  one-boot leftovers (`consumed`, `armed-boot`, `armed-boot.previous`, `last-boot`,
  `.persist-state.<pid>`, `pending`), several backup sets incl. an interrupted one and one
  with owned blocks, `persist-evidence`, staging copies and `/tmp` work files deleted while
  look-alike names stay; disagreeing records reported; earlier report kept; partial
  directory after power loss completed.
- `tests/packaging/replica_clean_uninstall.py` (not in `make test`; needs private inputs and
  proot). Replica root from the private stock tree with the vehicle's `sm.conf`,
  `sm_WCP.conf`, `version.ini` and pre-mx5dr `autostart.before` written over the stock
  copies, `oem-aa-mod` with the 0.9.1 DSOs and the vehicle `libpatch.conf`. Bundle: the
  published v1.0.0-beta.3 USB bundle (source `0dc5a9bb`, payloads unmodified) with this
  checkout's packaging scripts and a regenerated `SHA256SUMS`. Driven under proot +
  qemu-arm through the real `trial` menu; the real ARM guard ran through the installed owned
  autostart blocks (collector disabled, confirm delay 3 s). A simulated reboot clears tmpfs
  `/tmp` (except `/tmp/mnt`), changes the boot ID and authors a stock SM process
  (`cmdline`, `maps`, `fd`).
  - Scenario A (clean vehicle state): 54/54 checks. Menu 1 (persistent BETA) → menu 6
    refused 3; 4 product boots (normal, WCP, normal, normal) → menu 2 → menu 3
    (`export_exit=0`; `finish_exit=1` because no AA runtime acknowledges in the replica) →
    menu 4 → menu 6 refused 4 for a process mapping `libmx5dr.so` and for the SM still on the
    `/tmp` trial → reboot (stock) → refused 5 with the collector flock held from the host →
    refused (exit 1) with the mount lock held by "another installer" → a stale install lock
    and a stale `/tmp/mx5dr-hash.*` copy added → menu 6: 36 files, 4486275 bytes,
    `identical to the pre-install state`, package directory absent, install lock reclaimed,
    mount table back to its initial text → second run `Nothing to delete`, no change → next
    boot stock.
  - Scenario D (upgrade from the exported v0.3.12-shadow.5 tree, five backup sets, one
    interrupted): 62/62 checks. Same flow; 67 files, 39942433 bytes deleted; basis
    `19700101T000140-5752`, 4 complete records that agree; the interrupted set and the new
    beta set (its `autostart.before` holds the shadow.5 blocks) are not used.
  - Tree equality in both: snapshot of the replica before anything versus after menu 6 (D is
    compared with a separately built clean replica): 333 entries, 0 added, 0 removed, 0
    changed in path, type, mode, owner, regular-file sha256 and link target.
  - Listed, justified differences (not compared): directory mtimes of `/etc` (proot itself
    changes it on every invocation; nothing in `/etc` changed), `/jci/sm` (uninstall's staging
    copies created and removed), `/usr/bin` (autostart replaced), `/tmp/mnt/data_persist`
    (package directory and lock created and removed), `/tmp`; regular-file mtime of
    `/usr/bin/autostart` (a new inode from install and uninstall with identical bytes and
    mode). Excluded paths: `/proc` (authored kernel view; the mount table is compared
    separately), `/test-bin`, `/mount.calls`, `/drv.sh`, `/reboot.witness` (harness),
    `/tmp/mnt/sda1` (USB) and tmpfs `/tmp` outside `/tmp/mnt` (after menu 6 only
    `/tmp/.mx5dr-mount.lock` remains, by design). Owners are weak evidence under proot
    (`-0` does not apply `chown`).
- `make -k ... test-packaging` without the stock root: 356 tests, OK, 42 skipped (stock
  identity fixture unavailable, release bundle not set). Whole suite with
  `MX5DR_STOCK_ROOT=/tmp/jcidlg.IUmV3A/full`: see "Whole suite" below.

## Whole suite

`make -k HOST_DBUS_FLAGS=... HOST_DBUS_LIBS=... test` with
`MX5DR_STOCK_ROOT=/tmp/jcidlg.IUmV3A/full` on this branch: exit 0. Python groups: 47, 192, 37,
42, 1, 11, 356 (packaging, incl. `test_purge`), 69 and 6 tests, all OK; the C/C++ host
tests ran in the same invocation without failure. Skipped, reported explicitly: 4 packaging
tests (`MX5DR_RELEASE_BUNDLE` unset or the six-artifact release bundle not built:
`test_release_bundle_collector_binary`, `test_no_static_runtime_symbols_are_exported`,
`test_release_payload_with_existing_touch` in two classes) and the private trip replay
(`MX5DR_TRIP_DIR` unset). Exact-ARM tests (`tests/run_arm_all.sh`) were not run; no
ARM source changed.

## Not verified

- Nothing ran on the real CMU. Real kernel `/proc` (permissions, zombies, threads, the
  number of processes and the time of about 450 short reads on the CMU), real relfs flash
  behaviour on power loss during `rm -rf`, the real vfat USB remount and the real mount
  table were not exercised; the replica authors `/proc`, the mount table and `mount`.
- Whether any OEM process on the vehicle holds a package file after menu 4 and a reboot
  (expected none, because the trial is chosen only by the removed autostart blocks).
- The order of backup sets on the vehicle: names use a clock that restarts at 1970.
- The touch mod's files are protected by exact-name matching only; no real touch-mod
  install/uninstall ran alongside.
- `analyze_logs.py` and published ZIPs were not changed or rebuilt; no release was made.
