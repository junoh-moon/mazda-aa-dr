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
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_motion_batch.cpp -o "$build/motion-batch-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/motion-batch-test"
dbus="-I$QEMU_SYSROOT/usr/include/dbus-1.0 -I$QEMU_SYSROOT/usr/lib/dbus-1.0/include"
"${CROSS_COMPILE}gcc" -std=c99 $warn -pedantic $arch -Isrc/core src/core/dr_core.c tests/core/test_core.c -lm -o "$build/core-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/core-test"
"${CROSS_COMPILE}gcc" -std=c99 $warn -pedantic $arch -Isrc/core -c src/core/dr_core.c -o "$build/core.o"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/runtime/core_bridge.cpp tests/integration/test_pipeline.cpp src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S "$build/core.o" -lm -ldl -pthread -o "$build/pipeline-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/pipeline-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch src/runtime/config.cpp src/runtime/sha256.cpp tests/runtime/test_runtime.cpp -o "$build/runtime-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/runtime-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/loader.cpp src/navigation/pipeline.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp "$build/core.o" tests/runtime/test_journal.cpp -ldl -pthread -lrt -lm -o "$build/journal-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/journal-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch $dbus tests/collector/test_journal.cpp src/runtime/config.cpp -ldbus-1 -pthread -lrt -o "$build/collector-journal-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/collector-journal-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch tests/runtime/test_loader.cpp -ldl -pthread -o "$build/loader-test"
qemu-arm -L "$QEMU_SYSROOT" -E "LD_PRELOAD=$project/build/libmx5dr.so" "$build/loader-test"
sh tests/adapter/run_arm.sh

"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/navigation/pipeline.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp tests/navigation/test_navigation.cpp src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S "$build/core.o" -lm -ldl -pthread -o "$build/navigation-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/navigation-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/sensors/test_vim_source.cpp src/sensors/vim_source.cpp -o "$build/vim-parser-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/vim-parser-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/sensors/test_vim_tap.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp src/runtime/sha256.cpp src/runtime/config.cpp -ldl -pthread -lrt -o "$build/vim-tap-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/vim-tap-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/navigation/test_channel.cpp src/navigation/channel.cpp -o "$build/motion-channel-test"
result=0
qemu-arm -L "$QEMU_SYSROOT" "$build/motion-channel-test" || result=$?
[ "$result" -eq 0 ] || [ "$result" -eq 77 ]

"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/navigation/test_live_pipeline.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp src/navigation/pipeline.cpp src/runtime/core_bridge.cpp src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S "$build/core.o" -lm -ldl -pthread -o "$build/live-pipeline-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/live-pipeline-test"
