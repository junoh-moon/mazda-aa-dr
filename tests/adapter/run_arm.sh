#!/bin/sh
# Usage: CROSS_COMPILE=/absolute/toolchain/bin/arm-...- QEMU_SYSROOT=/absolute/sysroot ./tests/adapter/run_arm.sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cross=${CROSS_COMPILE:-arm-linux-gnueabi-}
sysroot=${QEMU_SYSROOT:-/usr/arm-linux-gnueabi}
build=${ADAPTER_ARM_BUILD:-$project/build/adapter-arm-tests}
mkdir -p "$build"
cd "$project"
# The production hook requires softfp; hard-float rejects installation by design.
flags='-std=c++11 -O2 -Wall -Wextra -Werror -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm -pthread'
"${cross}g++" $flags -fPIC -fvisibility=hidden -I src -shared \
    src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S \
    -Wl,-z,defs -ldl -o "$build/libadapter.so"
"${cross}readelf" -d "$build/libadapter.so" > "$build/dynamic.txt"
if grep -q TEXTREL "$build/dynamic.txt"; then
    echo 'ERROR: adapter has dynamic text relocations' >&2; exit 1
fi
"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S \
    tests/adapter/adapter_test.cpp -ldl -o "$build/adapter-test"
"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S \
    tests/adapter/veneer_arm_test.cpp tests/adapter/veneer_arm_fixture.S \
    -ldl -o "$build/veneer-test"
for test in observe scrub native malformed nested assist epoch reacquire expiry encoder backend request; do
    qemu-arm -L "$sysroot" "$build/adapter-test" "$test"
done
qemu-arm -L "$sysroot" "$build/veneer-test"
