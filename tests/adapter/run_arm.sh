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
    src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    -Wl,-z,defs -ldl -o "$build/libadapter.so"
"${cross}readelf" -d "$build/libadapter.so" > "$build/dynamic.txt"
if grep -q TEXTREL "$build/dynamic.txt"; then
    echo 'ERROR: adapter has dynamic text relocations' >&2; exit 1
fi
"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    tests/adapter/adapter_test.cpp -ldl -o "$build/adapter-test"
"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    tests/adapter/veneer_arm_test.cpp tests/adapter/veneer_arm_fixture.S \
    -ldl -o "$build/veneer-test"
for test in observe scrub native malformed nested assist epoch reacquire expiry encoder backend request; do
    qemu-arm -L "$sysroot" "$build/adapter-test" "$test"
done
qemu-arm -L "$sysroot" "$build/veneer-test"
"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    tests/adapter/veneer_unwind_test.cpp tests/adapter/veneer_unwind_fixture.S \
    -ldl -o "$build/veneer-unwind-test"
for case in throw_position throw_send nested_throw cancel_position cancel_send throw_enter cancel_enter nested_send; do
    qemu-arm -L "$sysroot" "$build/veneer-unwind-test" "$case"
done

"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S \
    src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S \
    src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    tests/adapter/request_hooks_arm_test.cpp tests/adapter/request_hooks_arm_fixture.S \
    -ldl -o "$build/request-hooks-test"
for case in normal unrelated failed_submit destroy_queued throw_notify throw_work cancel_notify cancel_work malformed other_worker delayed_callback throw_getter cancel_getter session_transition; do
    qemu-arm -L "$sysroot" "$build/request-hooks-test" "$case"
done
"${cross}g++" $flags -I src tests/adapter/cold_patch_test.cpp -o "$build/cold-patch-test"
qemu-arm -L "$sysroot" "$build/cold-patch-test"
"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S \
    src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S \
    src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    tests/adapter/session_early_init_test.cpp -ldl -o "$build/session-early-init-test"
qemu-arm -L "$sysroot" "$build/session-early-init-test"
