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

## Revision G2 (2026-10-06, after two independent reviews)

The sections above describe the first persistent design (`persist-state` v1). Two reviews found an
unbounded loop and four smaller defects; this revision supersedes the state machine above where they
differ. The healthy-marker rule stays OFF; its replacement needs no runtime cooperation.

### Guard-owned confirmation, probation and caps

- **`confirm`.** After a selection the owned autostart block starts, only when `guard/persist`
  exists, `( trap '' HUP; /bin/sleep 90; exec .../mx5dr-guard confirm ) </dev/null >/dev/null 2>&1 &`.
  It is backgrounded inside the owned block that runs before the `taskset ... sm` launch anchor (not after it), and the 90 s `sleep` is inside the background subshell, so it never delays the SM. `/bin/sleep`
  is the stock BusyBox applet; `trap ''` ignores a hang-up when `autostart` exits. `confirm`
  writes `confirmed_boot=<this boot>` only if this boot is the open attempt, `last-boot` equals it
  (the selection was committed) and `/proc/*/cmdline` shows `/jci/sm/sm -f <that trial> ...`
  (exactly the vehicle `ps` line of 2026-10-04: `/jci/sm/sm -f /tmp/mx5dr-trial-jQHO1w/sm.conf -e
  /tmp/smevents.txt`). A second `confirm` is a no-op; a stale or different boot is rejected. The lock
  is retried for 5 s.
- **State v2** (`mx5dr-persist-state-v2`): adds `probation`, `unconfirmed_count`, `recent` (last 10
  outcomes), `attempt_trial`, `confirmed_boot`. v1 files decline (menu 1 rewrites them).
- **Judgement of the previous attempt:** `R` failed_reset (report fingerprint changed, even if it
  was confirmed earlier), `B` boot loop (healthy rule only), `C` confirmed, `U` unconfirmed (no
  report and no confirmation). Only `C` resets `fail_count` and `unconfirmed_count` and ends
  probation; `U` and `R` never reset either counter.
- **Trips** (first match): `probation` — the first attempt after menu 1 was not confirmed (restores
  the one-boot bound for deterministic faults such as the SM rejecting the trial config);
  `reset_reports` — 2 consecutive `R`; `reset_reports_repeated` — 3 `R` within the last 10 attempts
  (alternating fail/ok cannot hide); `unconfirmed` — 3 consecutive `U`; `boot_loop` (rule on only);
  `runtime_disabled`.
- **Bounds:** a fault that leaves no report and stops the SM or the CMU before the confirmation
  runs the product at most once after menu 1, or at most 3 times in a row later. Undelivered
  selections (attempt recorded, no path printed, e.g. a failed fsync) also count as `U`.

**Short drives.** A boot counts as confirmed only if the CMU is still up about 90 s after the SM
launch (autostart runs after the kernel and early scripts; roughly 115–130 s uptime by the 2026-10-05 boot row (uptime 42 s at the boot row, SM earlier),
not measured). Whether the CMU powers down immediately at ignition off is not known. Three such
boots in a row, or the first one after menu 1, trip to stock: the safe direction; menu 1 re-enables.
Example: three consecutive trips of under about two minutes (moving the car in a garage) trip;
one longer drive in between resets the count. The procedure asks the owner to keep the first boot
after menu 5 running until menu 2 shows `ok CONF confirmed`.

### Hang bound

The block calls `timeout -t 15 -s KILL guard select ...` when the stock `/usr/bin/timeout` applet is
executable (BusyBox 1.19.2 in this firmware, `-t SECS -s SIG` syntax, checked under the stock
BusyBox emulation), otherwise the direct call. The guard itself sets `alarm(10)` with SIGALRM's
default action on every command; every path before the printed trial path is fail-closed, so an
abort leaves at most an unpublished `/tmp` file or an attempt record that counts as `U`. The `/data`
scan stops at 4096 entries, the `/proc` scan of `confirm` at 8192.

### Stale temporaries

Under the guard lock every command removes `.{arm,last-boot,persist,persist-state,last-decision}.<1-10
digits>` that are owned regular files (never followed, scan bounded at 256); `atomic_file()` also
unlinks its temporary name before the `O_EXCL` create, so a reused PID cannot block a state write.

### Owner visibility

Every persistent decline writes the root-owned `guard/last-decision` (`boot_id`, `reason`, atomic,
best effort). `mx5dr-guard verify` (read-only, no lock) prints the reason a new boot would decline
and a one-line instruction (`bindings changed: sm.conf edited by another tool: run menu 1`).
`mx5dr-guard status` prints the state as key=value lines from the same parser that `select` uses;
`trial_status.sh` no longer parses the state itself. Menu 2 shows `PERSIST enabled but bindings
changed`, `NO BOOT stock this boot (<reason>)`, `this boot confirmed yes|pending` and a `CONF` row.
The car gives no notification: the procedure asks the owner to check menu 2 every few drives.

### Evidence on re-enable

Menu 1 first copies `persist-state`, `last-decision` and `disable-next-start` to
`backups/persist-evidence/<n>` (numbered, no clock, last 3 kept), prints the previous trip reason and,
when tripped, advises menu 3 first (not enforced).

### Remaining unbounded or unobservable cases

1. A guard blocked in uninterruptible sleep (e.g. hung flash I/O) ignores both SIGALRM and SIGKILL;
   the command substitution then waits and the SM does not start. Unbounded.
2. A failure after the confirmation that leaves no SM report (kernel panic, hardware watchdog, power
   loss after 90 s) is judged `C` every time: a late, report-free reset loop is not detected.
3. A product that runs but misbehaves without a crash (AA, touch, HUD or location wrong while the SM
   stays up) is confirmed; the guard cannot observe it. Only the owner can (menu 2, menu 4).
4. A failing service the SM restarts or ignores without a reset report is invisible.
5. Any report rewrite counts as ours (OEM resets, other writers; files are mode 666): trips early.
6. The 90 s timing, the CMU power-down behaviour and the SM argv on a CMU without our trial were not
   measured on this firmware beyond the one vehicle `ps` line.

### Review notes added with the publication of v1.0.0-beta.2 (2026-10-06)

- **Confirmation timing (M1).** `confirm` runs at about 115-130 s uptime, before the AA dongle connects (190 s in the 2026-10-05 trace) and before most product activity. A reset WITHOUT an SM report after that point (kernel panic, hardware watchdog, power cut) is judged `C` every time and never trips. A jciAAPA crash is covered because `reset_board=yes` writes `/data/*.out` (`R`, trips at 2 consecutive or 3 of 10). A second confirmation stage (for example a 600 s mark) is future work.
- **No HMI-free emergency stop (M2).** Menu 4 needs the HMI/USB entry. If the SM is alive but the HMI is broken, `C` keeps being judged and the product keeps running. The practical way out is the unconfirmed cap: turning the ignition off before the 90 s mark three times in a row gives `tripped=unconfirmed`. Whether the CMU really powers down at ignition off is NOT measured; do not treat this as verified.
- **Unconfirmed cap is not windowed (M3).** `C` resets the unconfirmed counter, so a defect that kills the CMU without a report before 90 s in two of three boots (pattern U,U,C) is never tripped. This is the price for not tripping on honest short trips.
- **Display (L6).** If the guard is KILLed after printing the trial path but before exiting, the CMU boots stock while status shows `selected_this_boot=yes`; the next boot is judged `U`.
- **Lock retry (L5).** `enable` does not retry the lock; menu 1 run in the instant the background `confirm` holds it fails once, run it again.
- **Evidence backup (L4).** The menu-1 evidence copy uses `cp -pP` so a collector-owned file swapped for a symlink is copied as a link, not followed.
