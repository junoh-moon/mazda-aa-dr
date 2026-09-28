> **2026-09-28: installation on hold.** Historical 0.1 analysis/instructions follow. [Current status](STATUS_KO.md) and [review corrections](REVIEW_2026-09-28_KO.md) supersede installation GO statements. No vehicle or phone validation has been performed.

# Validation record — experimental 0.1

Date: 2026-09-27. No vehicle or phone was connected to this development host.
All executed ARM programs were our synthetic test programs, not OEM firmware.
The historical built library is omitted from this source repository. Its identity
is recorded in `validation/build-info.json`; the historical bundle path was
`bundle/libmx5dr.so`.

## Build

- Pinned lmagder/m3-toolchain commit: `61ec0343de84f6fc7c46840056df1d600d44be8a`.
- Compiler: GCC 4.9.1, C99 core and C++11 runtime.
- ARMv7 Cortex-A9, NEON, **softfp**, ARM instruction mode.
- Warnings are errors. No exceptions, RTTI, or dynamic dependency on libstdc++.
- Final link requires all strong symbols to resolve.
- ELF32 little-endian ARM EABI5; non-executable stack; RELRO and immediate binding.
- No TEXTREL. Required GLIBC symbol versions are exclusively **GLIBC_2.4**.
- Dynamic libraries: libdbus-1.so.3, libdl.so.2, libpthread.so.0, librt.so.1,
  libm.so.6 and libc.so.6. The toolchain has glibc 2.11 and DBus 1.6.30.

These checks reduce ABI risk; they do not demonstrate the actual CMU loader or
all libraries on the user's vehicle. See `toolchain.md` for a pinned download and
rebuild route. `validation/elf.txt` contains the final ELF inspection.

## Executed checks

| Check | Host | ARM QEMU with pinned toolchain |
| --- | --- | --- |
| DR core: geometry, reverse, quality, chronology, limit and reacquisition | 1,425 checks passed | 1,425 checks passed |
| Normalized CSV replay: determinism, stale/reacquire, sentinel and six malformed cases | Passed | Core is tested directly; CSV CLI not separately run |
| Adapter forwarding/scrub/assist-gate/error paths | 11 cases passed | 11 cases passed |
| Real assembly veneer, synthetic original function | Not applicable | Arguments, r0–r3 returns, nested TLS, exact-once calls and errno passed |
| Core → bridge → fake OEM | Eight exactly-once sends and rejection cases passed | Same passed |
| Runtime configuration and SHA-256 vectors | 27 checks passed | Same passed |
| Journal and queue failures | Rotation, bounded sizes, open/flush/rotation failures and queue-loss disable passed | Same passed |
| Child environment | Keeps library/JCI environment, removes preload/audit, bounds entries | Same passed |
| Final shared library preload smoke | Not counted as a release gate | 400 concurrent non-target loads, null-path and NOLOAD passed |
| Installation, removal and export fixtures | 11 cases passed using provided stock hashes and release payload | CMU BusyBox not executed |
| PC log analyzer | 18 cases passed | PC-only program |

`validation/host-tests.txt` and `validation/arm-tests.txt` retain execution output.
The loader smoke runs the **release library**, but deliberately does not execute
an OEM module. It establishes library loading and non-target interposition only.
The synthetic ARM veneer test exercises the real assembly; it does not install
the inline patch into Mazda's actual process.

The core and adapter authors also ran host ASan/UBSan successfully. LeakSanitizer
was disabled because the execution environment cannot inspect process task
directories. The core does not allocate. These are supplementary author checks;
the reproducible primary release records are the strict host and ARM runs above.

## Independent reviews and corrections

Astra agents independently covered core mathematics/state, ABI/hook logic,
installation/recovery and runtime review. Corrections made before this release:

- Start SCRUB only after the boot journal has opened and flushed successfully.
- Latch any journal flush/close/rotation failure and switch to OBSERVE immediately.
- Disable mutation after observation queue contention/overflow loses a record.
- Use a worker-local C numeric locale for JSON; never alter the OEM process locale.
- Retry interrupted waitpid; bound/reap spawned readers.
- Disable D-Bus auto-start on getter messages.
- Preserve SMDB child library/JCI environment while removing LD_PRELOAD/LD_AUDIT.
- Handle failure to restore executable permissions without returning into an NX page.
- Use C99 `::round` and explicit value initialization for the GCC 4.9 library configuration.

The pre-release review reported no additional blocker for experimental
OBSERVE/SCRUB. **That conclusion was superseded on 2026-09-28:** the loader and
crash-recovery defects block vehicle installation, including OBSERVE. That earlier
review never approved live ASSIST.
Claude was not used in this implementation; no callable Claude route was available.

## Explicitly unverified

- The actual car's module load order and the installed touch shim combination.
- Cold-start inline/GOT installation in the CMU, and failure recovery on that CMU.
- Actual BusyBox variants, storage behavior under power loss, and root access method.
- Sensor presence, production timestamp, source quality, latency and calibration.
- NNG activation and native DR with/without the user's SD and map coverage.
- Galaxy S25/wireless-dongle AA receipt, location fusion, and map-app adoption.
- Tunnel position/velocity accuracy or a long-tunnel solution.

The payload defaults to OBSERVE and has live ASSIST disabled in code and config.
SCRUB is a limited comparative experiment. A local log-analyzer PASS verifies
only recorded byte invariants; it is not proof of complete logging, phone receipt,
navigation correctness or successful DR.
