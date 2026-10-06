# Persistent BETA guard — design and verification (2026-10-06)

Owner decision (binding): v1.0 is an always-on installed product that runs on every boot. One-boot,
N-boot or remove-after-test is not the product. Safety comes from an automatic fail-safe, not from
removal. This record describes the persistent policy of `mx5dr-guard`, its packaging and the
offline verification. Nothing here ran on a CMU or in the vehicle.

## Architecture kept

The one-boot design in [RECOVERY_2026-09-28](../docs/RECOVERY_2026-09-28.md) stays: `/jci/sm/sm.conf` and
`sm_WCP.conf` never contain our preload; the owned autostart block runs the guard before the SM launch;
only a printed `/tmp/mx5dr-trial-XXXXXX/sm.conf` replaces the baseline; a missing, failed or
malformed guard leaves the stock baseline. The persistent policy only changes *who authorizes* a
selection: instead of a consumed `arm`, a root-owned persistent manifest plus a per-boot judgement.
The autostart block (still marked `MX5DR ONE-BOOT`, so removal stays byte-compatible) is unchanged.

## Persistent files (guard directory, UID 0, 0600, never symlinks)

- `persist` — written by `mx5dr-guard enable`:

  ```
  mx5dr-persist-v1
  mode=BETA
  healthy_rule=off
  mx5dr-one-boot-v3
  <8 SHA-256 lines: the same inputs as arm>
  ```

  `select` requires byte equality with the manifest rebuilt from today's inputs, so a payload, tap,
  config, template or touch (baseline) change invalidates it exactly as it invalidates an arm. The
  config must also start with `mode=BETA`. 582 bytes.
- `persist-state` — written only by the guard, atomically (temp, fsync, rename, directory fsync):

  ```
  mx5dr-persist-state-v1
  enabled_boot=<uuid>            boot that ran enable (never selected)
  fail_count=<0..2>              consecutive failed attempts
  attempts_since_healthy=<n>
  attempt_boot=<uuid>|none       last selected boot ...
  attempt_reports=<sha256>|none  ... and the reset-report fingerprint at its selection
  previous=none|ok|healthy|failed_reset|failed_bootloop
  healthy_previous=none|yes|no
  tripped=no|reset_reports|boot_loop|runtime_disabled
  ```

  Refinement of the brief: counters, attempt record and trip flag are one file, so every transition
  is a single atomic rename and an attempt can never be judged twice. A parse is strict (exact keys,
  order, canonical numbers, re-serialization equality); anything else declines to stock.

## State machine (`select` when `persist` exists)

Preconditions (any failure: exit 2, no output, no state change): guard directory/lock/baseline/manifest
checks as before; no `arm`; `persist` equals the rebuilt manifest; `mode=BETA`; valid boot id;
`last-boot` ≠ current boot; valid `persist-state`; current boot ≠ `enabled_boot` and ≠ `attempt_boot`;
`tripped=no`; reset-report fingerprint computable.

1. Judge the previous attempt (only if `attempt_boot` ≠ none):
   - **FAILED (reset)** if the fingerprint differs from `attempt_reports`.
   - **FAILED (boot loop)** if the healthy rule is on, the previous attempt has no valid `healthy`
     marker and `attempts_since_healthy` ≥ 3.
   - otherwise `healthy` (marker valid) or `ok`.
   A valid marker resets `attempts_since_healthy` to 0.
2. `fail_count`: +1 on FAILED; 0 when not failed and (marker healthy, or the healthy rule is off);
   unchanged when the rule is on and the attempt was neither failed nor healthy.
3. `fail_count` ≥ 2 → `tripped=reset_reports|boot_loop`. A present runtime `logs/disable-next-start`
   (anything but ENOENT) → `tripped=runtime_disabled`. Tripped: write the state with no open attempt,
   publish nothing (stock). Every later boot declines at the precondition until `enable`.
4. Otherwise: stage and fsync the trial; write the state with `attempt_boot`=this boot,
   `attempt_reports`=fingerprint, `attempts_since_healthy`+1; write `last-boot`; remove a previous
   boot's empty `logs/capture.stop` and regular `capture.done` (best effort, no follow/recursion);
   print the path. A failed fsync never prints a path; a published `last-boot` is revoked
   (exit 3 if that revocation cannot be confirmed). A durable attempt record without a published
   path only makes the next judgement stricter.

Same-boot double selection is refused twice over (`last-boot`, `attempt_boot`); concurrent selectors
are serialized by the existing `flock`. An undurable trip record is re-derived from the same
evidence on the next boot (tested).

### Reset-report fingerprint (no clocks)

The SM config (`/jci/sm/sm.conf` `<reports export_dir_path="/data">`) writes `meminfo.out`,
`ps_info.out`, `free_info.out`, `top_info.out`, `thread_info.out`, `df_info.out`, `dmesg.out` just before
it stops the watchdog (2026-10-04 reset). The guard resolves `/data` through the stock aliases
(`/data → /mnt/data`, `/mnt → /tmp/mnt`; any other target declines), lists every `*.out` (≤ 64, else
decline), and hashes per entry: name, file type, size, inode, mtime as an opaque value, and SHA-256 of
the first 1 MiB; regular files are read with `O_NOFOLLOW|O_NONBLOCK`, symlinks are fingerprinted, not
followed. Only equality is compared; the CMU clock restarting at 1970 does not matter (a test rewrites
content with the same size and the same 1970 mtime). Evidence that normal boots do not rewrite the
reports: the private 2026-10-05 export, taken 857 s into the shadow.5 trial boot (no reset, about
898 s), still held reports with mtimes 1970-01-01 00:24:12–13 (1,452 s uptime) and a `dmesg.out`
ending at 1,451 s, i.e. from the 2026-10-04 reset, after that day's recovery boot, the shadow.5
installation and its menu-5 reboot. One observation, not a proof for every shutdown path.
`start_normal_mode.sh` writes under `/data` before the SM line, so `/data` is mounted when the guard runs.

### Healthy marker (reader only; rule OFF in this build)

`<logs>/healthy` = `boot_id=<uuid>\nuptime_s=<n>\n`, written by the runtime after 120 s of stable
runtime. The guard opens `logs` and the marker relative to the trusted installation directory with
`O_NOFOLLOW` (a symlinked `logs` or marker is ignored), requires a regular file ≤ 128 bytes, the exact
format (lowercase UUID, canonical decimal ≤ 10 digits, ≥ 120) and `boot_id` equal to the previous
attempt. Missing or invalid means "not healthy", never an error.

**Limitation (coordinator decision 2026-10-06):** the runtime marker writer is not implemented, so the
boot-loop rule is compiled out: `MX5DR_GUARD_HEALTHY_RULE` defaults to 0, the manifest says
`healthy_rule=off` and the status shows `(rule off)`. A guard built with the rule on rejects an `off`
manifest and vice versa. In this build failure detection is the SM reset-report rule only
(2 consecutive failed boots ⇒ trip) plus the runtime's own `disable-next-start`. A boot loop that does
not make the SM write reports (for example a hang before SM supervision, or a power-cut loop) is not
detected. Both rule settings are tested.

## Packaging

- `install.sh`: `MODE=BETA` → policy `persistent` (guard `enable`; no `arm`/`armed-boot`), unless
  `--one-boot`. OBSERVE/SCRUB/SHADOW stay one-boot. Every install first removes `arm` and `persist`;
  a failure after that removes a fresh `persist`. A persistent re-install clears a regular
  `disable-next-start` (the owner's acknowledgement; printed). `installed.txt` records the policy.
- `arm.sh` removes `persist` (one policy at a time); the guard also refuses to select or enable with both.
- `uninstall.sh` (menu 4) removes `arm`, `persist`, the owned autostart blocks and tokens; keeps
  `persist-state`, logs and reports as evidence.
- Menu: BETA shows `1 Install or re-enable BETA, every boot`; 5 starts the first product boot;
  2 shows the verdict; 3 exports; 4 removes. `trial_status.sh` prints `guard_policy`, `persist`,
  reason, fail count, attempts since healthy, healthy previous boot and whether this boot was
  selected. Verdict example (product boot, 40-column screen):

  ```
  ---- GO / NO-GO ----
  PERSIST enabled (every boot)
  fail count 0 of 2
  attempts since healthy 3 (rule off)
  healthy previous boot no
  this boot selected yes
  ok   BOOT  product this boot
  ok   GUARD committed
  ok   PERS  enabled
  ...
  ```

  Tripped: `PERSIST tripped (reset_reports)`, `NO   PERS  tripped reset_reports` and the final lines
  `NO-GO: tripped, stock runs, BETA off.` / `Find the cause, then run menu 1 again.`
- `startup_diagnostics.sh` (menu 3) records both persistent files line by line.

## Verification (2026-10-06)

Recorded with results in [PERSISTENT_GUARD_2026-10-06](PERSISTENT_GUARD_2026-10-06.md).

## Residual risks for a CMU that runs this guard on every boot

- Never executed on a CMU. A reset that leaves no new SM report (watchdog/kernel panic before SM
  supervision, power loss, a hang that SM does not handle) is invisible to the reset rule; the
  boot-loop rule that would cover it is off until the runtime writes `healthy`.
- Any SM report rewrite counts as our failure, including OEM resets unrelated to us, a manual
  report deletion, or other writers (the files are mode 666). That only makes the product trip
  earlier (fail-safe direction). A reset during a stock boot that follows an unjudged attempt
  (e.g. bindings invalid) is attributed to that attempt.
- Two consecutive failures are needed: the first failed boot is followed by one more product boot.
- `capture.stop` from menu 3 is cleared at the next product boot by design (always-on); a nonempty or
  odd marker is left and then blocks capture/BETA in that boot.
- `logs` is writable by the collector account; a forged `healthy` marker could only weaken the
  (currently disabled) boot-loop rule, never the reset rule or the root-owned state.
- The guard reads up to 64 × 1 MiB of reports before the SM starts (actual ≈ 0.3 MiB).
