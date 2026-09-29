# CMU USB installation correction — 2026-09-29

**Correction after OEM execution:** the original account fixture in this record
used a non-root `cmu` and did not match the official NA 74.00.324A passwd update.
That account conclusion and the earlier local ZIP are superseded by
[OEM_RUNTIME_2026-09-29.md](OEM_RUNTIME_2026-09-29.md).
The historical checks below are not rewritten as new results.

This record covers the installation correction based on `be1179350b4fb6bfb3a9e6e264d6ad85e969bde4`
(v0.3.0-shadow.1 source), not the earlier release artifacts. The supplied private
NA 74.00.324A firmware was extracted locally. All four original-file SHA-256
identities matched `packaging/firmware.sha256`; that manifest was not changed.
No firmware bytes, account credentials, personal share URL or vehicle logs are
included in source or the USB ZIP.

## Failures established and corrected

| Failure | Correction and evidence |
| --- | --- |
| No diagnostic terminal entry on a clean USB | Four generated silent MP3s and the attributed upstream diagnostic JS; flat ZIP root; sda1–sdd1 ID3 title checks |
| Missing sha256sum | The firmware's BusyBox is **1.19.2**, without this applet. Static ARM helper, copied off non-executable USB files to a private `/tmp` directory; full SHA256SUMS verified without `-c` |
| Storage links rejected | Actual rootfs has `/data_persist -> /mnt/data_persist` and `/mnt -> /tmp/mnt`; installer, guard and parked helpers support both |
| Root account name assumed | Numeric UID 0 for ownership; emulated NSS contains UID 0 named `jci`, with no `root` entry |
| Read-only SM/config staging | Last equal mountpoint wins over the `rootfs / rw` pseudo-entry; canonical paths select the correct persistent mount. Existing SM configs are read and backed up without rewriting during install |
| Guard rejects stock permissions | OEM `/jci`, `/jci/sm`, SM configs and autostart are 0775 in the archive. Private ownership/no-symlink checks apply inside our installation; baseline files remain regular-file checked and hash-bound |
| BusyBox awk cannot edit existing touch preload | **Found by executing the actual stock ARM awk:** adjacent variable/parenthesis concatenation was parsed as an undefined function call. Added explicit empty-string concatenation; escaped regexp slashes also work on BSD awk |
| Recovery/export also assumes desktop tools | Hash helper installed with local tools; uninstall remounts automatically; USB export remounts/restores its destination; collector launch ignores HUP without requiring nohup |
| Cleanup order and late guard failure | Remove owned staging/lock files before restoring read-only mounts while retaining a separate stock `flock` through restoration; run production `guard check` before autostart publication, without creating an arm |
| Touch edits racing template creation/rearm | Generate from a copied snapshot and keep its SHA-256 beside each template. Guard check/arm/select reject a baseline that no longer matches that snapshot |
| Removal rewrites clean OEM files before staged data is synced | Sync staged contents before rename and retain unchanged OEM inodes; cleanup also removes owned second-pass awk temporary files |

## Executed checks

Host: Apple Silicon macOS with isolated Debian 12 Linux containers. Pinned ARM
compiler: m3-toolchain `61ec0343de84f6fc7c46840056df1d600d44be8a`, GCC 4.9.1,
ARMv7 Cortex-A9 NEON softfp. No sudo command was used.

| Check | Result |
| --- | --- |
| `make test` with private stock rootfs and current five-artifact bundle | **PASS, no skips**. Packaging 83 tests; recovery/editor 28; journal/analyzer 50; collector 10; PC analyzer 20; loader harness and existing core/navigation/adapter/runtime/integration checks all passed |
| `tests/run_arm_all.sh` | **PASS** in QEMU using the pinned compiler/sysroot, including real ARM veneer, loader, core, sensor tap, navigation, calibration, holdout and journal fixtures |
| Expanded recovery/editor harness built for ARM/QEMU | **28 tests PASS**, including full stock alias chain, 0775 OEM files, owned-file rejection, snapshot identity, check-without-arm and durability faults |
| Stock ARM BusyBox/libc chroot, production scripts/binaries, ZIP manifest | **PASS**: corrupt USB rejection before persistent writes; plain `sh install.sh`; actual guard check/arm/select; same/new-boot refusal after consumption; later touch edit declines raw arm, explicit rearm preserves the new touch setting; removal/reinstallation |
| Stock dynamic loader with each experimental DSO on `/bin/true` | **PASS**. Each must emit its `LD_DEBUG=libs` initialization record without a preload warning; a missing-DSO negative control confirms exit status alone is insufficient. This checks library initialization, not binding in OEM AA/VBS services |
| Production collector on stock libc/DBus | **PASS**: actual drop to cmu UID, owned journal, collector_boot and collector_stop. Missing vehicle bus/SMDB is an expected emulation condition, not sensor validation |
| Parked status/finish and USB export under stock tools | **PASS**. Status sensor/health rows and completion acknowledgement are explicitly synthetic. Export tar bytes and SHA-256 verified; original logs retained |
| Existing AA touch placement | Reproduced the successful installer's insertion immediately after the jciAAPA opening line in both real stock SM files; install/uninstall preserved those files byte-for-byte |
| Bundle | Five ARM artifacts, MP3/JS and all helper files; flat ZIP, internal manifest and external ZIP SHA-256 verified after extraction |
| ELF inspection | ARM32 little-endian; no TEXTREL or dynamic libstdc++; four dynamic artifacts require GLIBC_2.4 only; hash helper is fully static |
| Additional macOS CLI/hash checks | Mount/default policy 16 tests and native hash/fallback tests passed |

The chroot harness is `tests/packaging/cmu_emulation.py`. It uses a native Linux
container and ARM binfmt/QEMU, verifies that the guest actually reports stock
BusyBox 1.19.2, and never executes the OEM init, autostart, Service Manager, AA or
VBS services. An attempted PRoot route did not provide a reliable guest on this
host and was discarded; its output is not counted as successful verification.

**Mount operations are simulated.** The harness supplies duplicate rootfs/rw and
real-root/ro records, separate persistent and read-only/noexec USB records, records
remount calls and rejects OEM cp staging while its model is read-only. It verifies
selection/restoration logic without remounting the host. USB execute bits are
removed. Physical vfat mount enforcement, the CMU kernel's relfs remount behavior,
flash durability, electrical power loss and real boot ordering remain target
conditions. `/config-mfg/passwd` is on a separate factory partition, so its account
data is modelled from the reported UID/name condition rather than extracted.

Claude Code was actually invoked for separate read-only reviews. Its findings
on mount selection, stock ancestor permissions, USB export, hash failure masking,
preflight and removal durability informed these changes. Four independent Codex
reviewers covered installer/recovery, ARM guard/runtime, tests/ZIP and end-to-end
integration. Reproduced findings included the unlock/remount race, stale-template
rearm, a false-positive DSO test and omitted analyzer provenance. These were
corrected and regression-checked. The final integration reviewer also independently
ran the 16 CLI/mount and 28 guard/editor tests successfully. Review is not hardware
execution evidence.

## Reproduction

Supply an extracted, matching private stock rootfs. Keep it out of Git. Build with
the pinned toolchain and prepare the SHADOW bundle:

```sh
python3 tools/make_usb_zip.py --build-dir build --default-mode SHADOW \
  --output build/usb-test.zip
unzip -q build/usb-test.zip -d build/usb-test
MX5DR_STOCK_ROOT=/private/stock/rootfs MX5DR_RELEASE_BUNDLE="$PWD/build/usb-test" make test
```

Inside an isolated Linux UID 0 container with ARM binfmt/QEMU available:

```sh
python3 tests/packaging/cmu_emulation.py \
  --stock /private/stock/rootfs --bundle "$PWD/build/usb-test"
```

Build-info records the exact source commit, source file hashes and whether the
source was modified. A local ZIP and local commit are not a published GitHub
release. Existing release assets are not overwritten.

## Artifact identities

| Artifact | SHA-256 |
| --- | --- |
| libmx5dr.so | `c5afb4846c3b2481509ff8c0f33c0999defbddd7dfd6c13b855abf61eea48c38` |
| libmx5dr-vimtap.so | `200adc73b4c3c0615c27a6853c5ade05c065761478df8ed01fd134606564527c` |
| mx5dr-collector | `a3b5c6b95478a36cd18d562534688fc375c12b038388f7a169d665586116b42e` |
| mx5dr-guard | `2377cec0911a1e49dd8b82bb893b4e8d631efa3220f2b05796d63ed5fc278a5b` |
| mx5dr-sha256 | `358f5d8efc9405606f362d81c8441a4eef7d94e2acfc5453d8e69d134961f88b` |

## Remaining boundary

No physical CMU, phone or navigation app was exercised. MP3/HMI triggering is
the established upstream mechanism with checked paths, not a newly performed
vehicle test. SHADOW still records MODEL calculations while preserving OEM
sends; live ASSIST remains disabled. Same-running-SM retries, actual sensor
cadence and phone acceptance are unverified. An abrupt kill/power loss can leave
an installation lock or pending transaction requiring the documented recovery;
the installer does not guess that another active installer is stale.
