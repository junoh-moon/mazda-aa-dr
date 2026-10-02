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
    src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    -Wl,-z,defs -ldl -o "$build/libadapter.so"
"${cross}readelf" -d "$build/libadapter.so" > "$build/dynamic.txt"
if grep -q TEXTREL "$build/dynamic.txt"; then
    echo 'ERROR: adapter has dynamic text relocations' >&2; exit 1
fi
"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    tests/adapter/adapter_test.cpp -ldl -o "$build/adapter-test"
"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    tests/adapter/veneer_arm_test.cpp tests/adapter/veneer_arm_fixture.S \
    -ldl -o "$build/veneer-test"
for test in observe scrub native malformed nested assist epoch reacquire expiry encoder backend request; do
    qemu-arm -L "$sysroot" "$build/adapter-test" "$test"
done
"${cross}g++" $flags -I src tests/adapter/association_context_test.cpp src/adapter/adapter.cpp src/adapter/bus_hooks.cpp src/runtime/request_trace.cpp -o "$build/association-context-test"
for scenario in captured nested mutate_after nested_missing wrong_call wrong_generation wrong_request wrong_worker wrong_stage unavailable missing malformed request_failed reader_conflict reader_mismatch frame_reuse provenance_failed presence_empty presence_present legacy_layout invalid_presence presence_without_origin status_empty status_a status_v status_other invalid_status status_without_origin legacy_layout_v2; do
    qemu-arm -L "$sysroot" "$build/association-context-test" "$scenario"
done
"${cross}g++" $flags -I src tests/adapter/context_pool_association_test.cpp src/adapter/adapter.cpp src/adapter/bus_hooks.cpp src/runtime/request_trace.cpp -o "$build/context-pool-association-test"
for scenario in capacity capacity_raw reuse nested concurrent fork_full fork_live fork_nested fork_unavailable fork_depth9 fork_generation early; do
    qemu-arm -L "$sysroot" "$build/context-pool-association-test" "$scenario"
done
"${cross}g++" $flags -I src tests/adapter/context_pool_atfork_failure_test.cpp src/adapter/adapter.cpp src/adapter/bus_hooks.cpp src/runtime/request_trace.cpp -Wl,--wrap=pthread_atfork -o "$build/context-pool-atfork-failure-test"
qemu-arm -L "$sysroot" "$build/context-pool-atfork-failure-test"
"${cross}g++" $flags -I src tests/adapter/provenance_context_test.cpp \
    src/adapter/adapter.cpp src/adapter/bus_hooks.cpp src/runtime/request_trace.cpp -o "$build/provenance-context-test"
for case in captured nested failure missing invalidate unqualified malformed; do
    qemu-arm -L "$sysroot" "$build/provenance-context-test" "$case"
done
"${cross}g++" $flags -I src tests/adapter/context_pool_test.cpp \
    src/adapter/adapter.cpp src/adapter/bus_hooks.cpp src/runtime/request_trace.cpp \
    -ldl -o "$build/context-pool-test"
qemu-arm -L "$sysroot" "$build/context-pool-test" saturation
qemu-arm -L "$sysroot" "$build/veneer-test"
"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    tests/adapter/veneer_unwind_test.cpp tests/adapter/veneer_unwind_fixture.S \
    -ldl -o "$build/veneer-unwind-test"
for case in throw_position throw_send nested_throw cancel_position cancel_send throw_enter cancel_enter nested_send small_stack deep_nested deep_throw deep_cancel deep_small_stack deep_small_overflow throw_reuse cancel_reuse small_stack_throw small_stack_cancel; do
    qemu-arm -L "$sysroot" "$build/veneer-unwind-test" "$case"
done

"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S \
    src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S \
    src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    tests/adapter/request_hooks_arm_test.cpp tests/adapter/request_hooks_arm_fixture.S \
    -ldl -o "$build/request-hooks-test"
for case in normal unrelated failed_submit destroy_queued throw_notify throw_work cancel_notify cancel_work malformed other_worker delayed_callback throw_getter cancel_getter session_transition; do
    qemu-arm -L "$sysroot" "$build/request-hooks-test" "$case"
done
"${cross}g++" $flags -I src tests/adapter/cold_patch_test.cpp -o "$build/cold-patch-test"
qemu-arm -L "$sysroot" "$build/cold-patch-test"
"${cross}g++" $flags -I src \
    src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S \
    src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S \
    src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    tests/adapter/session_early_init_test.cpp -ldl -o "$build/session-early-init-test"
qemu-arm -L "$sysroot" "$build/session-early-init-test"
"${cross}g++" $flags -I src tests/adapter/session_request_test.cpp \
    src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S \
    src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp \
    -ldl -o "$build/session-request-test"
qemu-arm -L "$sysroot" "$build/session-request-test"
qemu-arm -L "$sysroot" "$build/session-request-test" bus_recreated
"${cross}g++" $flags -I src tests/adapter/bus_early_init_test.cpp \
    src/adapter/bus_hooks.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp \
    src/adapter/arm_veneer.S src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp \
    src/adapter/request_veneer.S src/runtime/request_trace.cpp src/runtime/request_observer.cpp \
    -ldl -o "$build/bus-early-init-test"
qemu-arm -L "$sysroot" "$build/bus-early-init-test"
