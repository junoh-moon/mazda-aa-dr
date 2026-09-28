# 0.2 candidate integration verification — 2026-09-28

This record covers the integrated OFF loader, isolated collector and one-boot guard changes. No CMU, OEM executable, phone or navigation app was executed. Claude was not used; Astra agents implemented/reviewed independently and the root agent integrated and reran checks.

## Executed checks

| Check | Result and scope |
| --- | --- |
| Host `make test` | Pass: 14 recovery/editor tests; 24 actual-interposer cases (one Python harness); core 1,425 checks and replay; 11 adapter cases; config/SHA 27 checks; AA journal faults; collector journal/environment; 10 collector process/helper tests; 13 packaging tests; 20 analyzer tests; synthetic pipeline 8 sends |
| Packaging identities | Private matching stock files read as fixtures, including actual autostart. Built payload fixture present; no packaging skips in integrated run. No OEM executable run |
| Production ARM build | `make arm` with pinned GCC 4.9.1: `libmx5dr.so`, `mx5dr-collector`, `mx5dr-guard`; ARMv7 Cortex-A9 NEON softfp |
| ARM/QEMU suite | Core, config/SHA, adapter, real ARM veneer, journal, collector journal and pipeline passed via `tests/run_arm_all.sh` |
| ARM loader fault cases | Same 24 synthetic ELF interposer cases passed with the pinned compiler/sysroot in QEMU |
| ARM guard fixture suite | Same 14-test harness passed with the guard cross-compiled and run under QEMU. Shell/editor and deliberately crashing constructor/main fixtures remain authored **host** programs, not ARM/OEM crash tests |
| Binary dependencies | Preload has no D-Bus dependency or polling/spawn import. Three artifacts require only GLIBC_2.4 versions and no dynamic libstdc++; no test fixture/fault environment strings in production guard |
| Stock dependency audit | BLM/launcher combined static scope: 50 ELF objects, 3,641 strong import records, no unmatched name/version pairs. Runtime interposition and constructors remain untested; see separate report |

Independent review found and closed: loader-lock inversion, target NOLOAD pre/post-call exposure races, failed arm-publication fsync leaving authorization behind, and legacy permanent-preload migration ordering. Final recovery integration review found no additional blocker within the declared scope. Review is not proof of all possible failures.

## Reproducing the extra ARM guard suite

Use the pinned toolchain root as `MX5_TOOLCHAIN`. The explicit loader below avoids QEMU's `-L sysroot` path remapping changing the guard's `open("/")` / `openat` traversal. The initial remapped run failed with `openat(root, "tmp") = ENOENT`; no production trust check was relaxed.

```sh
GUARD_CXX="$MX5_TOOLCHAIN/bin/arm-cortexa9_neon-linux-gnueabi-g++" \
GUARD_ARCH_FLAGS='-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm' \
GUARD_RUNNER="qemu-arm -L / $MX5_TOOLCHAIN/arm-cortexa9_neon-linux-gnueabi/sysroot/lib/ld-linux.so.3 --library-path $MX5_TOOLCHAIN/arm-cortexa9_neon-linux-gnueabi/sysroot/lib:$MX5_TOOLCHAIN/arm-cortexa9_neon-linux-gnueabi/sysroot/usr/lib" \
python3 tests/recovery/test_guard.py
```

The test build alone enables fixture roots and fault injection. Production `make arm` does not define `MX5DR_GUARD_TESTING` or `MX5_COLLECTOR_TESTING`. The full collector process tests and its real cmu UID/GID transition were not run on a CMU. The guard's next-start boundary does not establish same-running-SM retry behavior.

## Result

The three reported implementation blockers now have code, regression coverage and a buildable first stationary OBSERVE trial package. Physical first boot, touch coexistence, collector bus permission, second-boot baseline return, phone location adoption and navigation accuracy remain unperformed checks. Live ASSIST remains disabled. Follow `docs/FIRST_TRIAL_KO.md`; historical 0.1 installation statements are superseded.
