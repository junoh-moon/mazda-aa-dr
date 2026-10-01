#!/bin/sh
set -eu
# Instrument one test-only TU, never production build flags or objects.
build=${MX5DR_JOURNAL_BOUNDARY_BUILD:-build/journal-boundaries}
cxx=${CXX:-c++}
cc=${CC:-cc}
arch=
veneer=
if [ -n "${CROSS_COMPILE:-}" ]; then
    cxx=${CROSS_COMPILE}g++
    cc=${CROSS_COMPILE}gcc
    arch='-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm'
    veneer='src/adapter/arm_veneer.S src/adapter/request_veneer.S'
    : "${QEMU_SYSROOT:?Set the pinned ARM sysroot}"
fi
if ! "$cxx" --help=optimizers 2>/dev/null | grep -q -- '-finline-atomics'; then
    echo 'SKIP journal boundary scheduler: GCC out-of-line atomics required (Clang is unsupported)'
    exit 77
fi
atomic=$("$cxx" $arch -print-file-name=libatomic.a)
test -f "$atomic" || { echo 'Static test-only libatomic.a is missing'; exit 1; }
mkdir -p "$build"
"$cc" -std=c99 -O2 -Wall -Wextra -Werror $arch -c src/core/dr_core.c -o "$build/core.o"
"$cxx" -std=c++11 -O2 -Wall -Wextra -Werror $arch -Isrc -fno-inline-atomics \
    -c tests/runtime/journal_boundaries_api.cpp -o "$build/api.o"
"$cxx" -std=c++11 -O2 -Wall -Wextra -Werror $arch -Isrc \
    tests/runtime/test_journal_boundaries.cpp "$build/api.o" "$build/core.o" \
    src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/request_hooks.cpp \
    src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp $veneer \
    src/runtime/request_trace.cpp src/runtime/request_observer.cpp src/runtime/config.cpp \
    src/runtime/sha256.cpp src/runtime/loader.cpp src/runtime/core_bridge.cpp src/runtime/assist_worker.cpp \
    src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp \
    -Wl,--wrap=__atomic_fetch_add_8 -Wl,--wrap=__atomic_fetch_sub_8 \
    -Wl,--wrap=__atomic_store_1 "$atomic" -pthread -ldl -lrt -lm \
    -o "$build/test"
if [ -n "${CROSS_COMPILE:-}" ]; then
    qemu-arm -L "$QEMU_SYSROOT" "$build/test"
else
    "$build/test"
fi
