#!/bin/sh
# Developer PC only. Runs generated synthetic programs in qemu, no OEM binary.
set -eu
# Match build_arm.build_environment for every compiler and QEMU child, not
# only the identity probe. Preserve explicit CROSS_COMPILE/QEMU_SYSROOT.
unset MAKEFLAGS GNUMAKEFLAGS MFLAGS MAKEOVERRIDES MAKEFILES MAKELEVEL \
    GCC_EXEC_PREFIX COMPILER_PATH LIBRARY_PATH CPATH C_INCLUDE_PATH \
    CPLUS_INCLUDE_PATH DEPENDENCIES_OUTPUT SUNPRO_DEPENDENCIES LD_RUN_PATH
# QEMU also inherits guest loader settings. The loader test adds only its
# selected production preload explicitly with qemu-arm -E below.
unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT QEMU_SET_ENV QEMU_UNSET_ENV QEMU_LD_PREFIX
LC_ALL=C
export LC_ALL
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
: "${CROSS_COMPILE:?Set the CMU-compatible compiler prefix}"
: "${QEMU_SYSROOT:?Set its sysroot}"
cd "$project"
build=$project/build/arm-full-tests
preload=${MX5DR_ARM_LIBRARY:-$project/build/libmx5dr.so}
if [ -n "${MX5DR_ARM_BUILD:-}" ]; then
    preload=$MX5DR_ARM_BUILD/libmx5dr.so
fi
[ -s "$preload" ] || { echo "Missing production preload: $preload" >&2; exit 1; }
verify_inputs() {
    if [ -n "${MX5DR_ARM_BUILD:-}" ]; then
        python3 tools/check_arm_test_inputs.py --library "$preload" \
            --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
            --release-build "$MX5DR_ARM_BUILD"
    else
        python3 tools/check_arm_test_inputs.py --library "$preload" \
            --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT"
    fi
}
verify_inputs
mkdir -p "$build"
arch='-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm'
warn='-O2 -Wall -Wextra -Werror'
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_motion_batch.cpp -o "$build/motion-batch-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/motion-batch-test"
dbus="-I$QEMU_SYSROOT/usr/include/dbus-1.0 -I$QEMU_SYSROOT/usr/lib/dbus-1.0/include"
"${CROSS_COMPILE}gcc" -std=c99 $warn -pedantic $arch -Isrc/core src/core/dr_core.c tests/core/test_core.c -lm -o "$build/core-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/core-test"
"${CROSS_COMPILE}gcc" -std=c99 $warn -pedantic $arch -Isrc/core -c src/core/dr_core.c -o "$build/core.o"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/runtime/core_bridge.cpp tests/integration/test_pipeline.cpp src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -lm -ldl -pthread -o "$build/pipeline-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/pipeline-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch src/runtime/config.cpp src/runtime/sha256.cpp tests/runtime/test_runtime.cpp -o "$build/runtime-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/runtime-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/runtime/request_trace.cpp tests/runtime/test_request_trace.cpp -pthread -o "$build/request-trace-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/request-trace-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/runtime/request_trace.cpp src/runtime/request_observer.cpp tests/runtime/test_request_observer.cpp -pthread -o "$build/request-observer-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/request-observer-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_model_session.cpp -o "$build/model-session-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/model-session-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_request_handoff.cpp -pthread -o "$build/request-handoff-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/request-handoff-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/runtime/request_trace.cpp tests/runtime/test_request_status.cpp -pthread -o "$build/request-status-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/request-status-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/packaging/test_oem_session_callbacks.cpp src/runtime/sha256.cpp -ldl -pthread -lrt -o "$build/session-callback-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/session-callback-test"
MX5DR_PUBLICATION_BUILD="$build/request-publication" sh tests/runtime/run_request_publication.sh
MX5DR_JOURNAL_BOUNDARY_BUILD="$build/journal-boundaries" sh tests/runtime/run_journal_boundaries.sh
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_journal_queue.cpp -pthread -o "$build/journal-queue-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/journal-queue-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/loader.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp "$build/core.o" tests/runtime/test_journal.cpp -ldl -pthread -lrt -lm -o "$build/journal-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/journal-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/loader.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp "$build/core.o" tests/runtime/test_worker_session.cpp -ldl -pthread -lrt -lm -o "$build/worker-session-test"
for scenario in destroy recreate status failed_create ambiguous inflight bus_disconnect bus_reconnect bus_reuse bus_closed bus_signal bus_ambiguous bus_inflight bus_free_inflight bus_late_same; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/worker-session-test" "$scenario"
done
MX5DR_TEST_STALE_RAW=1 qemu-arm -L "$QEMU_SYSROOT" "$build/worker-session-test" bus_reuse
for scenario in bus_disconnect bus_reconnect bus_reuse bus_closed bus_signal bus_ambiguous bus_inflight; do
    MX5DR_TEST_PREGAP=1 qemu-arm -L "$QEMU_SYSROOT" "$build/worker-session-test" "$scenario"
done
for fixture in reset input; do
    "${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/loader.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp "$build/core.o" "tests/runtime/test_model_session_$fixture.cpp" -ldl -pthread -lrt -lm -o "$build/model-session-$fixture-test"
    qemu-arm -L "$QEMU_SYSROOT" "$build/model-session-$fixture-test"
    if [ "$fixture" = reset ]; then qemu-arm -L "$QEMU_SYSROOT" "$build/model-session-$fixture-test" bus; fi
done
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch $dbus tests/collector/test_journal.cpp src/runtime/config.cpp -ldbus-1 -pthread -lrt -o "$build/collector-journal-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/collector-journal-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch tests/runtime/test_loader.cpp -ldl -pthread -o "$build/loader-test"
qemu-arm -L "$QEMU_SYSROOT" -E "LD_PRELOAD=$preload" "$build/loader-test" "$preload"
sh tests/adapter/run_arm.sh
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --output-dir "$build/unwind-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite request --output-dir "$build/request-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite session --output-dir "$build/session-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite bus --output-dir "$build/bus-dso"

"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp tests/navigation/test_navigation.cpp src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -lm -ldl -pthread -lrt -o "$build/navigation-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/navigation-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/sensors/test_vim_source.cpp src/sensors/vim_source.cpp -o "$build/vim-parser-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/vim-parser-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/sensors/test_vim_tap.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp src/runtime/sha256.cpp src/runtime/config.cpp -ldl -pthread -lrt -o "$build/vim-tap-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/vim-tap-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -DMX5DR_CHANNEL_RECVMSG_WRAP -Wl,--wrap=recvmsg -Isrc tests/navigation/test_channel.cpp src/navigation/channel.cpp -pthread -lrt -o "$build/motion-channel-test"
result=0
qemu-arm -L "$QEMU_SYSROOT" "$build/motion-channel-test" || result=$?
[ "$result" -eq 0 ] || [ "$result" -eq 77 ]

"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/navigation/test_live_pipeline.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp src/navigation/pipeline.cpp src/runtime/core_bridge.cpp src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -lm -ldl -pthread -lrt -o "$build/live-pipeline-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/live-pipeline-test"

# The same worker algorithms and test fixtures on the release ARM32 ABI.
for fixture in gyro_bias gps_wheel holdout; do
  "${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc "tests/navigation/test_${fixture}.cpp" src/navigation/pipeline.cpp src/navigation/holdout.cpp src/runtime/core_bridge.cpp src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -lm -ldl -pthread -o "$build/${fixture}-test"
  qemu-arm -L "$QEMU_SYSROOT" "$build/${fixture}-test"
done

"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_shadow_log.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/runtime/core_bridge.cpp src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -lm -ldl -pthread -o "$build/shadow-log-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/shadow-log-test"
verify_inputs
