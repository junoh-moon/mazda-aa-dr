#!/bin/sh
set -eu
# GCC-only scheduling instrumentation. This never changes production flags.
build=${MX5DR_PUBLICATION_BUILD:-build/request-publication}
cxx=${CXX:-c++}
arch=
if [ -n "${CROSS_COMPILE:-}" ]; then
    cxx=${CROSS_COMPILE}g++
    arch='-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm'
    : "${QEMU_SYSROOT:?Set the pinned ARM sysroot}"
fi
if ! "$cxx" --help=optimizers 2>/dev/null | grep -q -- '-finline-atomics'; then
    echo 'SKIP request publication scheduler: GCC out-of-line atomics required (Clang is unsupported)'
    exit 77
fi
atomic=$("$cxx" $arch -print-file-name=libatomic.a)
test -f "$atomic" || { echo 'Static test-only libatomic.a is missing'; exit 1; }
mkdir -p "$build"
"$cxx" -std=c++11 -O2 -Wall -Wextra -Werror $arch -Isrc -fno-inline-atomics \
    -c src/runtime/request_trace.cpp -o "$build/request-trace.o"
"$cxx" -std=c++11 -O2 -Wall -Wextra -Werror $arch -Isrc \
    tests/runtime/test_request_publication.cpp "$build/request-trace.o" \
    -Wl,--wrap=__atomic_exchange_4 "$atomic" -pthread -o "$build/test"
if [ -n "${CROSS_COMPILE:-}" ]; then
    qemu-arm -L "$QEMU_SYSROOT" "$build/test"
else
    "$build/test"
fi
