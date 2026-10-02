#!/bin/sh
# Developer PC only. Runs authored ARM fixtures in QEMU. When MX5DR_LDS_STOCK
# is provided, the LDS installation suite also executes private OEM libraries.
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
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/adapter/data_patch_test.cpp -o "$build/data-patch-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/data-patch-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/sensors/test_lds_tap.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/lds_sideband.cpp src/runtime/lds_association_channel.cpp -pthread -lrt -o "$build/lds-tap-test"
for scenario in off invalid missing_mode missing_config disabled marker_symlink marker_error observe scrub install_failed rollback_failed cold_lost normal late_receiver sender_failed unrequested null_handle repeated association association_fork association_failed association_invalidate; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/lds-tap-test" "$scenario"
done
if [ -n "${MX5DR_LDS_STOCK:-}" ]; then
    LDS_INSTALL_BUILD="$build/lds-install" sh tests/adapter/run_lds_install.sh
else
    echo 'SKIP: original LDS cold installer needs private MX5DR_LDS_STOCK'
fi
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/adapter/lds_hooks_test.cpp tests/adapter/lds_relay.S src/adapter/lds_hooks.cpp src/sensors/lds_lineage.cpp src/runtime/lds_sideband.cpp -pthread -lrt -o "$build/lds-hooks-test"
for scenario in chain prepare inactive register inline retained read_copy snapshot missing_read wrong_pointer lifetime late unwind cancel chain_mismatch endpoint endpoint_post nested_path failed_send failed_build path_unwind all_ids initialize_overlap initialize_unwind locked locked_pair locked_unknown locked_unthreaded locked_null locked_generation locked_mutex locked_native_pc locked_message_pc locked_native_fail locked_no_native locked_native_twice locked_message_twice locked_reply locked_destination locked_endpoint locked_failed_send locked_unwind locked_extra_send locked_inactive locked_clock locked_type locked_zero_request locked_endpoint_changed locked_snapshot locked_nested_send; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/lds-hooks-test" "$scenario"
done
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_lds_sideband.cpp src/runtime/lds_sideband.cpp -pthread -lrt -o "$build/lds-sideband-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/lds-sideband-test"
MX5DR_LDS_FIXTURE="qemu-arm -L $QEMU_SYSROOT $build/lds-sideband-test" \
    MX5DR_LDS_HOOK_FIXTURE="qemu-arm -L $QEMU_SYSROOT $build/lds-hooks-test" \
    python3 -m unittest discover -s tests/journal -p test_lds_sideband.py -v
"${CROSS_COMPILE}gcc" -std=c99 $warn -pedantic $arch -Isrc/core src/core/dr_core.c tests/core/test_core.c -lm -o "$build/core-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/core-test"
"${CROSS_COMPILE}gcc" -std=c99 $warn -pedantic $arch -Isrc/core -c src/core/dr_core.c -o "$build/core.o"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_lds_association_channel.cpp src/runtime/lds_association_channel.cpp -Wl,--wrap=pwrite -Wl,--wrap=mmap -pthread -lrt -o "$build/lds-association-channel-test"
for scenario in basic exact capacity invalidation fork stop allocation scalar_bits old_offer loss borrowing exhaustion fd_validation drain_bound malformed_map concurrent retirement_scheduled; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/lds-association-channel-test" "$scenario"
done
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/adapter/runtime_lds_association_test.cpp src/runtime/lds_sideband.cpp src/runtime/lds_request_source.cpp src/runtime/lds_association_channel.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/loader.cpp src/runtime/assist_worker.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp "$build/core.o" -ldl -pthread -lrt -lm -o "$build/runtime-lds-association-test"
for scenario in adopted freeze audit journal_failure pre_stopped fork journal bounds; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/runtime-lds-association-test" "$scenario"
done
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_worker_lds.cpp src/runtime/lds_sideband.cpp src/runtime/lds_request_source.cpp src/runtime/lds_association_channel.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/loader.cpp src/runtime/assist_worker.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp "$build/core.o" -ldl -pthread -lrt -lm -o "$build/worker-lds-test"
for scenario in capture occupied pre_stopped bounded malformed wrong_uid; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/worker-lds-test" "$scenario"
done
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_lds_request_source.cpp src/runtime/lds_request_source.cpp src/runtime/lds_sideband.cpp src/runtime/request_trace.cpp -pthread -lrt -o "$build/lds-request-source-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/lds-request-source-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_lds_source_bus.cpp src/runtime/lds_request_source.cpp src/runtime/lds_sideband.cpp src/runtime/request_trace.cpp -pthread -lrt -o "$build/lds-source-bus-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/lds-source-bus-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_worker_lds_source.cpp src/runtime/lds_sideband.cpp src/runtime/lds_request_source.cpp src/runtime/lds_association_channel.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/loader.cpp src/runtime/assist_worker.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp "$build/core.o" -ldl -pthread -lrt -lm -o "$build/worker-lds-source-test"
for scenario in position_first sideband_first mismatch late_conflict pre_stopped malformed_recovery first_bus startup_bus bus_reconnect; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/worker-lds-source-test" "$scenario"
done
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/runtime/core_bridge.cpp tests/integration/test_pipeline.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -lm -ldl -pthread -o "$build/pipeline-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/pipeline-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch src/runtime/config.cpp src/runtime/sha256.cpp tests/runtime/test_runtime.cpp -o "$build/runtime-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/runtime-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/runtime/request_trace.cpp tests/runtime/test_request_trace.cpp -pthread -o "$build/request-trace-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/request-trace-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/runtime/request_trace.cpp src/runtime/request_observer.cpp tests/runtime/test_request_observer.cpp -pthread -o "$build/request-observer-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/request-observer-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/adapter/bus_endpoint_test.cpp tests/adapter/bus_endpoint_relay.S src/adapter/bus_hooks.cpp src/adapter/adapter.cpp src/runtime/request_observer.cpp src/runtime/request_trace.cpp -pthread -o "$build/bus-endpoint-test"
for scenario in normal missing_guid missing_unique empty long failed_register failed_connect early_close wrong_raw wrong_caller duplicate nested getter_nested register_throw getter_throw getter_cancel connect_throw raw_mismatch reconnect_before_send reconnect_in_send transition_send reconnect address_reuse readers no_api; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/bus-endpoint-test" "$scenario"
done
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite endpoint --output-dir "$build/endpoint-dso"
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
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/loader.cpp src/runtime/assist_worker.cpp src/runtime/lds_sideband.cpp src/runtime/lds_request_source.cpp src/runtime/lds_association_channel.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp "$build/core.o" tests/runtime/test_journal.cpp -ldl -pthread -lrt -lm -o "$build/journal-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/journal-test"
for scenario in startup during query invalid healthy; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/journal-test" --storage "$scenario"
done
for journal_suite in test_shadow_results.py test_request_log.py; do
    MX5DR_JOURNAL_FIXTURE="qemu-arm -L $QEMU_SYSROOT $build/journal-test" \
        python3 -m unittest discover -s tests/journal -p "$journal_suite" -v
done
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/loader.cpp src/runtime/assist_worker.cpp src/runtime/lds_sideband.cpp src/runtime/lds_request_source.cpp src/runtime/lds_association_channel.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp "$build/core.o" tests/runtime/test_worker_session.cpp -ldl -pthread -lrt -lm -o "$build/worker-session-test"
for scenario in destroy recreate status failed_create ambiguous inflight bus_disconnect bus_reconnect bus_reuse bus_closed bus_signal bus_ambiguous bus_inflight bus_free_inflight bus_late_same; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/worker-session-test" "$scenario"
done
MX5DR_TEST_STALE_RAW=1 qemu-arm -L "$QEMU_SYSROOT" "$build/worker-session-test" bus_reuse
MX5DR_TEST_SLOW_YAW=1 qemu-arm -L "$QEMU_SYSROOT" "$build/worker-session-test" bus_reuse
for scenario in bus_disconnect bus_reconnect bus_reuse bus_closed bus_signal bus_ambiguous bus_inflight; do
    MX5DR_TEST_PREGAP=1 qemu-arm -L "$QEMU_SYSROOT" "$build/worker-session-test" "$scenario"
done
for fixture in reset input; do
    "${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/loader.cpp src/runtime/assist_worker.cpp src/runtime/lds_sideband.cpp src/runtime/lds_request_source.cpp src/runtime/lds_association_channel.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp "$build/core.o" "tests/runtime/test_model_session_$fixture.cpp" -ldl -pthread -lrt -lm -o "$build/model-session-$fixture-test"
    qemu-arm -L "$QEMU_SYSROOT" "$build/model-session-$fixture-test"
    if [ "$fixture" = reset ]; then qemu-arm -L "$QEMU_SYSROOT" "$build/model-session-$fixture-test" bus; fi
done
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch $dbus tests/collector/test_journal.cpp src/runtime/config.cpp -ldbus-1 -pthread -lrt -o "$build/collector-journal-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/collector-journal-test"
for scenario in startup during query invalid healthy; do
    qemu-arm -L "$QEMU_SYSROOT" "$build/collector-journal-test" --storage "$scenario"
done
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch tests/runtime/test_loader.cpp -ldl -pthread -o "$build/loader-test"
qemu-arm -L "$QEMU_SYSROOT" -E "LD_PRELOAD=$preload" "$build/loader-test" "$preload"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_assist_worker.cpp src/runtime/assist_worker.cpp src/runtime/core_bridge.cpp src/navigation/pipeline.cpp src/navigation/channel.cpp src/navigation/holdout.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -pthread -ldl -lrt -lm -o "$build/assist-worker-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/assist-worker-test"
sh tests/adapter/run_arm.sh
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --output-dir "$build/unwind-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite request --output-dir "$build/request-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite request-wire --output-dir "$build/request-wire-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite provenance-context --output-dir "$build/provenance-context-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite association-context --output-dir "$build/association-context-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite context-pool --output-dir "$build/context-pool-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite session --output-dir "$build/session-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite bus --output-dir "$build/bus-dso"
python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite assist --output-dir "$build/assist-dso"

"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc src/navigation/pipeline.cpp src/navigation/holdout.cpp src/navigation/channel.cpp src/runtime/core_bridge.cpp tests/navigation/test_navigation.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -lm -ldl -pthread -lrt -o "$build/navigation-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/navigation-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/sensors/test_vim_source.cpp src/sensors/vim_source.cpp -o "$build/vim-parser-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/vim-parser-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/sensors/test_lds_lineage.cpp src/sensors/lds_lineage.cpp -o "$build/lds-lineage-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/lds-lineage-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/sensors/test_vim_tap.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp src/runtime/sha256.cpp src/runtime/config.cpp -ldl -pthread -lrt -o "$build/vim-tap-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/vim-tap-test"
"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -DMX5DR_CHANNEL_RECVMSG_WRAP -Wl,--wrap=recvmsg -Isrc tests/navigation/test_channel.cpp src/navigation/channel.cpp -pthread -lrt -o "$build/motion-channel-test"
result=0
qemu-arm -L "$QEMU_SYSROOT" "$build/motion-channel-test" || result=$?
[ "$result" -eq 0 ] || [ "$result" -eq 77 ]

"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/navigation/test_live_pipeline.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp src/navigation/pipeline.cpp src/runtime/core_bridge.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -lm -ldl -pthread -lrt -o "$build/live-pipeline-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/live-pipeline-test"

# The same worker algorithms and test fixtures on the release ARM32 ABI.
for fixture in gyro_bias gps_wheel holdout; do
  "${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc "tests/navigation/test_${fixture}.cpp" src/navigation/pipeline.cpp src/navigation/holdout.cpp src/runtime/core_bridge.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -lm -ldl -pthread -o "$build/${fixture}-test"
  qemu-arm -L "$QEMU_SYSROOT" "$build/${fixture}-test"
done

"${CROSS_COMPILE}g++" -std=c++11 $warn $arch -Isrc tests/runtime/test_shadow_log.cpp src/navigation/pipeline.cpp src/navigation/holdout.cpp src/runtime/core_bridge.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/arm_veneer.S src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/adapter/request_veneer.S src/runtime/request_observer.cpp src/runtime/request_trace.cpp "$build/core.o" -lm -ldl -pthread -o "$build/shadow-log-test"
qemu-arm -L "$QEMU_SYSROOT" "$build/shadow-log-test"
MX5DR_SHADOW_FIXTURE="qemu-arm -L $QEMU_SYSROOT $build/shadow-log-test" \
    python3 -m unittest discover -s tests/journal -p test_calibration_logs.py -v

python3 tests/adapter/run_unwind_dso.py --library "$preload" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$QEMU_SYSROOT" \
    --suite runtime-assist --output-dir "$build/runtime-assist-dso"
verify_inputs
