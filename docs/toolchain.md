# Reproducible Mazda ARM build

The release build uses [lmagder/m3-toolchain](https://github.com/lmagder/m3-toolchain)
at commit `61ec0343de84f6fc7c46840056df1d600d44be8a`: GCC 4.9.1 from crosstool-NG
1.20.0, ARMv7 Cortex-A9 NEON with the softfp calling convention. Its sysroot
includes the old glibc and DBus 1.6.30 headers/libraries. A current host ARM
compiler is useful for additional checks but is not the release compiler.

The ARM build also creates `mx5dr-sha256`, a statically linked file-hash helper
for the CMU's missing sha256sum applet. It uses the existing streaming SHA-256
implementation. USB packaging and installed recovery/export helpers carry it;
the target needs neither a compiler nor an extra software installation.

From the project directory:

```sh
python3 tools/fetch_m3_toolchain.py --jobs 16
MX5_TOOLCHAIN=$(pwd)/tools/m3-toolchain
python3 tools/build_arm.py --toolchain "$MX5_TOOLCHAIN" \
  --build-dir build/release-arm
```

Choose a new build directory each time. The release helper verifies the pinned
toolchain files, compiles all five artifacts without cached objects, checks ARM
ELF/ABI dependencies, and records source and artifact hashes in `arm-build.json`.
`make_usb_zip.py` requires that record and rejects changed inputs or mixed
artifacts. The record is an integrity check, not a signature or vehicle test.
For incremental development, direct `make arm ARM_PREFIX=... ARM_SYSROOT=...`
is still supported; compiler-generated `.d` files track transitive headers.

The optional positional argument selects a different download directory. The
script requires Python 3 and network access to GitHub; it requires no root
privileges. It defaults to 16 parallel downloads (`--jobs 1` through `64`).
The destination must be case-sensitive: Linux kernel headers contain distinct
upper/lowercase names. On macOS, use storage inside the Linux container rather
than a bind mount of a default macOS volume. The fetcher rejects that filesystem
before downloading compiler files, instead of silently overwriting headers.
Rerunning verifies existing files and resumes incomplete downloads. It downloads
the compiler, C/C++ development/runtime subset and DBus; unused Boost headers and
locales are omitted. Every file and symlink is verified against the pinned
upstream Git blob identity. `SOURCE_COMMIT`, `source_tree.json` and
`SUBSET_MANIFEST.json` record provenance locally. The toolchain is not included
in the project archive.

Compiler helpers must keep their upstream executable modes. In particular,
`libexec/gcc/arm-cortexa9_neon-linux-gnueabi/4.9.1/cc1plus` lacking its execute bit
can make the GCC driver report `error trying to exec 'cc1plus': ... No such file
or directory`, despite the file being present. The fetch script restores modes
on every run, including cached files. Rerun it after copying/extracting the
compiler through a process that loses executable modes. No compiler binary
patch or alternate host dynamic loader is required on this x86-64 Linux host.

The old libstdc++ configuration does not expose `std::round`; adapter code uses
the declared C99 `::round`. Aggregate value initialization uses `Type()` to avoid
GCC 4.9's `-Wmissing-field-initializers` diagnostics for `Type value = {}`.
`-Wall -Wextra -Werror` remains enabled.

For the isolated adapter and actual ARM veneer tests, install qemu-user and run:

```sh
CROSS_COMPILE="$MX5_TOOLCHAIN/bin/arm-cortexa9_neon-linux-gnueabi-" \
QEMU_SYSROOT="$MX5_TOOLCHAIN/arm-cortexa9_neon-linux-gnueabi/sysroot" \
  tests/adapter/run_arm.sh
```

All 11 functional cases and the synthetic nested ARM veneer fixture passed with
this pinned compiler and sysroot. The fixture checks argument and return
registers, exact-once original calls, TLS nesting, scrub selection and errno; it
does not execute OEM code. The adapter shared object had no TEXTREL and required
only GLIBC_2.4 symbol versions. This does not replace exact-OEM stationary loader
validation.

### Timing-class ARM tests (2026-10-10)

`tests/run_arm_all.sh` runs the tests that assert real product timing
contracts against the wall clock (`journal-test --writer`,
`runtime-lds-association-test`, `worker-session-test`, `worker-lds-test`,
`worker-lds-source-test` and the product-DSO `runtime-assist` suite) through `tests/run_arm_timing.sh`: at most 3
attempts, 5 s apart, passed if any attempt passes, failed if all fail. No
threshold changes; every other ARM test runs once. Each attempt prints
`ARM_TIMING_ATTEMPT name=... attempt=k/3 exit=... load=... assertion=...`
after its full output, and the run ends with a flaky report block plus
`ARM_TIMING_RETRIES=n` (tests that passed only after a retry) and
`ARM_TIMING_FAILED=n`. A release states `ARM_TIMING_RETRIES` in its notes
(`docs/RELEASING_KO.md`). Under QEMU the journal writer needs a real share of
a host CPU; with every core busy at nice 0 and the suite at nice 10 the
throughput tests fail on every attempt, which is reported, not retried away.
See `validation/ARM_TIMING_STABILISATION_2026-10-10.md`.
