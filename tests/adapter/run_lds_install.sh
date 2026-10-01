#!/bin/sh
# Developer PC only: exact private OEM dependencies, never a vehicle command.
# CROSS_COMPILE/QEMU_SYSROOT select the pinned build toolchain. Execution uses
# the separate MX5DR_LDS_STOCK root, including its original shared runtime.
set -eu
if [ -z "${MX5DR_LDS_STOCK:-}" ] || [ ! -d "$MX5DR_LDS_STOCK" ]; then
    echo 'SKIP LDS cold installer: set MX5DR_LDS_STOCK to the private original root' >&2
    exit 77
fi
: "${CROSS_COMPILE:?Set the CMU-compatible compiler prefix}"
: "${QEMU_SYSROOT:?Set the pinned compiler sysroot}"
[ -d "$QEMU_SYSROOT" ] || { echo 'ERROR: compiler sysroot is missing' >&2; exit 1; }
project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
stock=$(CDPATH= cd -- "$MX5DR_LDS_STOCK" && pwd)
qemu=${QEMU_ARM:-qemu-arm}
for owner in jci/lds/svcjcilds.so jci/lib/libjcilds-dbus.so \
             jci/lib/libjcilds-driver.so jci/lib/libjcidbus.so \
             jci/lib/libjcicommon.so usr/lib/libdbus-1.so.3 lib/ld-linux.so.3; do
    [ -r "$stock/$owner" ] || {
        echo "SKIP LDS cold installer: incomplete private runtime ($owner)" >&2
        exit 77
    }
done
command -v "${CROSS_COMPILE}g++" >/dev/null
command -v "$qemu" >/dev/null
command -v sha256sum >/dev/null
unset MAKEFLAGS GNUMAKEFLAGS MFLAGS MAKEOVERRIDES MAKEFILES MAKELEVEL \
    GCC_EXEC_PREFIX COMPILER_PATH LIBRARY_PATH CPATH C_INCLUDE_PATH \
    CPLUS_INCLUDE_PATH DEPENDENCIES_OUTPUT SUNPRO_DEPENDENCIES LD_RUN_PATH \
    LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT QEMU_SET_ENV QEMU_UNSET_ENV QEMU_LD_PREFIX
LC_ALL=C
export LC_ALL
base=${LDS_INSTALL_BUILD:-$project/build/lds-install-arm-tests}
mkdir -p "$base"
output=$(mktemp -d "$base/run.XXXXXX")
output=$(CDPATH= cd -- "$output" && pwd)
cd "$project"

original_hashes() {
    (cd "$stock" && sha256sum \
        jci/lds/svcjcilds.so jci/lib/libjcilds-dbus.so \
        jci/lib/libjcilds-driver.so jci/lib/libjcidbus.so \
        jci/lib/libjcicommon.so usr/lib/libdbus-1.so.3)
}
source_hashes() {
    sha256sum tests/adapter/run_lds_install.sh tests/adapter/lds_install_test.cpp \
        src/adapter/lds_install.cpp src/adapter/lds_hooks.cpp src/adapter/bus_hooks.cpp \
        src/adapter/adapter.cpp src/sensors/lds_lineage.cpp src/runtime/lds_sideband.cpp \
        src/runtime/request_trace.cpp src/runtime/sha256.cpp \
        src/adapter/*.h src/runtime/*.h src/sensors/*.h
}
run_logged() {
    label=$1
    shift
    # NUL-separated argv preserves spaces without generating shell code.
    printf '%s\000' "$@" > "$output/$label.argv"
    if "$@" > "$output/$label.log" 2>&1; then result=0; else result=$?; fi
    printf '%s\n' "$result" > "$output/$label.exit"
    return "$result"
}
original_hashes > "$output/original-before.sha256"
source_hashes > "$output/source-before.sha256"
total=0
failed=0
finish() {
    status=$?
    trap - 0
    originals_same=no
    source_same=no
    if original_hashes > "$output/original-after.sha256" && \
       cmp -s "$output/original-before.sha256" "$output/original-after.sha256"; then
        originals_same=yes
    else status=1
    fi
    if source_hashes > "$output/source-after.sha256" && \
       cmp -s "$output/source-before.sha256" "$output/source-after.sha256"; then
        source_same=yes
    else status=1
    fi
    {
        printf 'arm_cases=%s\nfailed=%s\nexit=%s\n' "$total" "$failed" "$status"
        printf 'original_files_unchanged=%s\nsource_unchanged=%s\n' "$originals_same" "$source_same"
        printf 'mprotect_injection=linker_wrap\nnormal_service_init_executed=no\n'
        printf 'driver_open_executed=no\nproduct_dso_executed=no\n'
    } > "$output/summary.txt"
    printf 'LDS_INSTALL_RECORD=%s\n' "$output"
    exit "$status"
}
trap finish 0
"${CROSS_COMPILE}g++" --version > "$output/compiler.txt"
"$qemu" --version > "$output/qemu.txt"
printf 'stock=%s\ncompiler_sysroot=%s\n' "$stock" "$QEMU_SYSROOT" > "$output/paths.txt"

# The real adapter state supplies bus invalidation. Its AA-only ARM entry is a
# separate translation unit and is intentionally absent from this LDS fixture.
# No --gc-sections or function/data sections are needed with the pinned linker.
# Only failure injection wraps mprotect; normal calls use the real syscall.
if ! run_logged build "${CROSS_COMPILE}g++" --sysroot="$QEMU_SYSROOT" \
    -std=c++11 -O2 -Wall -Wextra -Werror -fexceptions \
    -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp \
    -DMX5DR_LDS_WRAP_MPROTECT -Isrc \
    tests/adapter/lds_install_test.cpp src/adapter/lds_install.cpp \
    src/adapter/lds_hooks.cpp src/sensors/lds_lineage.cpp \
    src/runtime/lds_sideband.cpp src/adapter/bus_hooks.cpp \
    src/adapter/adapter.cpp src/runtime/request_trace.cpp src/runtime/sha256.cpp \
    -Wl,--wrap=mprotect -ldl -pthread -lrt -lm -o "$output/lds-install-test"; then
    echo "ERROR: LDS fixture build failed; see $output/build.log" >&2
    exit 1
fi
sha256sum "$output/lds-install-test" > "$output/fixture.sha256"
scenarios='positive-local positive-global retain-local retain-global repeat
invalid-0 invalid-1 invalid-2 invalid-3 invalid-4 invalid-5 invalid-6
hash-0 hash-1 hash-2 hash-3 hash-4 hash-5
chain-0 chain-1 chain-2 chain-3 chain-4 chain-5 chain-6 chain-7 chain-8 chain-9
chain-10 chain-11 chain-12 chain-13 chain-14 chain-15 chain-16 chain-17 chain-18 chain-19
chain-20 chain-21 chain-22 chain-23 chain-24 denied-lease
registration-0 registration-1 registration-2 registration-3 registration-4
registration-5 registration-6 registration-7 registration-8 registration-9
unload-baseline descriptor-0 descriptor-1 descriptor-2 descriptor-3
cache-literal-0 cache-literal-1 cache-literal-2 cache-literal-3
function-prefix caller-word register-word protect-0 protect-1 protect-2
lease-chain-change preoccupied-bus'
printf '%s\n' $scenarios > "$output/scenarios.txt"
for scenario in $scenarios; do
    total=$((total+1))
    if run_logged "$scenario" "$qemu" -L "$stock" \
        -E "LD_LIBRARY_PATH=$stock/jci/lib:$stock/usr/lib:$stock/lib" \
        -E LD_BIND_NOW=1 -E "MX5DR_LDS_STOCK=$stock" \
        "$output/lds-install-test" "$scenario"; then
        printf 'PASS LDS install %s\n' "$scenario"
    else
        failed=$((failed+1))
        printf 'FAIL LDS install %s (see %s)\n' "$scenario" "$output/$scenario.log" >&2
    fi
done
[ "$total" -eq 71 ] && [ "$failed" -eq 0 ]
