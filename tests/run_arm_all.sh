#!/bin/sh
# Developer PC only. Runs generated synthetic programs in qemu, no OEM binary.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
: "${CROSS_COMPILE:?Set the CMU-compatible compiler prefix}"
: "${QEMU_SYSROOT:?Set its sysroot}"
cd "$project"
build=$project/build/arm-full-tests
mkdir -p "$build"
arch='-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm'
warn='-O2 -Wall -Wextra -Werror'
dbus="-I$QEMU_SYSROOT/usr/include/dbus-1.0 -I$QEMU_SYSROOT/usr/lib/dbus-1.0/include"
"${CROSS_COMPILE}gcc" -std=c99 $warn -pedantic $arch -Isrc/core src/core/dr_core.c tests/core/test_core.c -lm -o "$build/core-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/core-test"
"${CROSS_COMPILE}gcc" -std=c99 $warn -pedantic $arch -Isrc/core -c src/core/dr_core.c -o "$build/core.o"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/runtime/core_bridge.cpp tests/integration/test_pipeline.cpp src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S "$build/core.o" -lm -ldl -pthread -o "$build/pipeline-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/pipeline-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch src/runtime/config.cpp src/runtime/sha256.cpp tests/runtime/test_runtime.cpp -o "$build/runtime-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/runtime-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc $dbus src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/runtime/config.cpp src/runtime/sha256.cpp tests/runtime/test_journal.cpp -ldbus-1 -ldl -pthread -lrt -lm -o "$build/journal-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/journal-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch tests/runtime/test_loader.cpp -ldl -pthread -o "$build/loader-test"
qemu-arm -L "$QEMU_SYSROOT" -E "LD_PRELOAD=$project/build/libmx5dr.so" "$build/loader-test"
sh tests/adapter/run_arm.sh
