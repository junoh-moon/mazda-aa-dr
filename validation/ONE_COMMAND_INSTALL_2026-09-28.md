# One-command installer validation — 2026-09-28

Base: `048ee575940fa810a45e34fd18c0afb81cec9e29`.
The production change is in `packaging/install.sh`: automatic temporary
remounts for ordinary target installs, `--no-remount` opt-out, optional bundle
manifest verification, and a completion message. Existing binary/firmware
checks, touch ownership, one-boot guard, and ASSIST blocking are unchanged.

Host test evidence:

- CLI policy tests execute the actual installer with a test-only `common.sh`
  that stops at the firmware-check boundary. Plain install selects OBSERVE and
  permits necessary remounts; legacy explicit options and fixture denial hold.
- Mount tests source the real `common.sh` and run its mount selection/exit trap
  with synthetic `/proc/mounts` input and mocked `mount` commands. Read-only
  mounts are restored on success and failure, original read-write mounts are
  preserved, repeated access does not double-remount, and restore failure
  makes the command fail. No host or CMU mount was changed by these tests.
- Packaging integration exercises default installation, valid/corrupted bundle
  manifests, symlink refusal, and existing touch preservation with the supplied
  private stock fixture. Stock files are read-only test inputs, never executed.

Final `make test` with the explicit private-fixture path: **passed, no skips**.
Packaging has 25 tests (13 existing, 10 CLI/mount tests, 2 manifest tests).
The remaining suite passed guard 14, loader 29 scenarios, core 1,425 checks and
replay, adapter 11 scenarios, config/SHA 38, journals, collector 10, tools 20,
and integration 8 sends. `sh -n packaging/install.sh` and `git diff --check`
also passed. No ARM rebuild is needed for this shell-only runtime-package change.

An initial new-test insertion misplaced
three pre-existing assertions; that test-layout error was corrected before
the final run. No release artifact has been silently replaced.

Limitations: no physical CMU trial; host mocks are not evidence of actual
mount behavior. Shell traps do not run after SIGKILL or power loss. Installation
completion does not prove the next AA boot, touch coexistence, or phone receipt.
The unchanged one-boot recovery boundary still applies. This is a draft source
change; the published v0.2.0-observe.1 ZIP still uses explicit `--remount`.
