> **2026-09-28: installation on hold.** Historical 0.1 analysis/instructions follow. [Current status](STATUS_KO.md) and [review corrections](REVIEW_2026-09-28_KO.md) supersede installation GO statements. No vehicle or phone validation has been performed.

# Scoped CMU package: installation, removal and parked export

This package contains host-tested POSIX shell scripts intended for the CMU's
BusyBox tools. It has not been installed or executed on a vehicle. Firmware
identity checking and a basic ARM ELF check do not establish runtime ABI or
phone/app acceptance. The build/runtime validation gates still apply.

## Actual entry point

An **already authorized root shell or existing authorized installer** is
required to run the scripts. This package supplies no USB auto-execution exploit,
jailbreak, unlock or firmware flash. Merely putting it on USB does not install it.
The upstream oem-aa-mod project's installation guide documents per-service
environment injection; this package uses that scoped mechanism and does not alter the source
project's touch/HUD configuration or system-attribute XMLs.

The host prepares a complete bundle from the actual built shared library:

```sh
sh packaging/make_bundle.sh build/libmx5dr.so dist/mx5dr-bundle
```

`dist/mx5dr-bundle` must not already exist. It contains the installer, narrow XML
editor, fixed firmware identity manifest, runtime library and its SHA-256 sidecar.
The SHA-256 sidecar detects damage; it is not a release signature or a trust root.
The installer never executes an uploaded OEM binary or firmware updater.

Copy that whole directory to a mounted USB/SD volume or another staging directory
using the existing access method. From its directory, run:

```sh
sh install.sh
```

This defaults to OBSERVE. Explicit alternatives are `--mode=SHADOW`,
`--mode=SCRUB`, and `--mode=OFF`. SCRUB only clears stale LOCATION speed/bearing
when the runtime's stale-detection contract allows it; it does **not** compute
new DR coordinates. ASSIST is rejected. Runtime configuration is pinned to:

```text
/data_persist/mx5-aa-dr/mx5dr.conf
mode=OBSERVE
max_log_bytes=8388608
max_log_files=3
sample_ms=1000
```

Runtime configuration requires exactly one explicit `mode` entry. An empty,
whitespace-only, comment-only or otherwise mode-less file is invalid and keeps
hooks disabled; it does not select OBSERVE implicitly. Valid explicit modes may
omit numeric options to use their defaults. The installer default below is an
explicitly written `mode=OBSERVE`, not a parser fallback.

Every invocation explicitly stages the requested mode (default OBSERVE) and
these limits; it does not silently preserve a former SCRUB choice. Runtime reads
the configuration at cold load; scripts make no promise of immediate hot disable.

The default edits only `/jci/sm/sm.conf`. Add `--with-wcp` only when deliberately
installing for both known startup configurations. `/jci/sm/sm_WCP.conf` is never
auto-selected from a guess about the receiver or board. The installer checks the
real version/region/patch and SHA-256 of the four original firmware files:

- `/jci/aapa/blmjciaapa.so`
- `/usr/lib/libaap_interface.so`
- `/jci/lib/libjcilds-util.so`
- `/usr/bin/aap_service`

Unsupported or directly modified OEM binaries fail the gate. A preload-based
touch mod can remain in place because its separate library does not change those
four original files. This is the only accepted firmware set; do not replace the
manifest hashes to force another firmware through the check.

## What changes

The library is copied locally to `/data_persist/mx5-aa-dr/libmx5dr.so`. Only the
`jciAAPA` service receives the exact additional token, prepended to any existing
`LD_PRELOAD` list. Existing library tokens retain order and remain present; list
separators may be normalized to colons. Example:

```xml
<environ_var env_name="LD_PRELOAD"
 env_value="/data_persist/mx5-aa-dr/libmx5dr.so:/data_persist/oem-aa-mod/libpatch-blmjciaapa.so"/>
```

No OEM `.so`, `/etc/ld.so.preload`, launcher executable, CAN configuration, touch
shim, HUD service or AA attribute XML is replaced. The library and helper scripts
are local after installation, so the transfer medium can be removed and the
single working USB port used for Android Auto. No kill, restart or reboot command
is issued; the change applies at the next normal service start.

The editor deliberately supports the observed double-quoted, single
`LD_PRELOAD`, normal `jciAAPA` stanza, including multiline tags. Duplicate
stanzas/environment declarations, unexpected service paths, CRLF and unsupported
preload syntax fail closed. Comments are ignored for target matching. It does not
attempt to repair arbitrary XML or guess the intent of a modified launcher.

## Transaction and filesystem behavior

The package serializes its own operations with a lock directory. It preserves
config ownership/mode using `cp -p`, stages sibling temporary files and validates
all chosen launchers before replacing one. It compares the source file hash again
before rename to detect intervening edits. Existing configuration snapshots are
saved under `/data_persist/mx5-aa-dr/backups/`; removal does **not** restore these
snapshots over newer changes. The new library is copied and atomically renamed,
never truncated while it might be mapped. Logs and configuration are owned by
the existing `cmu` user; the service is not changed to run as root.

On the CMU, read-only mounts are detected through `/proc/mounts`. Default operation
refuses a read-only mount. From the authorized root shell, `--remount` explicitly
allows temporarily changing only the containing mounts to read/write. Exit and
signal cleanup restores mounts it changed to read-only and reports restoration
failure. Existing read/write mounts remain read/write. No fixture run can
remount. Abrupt power loss cannot run shell cleanup; after recovery inspect mount
state and the transaction marker before trying another install.

Renames are atomic per file, not an atomic transaction across both launcher files.
`pending` is written and synced before launcher commits. An interrupted commit
can leave one launcher changed; rerunning install refuses that pending state.
Run the removal script to remove this package's token from current launchers,
then retry. This preserves unrelated changes rather than overwriting from backup.
There is no validated crash-loop rescue or unattended autostart health monitor;
keep the existing authorized recovery access available for experimental loads.

An orphaned lock is not automatically deleted. Confirm no installer is active,
inspect `/data_persist/.mx5dr-install-lock/pid`, and only then remove that lock
through the authorized recovery method. Temporary `.mx5dr-new.*` files left by
an interrupted preflight are not active launcher files.

## Remove / roll back only this feature

```sh
sh /data_persist/mx5-aa-dr/tools/uninstall.sh
```

Use `--remount` under the same explicit rules if needed. Both pinned launcher
files are inspected, and only the exact mx5dr preload token is removed from
`jciAAPA`. Other tokens and unrelated edits remain. Runtime mode OFF is staged.
Removal intentionally does not require matching firmware: it must remain usable
after a firmware update. Unsupported launcher syntax still requires inspection;
the script does not blindly restore a historical whole-file backup.

Library, logs, helper tools and backups are retained. This avoids unlinking
diagnostic material and avoids pretending active code can safely be unloaded.
The already running process can continue until its normal termination. After the
next normal start, inspect loaded mappings with the existing diagnostic method
before claiming the module is absent. No automatic purge or `dlclose` is provided.

## Parked export

After parking, disconnect AA if the USB port is needed, connect the export medium
and use its actual existing mount path:

```sh
sh /data_persist/mx5-aa-dr/tools/export_logs.sh /mnt/sdb1
```

The path is an example, not a claim about this car's mount enumeration. The script
creates a timestamp/PID tar and SHA-256 sidecar containing only this module's logs
and configuration. It retains originals and never uploads them. Coordinates are
location history. A live writer can rotate during export; the script reports tar
errors and notes that final JSONL records can be partial. Runtime implements the
bounded log writer (three files, at most 8 MiB each by default); this exporter does
not provide a second unbounded onboard spool.

## Host verification scope

Run `python3 -m unittest discover -s tests/packaging -v` from the repository.
Python is used **only on the host**. Tests use copies of the provided firmware
files for their actual hashes and a deliberately fake ARM ELF header to test
packaging; the fake payload is never executed and is not a runtime test. A
separate fixture installs/removes the actual bundled ARM payload as bytes, while
preserving a pre-existing touch token; it also does not execute the payload.

Fixtures cover stock installation/removal, an existing multiline touch preload,
repeat installation/removal, unrelated drift, wrong firmware/payload/architecture,
explicit WCP selection, incomplete-transaction removal, duplicate declarations,
invalid second-file preflight, mode/remount guards and parked export. Tests run
against host `/bin/sh` and `awk`; actual CMU BusyBox command/loader behavior remains
unverified. A host test-root must be explicitly supplied in
`MX5DR_FIXTURE_ROOT` and contain `.mx5dr-fixture`; it does not bypass hash checks.
