# Separate observation collector — 2026-09-28

This change addresses R2 in `REVIEW_2026-09-28_KO.md`. It does not approve vehicle installation or resolve the separate preload recovery and loader blockers. Historical 0.1 test counts remain historical.

`libmx5dr.so` now contains the hook, bounded observation queue and trace writer, with no D-Bus dependency, bus calls, SMDB reads, child spawning, or collector startup. A standalone `mx5dr-collector` performs read-only SMDB and D-Bus polling in a separate address space. It initializes D-Bus threading before opening a connection. There is no shared-memory/IPC interface from the collector to the adapter, and no live ASSIST input is produced.

## Starting and stopping

The bundle keeps the existing two-argument builder interface. `make arm` builds the library and `mx5dr-collector` in the same output directory; `packaging/make_bundle.sh build/libmx5dr.so NEW_DIRECTORY` requires both siblings. The installer validates each SHA-256 and ARM ELF header before staging. It copies the collector and start/stop scripts to fixed local paths so removable media can be detached for AA. It does not start either AA or the collector and does not add a guessed service to `sm.conf`.

From an already authorized **root** shell **while parked**, run:

```sh
sh /data_persist/mx5-aa-dr/tools/start_collector.sh
# After the session, while parked:
sh /data_persist/mx5-aa-dr/tools/stop_collector.sh
```

The optional start argument is a session limit in seconds (1–86400, default 28800/eight hours). Start is asynchronous: its success means a launch was requested, not that collector initialization succeeded. Confirm `collector_boot` in the logs while parked. The process then collects automatically without driving-time interaction. A verified external startup guard may invoke the same root-owned helper once after selecting an authorized trial; that integration belongs to the recovery change. This helper itself does not add reboot persistence or recover a failing AA preload.

The helper removes `LD_PRELOAD` and `LD_AUDIT` before executing the collector, preserving other caller environment. In production, a root-launched collector drops supplementary groups, primary GID and UID to the `cmu` account before config/log/DBus access. It fails if this cannot be done. The packaging helpers require root; they are not direct cmu-shell entrypoints. The executable itself also supports a non-root invocation only when that user owns the log directory, for a separately verified startup path. The collector's actual CMU library environment and D-Bus authorization still need stationary verification; copying the existing AA service environment is not automatically established by this helper. Already preloaded code in the shell that invokes the helper cannot be undone by `unset`.

A kernel `flock` on `logs/collector.lock` enforces one collector. It is released on process death; neither a PID file nor a stale lock pathname authorizes killing a process. `collector.pid` is diagnostic only. Explicit startup clears a stale stop marker after acquiring the singleton lock. The stop script creates `logs/collector.stop` as an atomic directory marker. The collector checks it between polls and during sleep. Setting config to OFF/invalid also ends collection at the next cycle. SIGTERM/SIGINT request graceful exit; normal exits write `collector_stop` and remove the diagnostic PID file.

Stop is cooperative. D-Bus registration, kernel work, or a stuck process can delay it; there is no hard termination deadline. The session limit is checked between cycles and can also overrun while a call is blocked. The scripts never kill/restart AA, never kill a recorded PID, and never try to fix an unresponsive collector by repeatedly spawning replacements. Recovery after a truly hung collector is an explicit parked maintenance action, outside this script. No CMU stop/start behavior was vehicle-tested here.

## Observation and storage contract

- Collector files are `logs/collector.0.jsonl` (newest), `.1.jsonl` (older). They use an independent cap of `min(config.max_log_bytes, 1 MiB)` per file and `min(config.max_log_files, 2)` files. AA `trace.N.jsonl` limits remain unchanged. At default settings, maximum retained JSONL capacity is 24 MiB AA + 2 MiB collector, excluding small control files. Bounded retained size does not bound total flash writes.
- Every collector record has `stream="collector"`, `collector_pid`, `observed_at_mono_ns`, `producer_mono_ns=null`, and `producer_time_status="unknown"`. Observation time is when the collector records the result; the SMDB batch also retains begin/end timestamps. Three serial polls are not an atomic sensor snapshot. `receipt_ns` in D-Bus records is likewise receipt time. The producer's original UTC value in `position_poll.utc_s` is recorded without claiming freshness or clock validity.
- Correlation with AA `mono_ns` is only meaningful on the same CMU kernel boot. Both AA `boot` and `collector_boot` now record validated `/proc/sys/kernel/random/boot_id` (or explicit `unknown`). The analyzer retains matching kernel boot IDs as correlation evidence only. Neither stream proves a common exact sensor request, source epoch, or producer measurement time; independently rotated files may span boots. A missing/unknown boot ID cannot be replaced by matching PID or monotonic values.
- Owner PID/comm, receiver selection, SMDB values and native position are observations. `freshness="unproven_poll"`, `quality="unknown"`, `request_provenance=false` and `assist_ready=false` remain explicit. Poll success is not a VALID/fresh measurement and cannot qualify live DR.
- SMDB child output is bounded; the ordinary wait polls for 250 ms, sends SIGKILL on timeout and polls for reaping for up to another 100 ms. Spawn/scheduling/kernel work have no claimed hard time bound. An unreaped child stops further spawning and ends the collector; no unbounded child accumulation is attempted. Child environment removes preload/audit variables while preserving other service-specific variables.
- Collector write/open/rotation errors stop this collector. They do not change adapter mode or occupy its ring. The processes still share system CPU, flash and kernel resources: this separation does not prove absence of shared-resource contention or disk-full effects on AA.

The existing export script includes both streams under `logs/`. The PC analyzer keeps collector records out of AA call/generation/boot/health correlation and reports them under `collector`. A collector-only export remains inconclusive for AA invariants (`no_location_samples`), not a fabricated AA session missing boot/health. Missing collector start/stop boundaries are reported as incomplete observations. A valid collector record cannot claim an AA hook event or producer measurement timestamp.

## Verification of this change

Host `make test` passed on this change: core 1425 checks, deterministic replay, 11 adapter cases, config/SHA 27 checks, AA journal failures/rotation, independent collector journal/environment checks, ten collector process/helper tests, 20 analyzer tests, synthetic bridge pipeline, and packaging tests (11 passed; one built-release fixture test skipped because the release bundle had not yet been built). Test fixtures run no OEM executable and establish no phone receipt or navigation accuracy.

The collector process tests cover unavailable D-Bus, missing SMDB, invalid/OFF config, singleton/restart, cooperative stop, live OFF stop, SIGSTOP/SIGKILL isolation from independently executed synthetic AA send/journal checks, SMDB timeout, symlink rejection, binary imports, collector-only analyzer handling, and parked helper launch/environment/stop contracts. The synthetic independence check demonstrates distinct failure domains on the host; it is not a measurement of OEM throughput or target scheduling under load. Production root→cmu account transition requires the target account setup and was not exercised by the test-only root/path override build.

Integration reran `make test`, built the production library and collector with pinned GCC 4.9.1, and passed `tests/run_arm_all.sh` in QEMU, including the separate collector journal/environment fixture. ELF inspection confirms the preload has no D-Bus dependency or polling/spawn import. The ten full collector process/helper tests were host tests, not target execution. These checks do not resolve physical-car startup, permissions, source quality, or app-adoption gates.
