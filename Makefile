# Host checks are independent of the CMU. ARM_PREFIX must select a compatible
# ARMv7 softfp toolchain and sysroot, not the host compiler.
CC ?= cc
CXX ?= c++
PYTHON ?= python3
BUILD ?= build
C_WARN = -std=c99 -O2 -Wall -Wextra -Werror -pedantic
CXX_WARN = -std=c++11 -O2 -Wall -Wextra -Werror -Isrc
CORE = src/core/dr_core.c
REQUEST = src/adapter/request_hooks.cpp src/runtime/request_observer.cpp src/runtime/request_trace.cpp
ADAPTER = src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp $(REQUEST)
NAVIGATION = src/navigation/pipeline.cpp src/navigation/channel.cpp src/navigation/holdout.cpp
NAV_HEADERS = src/navigation/channel.h src/navigation/pipeline.h src/navigation/gyro_bias.h src/navigation/gps_wheel.h src/navigation/holdout.h src/runtime/beta_profile.h src/runtime/core_bridge.h
SENSOR_TAP = src/sensors/vim_tap.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp src/runtime/config.cpp src/runtime/sha256.cpp
SENSOR_OBJECTS = $(patsubst %.cpp,$(BUILD)/arm/%.o,$(SENSOR_TAP))
RUNTIME_SUPPORT = src/runtime/config.cpp src/runtime/sha256.cpp
LDS_SIDEBAND = src/runtime/lds_sideband.cpp
LDS_REQUEST_SOURCE = src/runtime/lds_request_source.cpp
LDS_ASSOCIATION = src/runtime/lds_association_channel.cpp
LDS_HEADERS = src/runtime/lds_sideband.h src/sensors/lds_lineage.h src/sensors/nmea_course_token.h src/runtime/lds_association.h src/runtime/lds_association_channel.h src/runtime/lds_association_protocol.h
LDS_HOOKS = src/adapter/lds_hooks.cpp src/sensors/lds_lineage.cpp src/sensors/nmea_course_token.cpp
LDS_TAP = $(LDS_HOOKS) $(LDS_SIDEBAND) $(LDS_ASSOCIATION) $(RUNTIME_SUPPORT) src/sensors/lds_tap.cpp src/adapter/lds_install.cpp src/adapter/bus_hooks.cpp src/runtime/request_trace.cpp src/runtime/loader.cpp
LDS_OBJECTS = $(patsubst %.cpp,$(BUILD)/arm-lds/%.o,$(LDS_TAP))
STORAGE_HEADERS = src/runtime/storage.h src/runtime/boot_id.h
HOST_DBUS_FLAGS = $(shell pkg-config --cflags dbus-1)
HOST_DBUS_LIBS = $(shell pkg-config --libs dbus-1)
ARM_PREFIX ?=
ARM_SYSROOT ?=
ARM_FLAGS = -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm
ARM_CPPFLAGS = -Isrc -I$(ARM_SYSROOT)/usr/include/dbus-1.0 -I$(ARM_SYSROOT)/usr/lib/dbus-1.0/include
ARM_CXXFLAGS = -std=c++11 -Os -Wall -Wextra -Werror -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fno-omit-frame-pointer -ftls-model=initial-exec $(ARM_FLAGS)
ARM_DEPFLAGS = -MMD -MP -MF $(@:.o=.d).tmp -MT $@
ASSIST_WORKER = src/runtime/assist_worker.cpp
ARM_SOURCES = $(ASSIST_WORKER) src/runtime/loader.cpp $(ADAPTER) $(RUNTIME_SUPPORT) $(LDS_SIDEBAND) $(LDS_REQUEST_SOURCE) $(LDS_ASSOCIATION) src/runtime/runtime.cpp src/runtime/core_bridge.cpp $(NAVIGATION)
ARM_OBJECTS = $(patsubst %.cpp,$(BUILD)/arm/%.o,$(ARM_SOURCES)) $(BUILD)/arm/src/core/dr_core.o $(BUILD)/arm/src/adapter/arm_veneer.o $(BUILD)/arm/src/adapter/request_veneer.o
COLLECTOR_OBJECTS = $(BUILD)/arm/src/collector/collector.o $(BUILD)/arm/src/runtime/config.o
GUARD_OBJECTS = $(BUILD)/arm/src/guard/guard.o $(BUILD)/arm/src/runtime/sha256.o
HASH_OBJECTS = $(BUILD)/arm/src/tools/sha256_main.o $(BUILD)/arm/src/runtime/sha256.o
ALL_ARM_OBJECTS = $(sort $(ARM_OBJECTS) $(SENSOR_OBJECTS) $(COLLECTOR_OBJECTS) $(GUARD_OBJECTS) $(HASH_OBJECTS) $(LDS_OBJECTS))

.PHONY: all test test-build-deps test-motion-journal test-recovery test-loader test-core test-adapter test-runtime test-lds test-request-publication test-journal-boundaries test-collector test-packaging test-tools test-integration test-navigation test-sensors arm clean
all: test
$(BUILD):
	mkdir -p $@
$(BUILD)/test_core: $(CORE) src/core/dr_core.h tests/core/test_core.c | $(BUILD)
	$(CC) $(C_WARN) -Isrc/core $(CORE) tests/core/test_core.c -lm -o $@
$(BUILD)/replay: $(CORE) src/core/dr_core.h tests/core/replay.c | $(BUILD)
	$(CC) $(C_WARN) -Isrc/core $(CORE) tests/core/replay.c -lm -o $@
$(BUILD)/test_adapter: $(ADAPTER) src/adapter/adapter.h tests/adapter/adapter_test.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(ADAPTER) tests/adapter/adapter_test.cpp -ldl -pthread -o $@
$(BUILD)/test_provenance_context: src/adapter/adapter.cpp src/adapter/bus_hooks.cpp src/adapter/adapter.h src/runtime/request_trace.cpp src/runtime/request_trace.h src/runtime/session_trace.h src/runtime/bus_trace.h tests/adapter/provenance_context_test.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
$(BUILD)/test_context_pool: src/adapter/adapter.cpp src/adapter/bus_hooks.cpp src/adapter/adapter.h src/runtime/request_trace.cpp src/runtime/request_trace.h tests/adapter/context_pool_test.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
$(BUILD)/test_runtime: $(RUNTIME_SUPPORT) tests/runtime/test_runtime.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(RUNTIME_SUPPORT) tests/runtime/test_runtime.cpp -o $@
$(BUILD)/test_request_trace: src/runtime/request_trace.cpp src/runtime/request_trace.h tests/runtime/test_request_trace.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) src/runtime/request_trace.cpp tests/runtime/test_request_trace.cpp -pthread -o $@
$(BUILD)/test_model_session: tests/runtime/test_model_session.cpp src/runtime/model_bus.h src/runtime/model_session.h src/runtime/session_trace.h src/adapter/adapter.h src/runtime/request_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $< -o $@
$(BUILD)/test_request_observer: src/runtime/request_trace.cpp src/runtime/request_trace.h src/runtime/request_observer.cpp src/runtime/request_observer.h tests/runtime/test_request_observer.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
$(BUILD)/test_request_handoff: tests/runtime/test_request_handoff.cpp src/runtime/request_trace.cpp src/runtime/request_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $< -pthread -o $@
$(BUILD)/test_request_status: tests/runtime/test_request_status.cpp src/runtime/request_trace.cpp src/runtime/request_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) src/runtime/request_trace.cpp tests/runtime/test_request_status.cpp -pthread -o $@
$(BUILD)/test_journal_queue: tests/runtime/test_journal_queue.cpp src/runtime/journal_queue.h src/adapter/adapter.h src/runtime/request_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $< -pthread -o $@
$(BUILD)/test_journal: $(ASSIST_WORKER) src/runtime/assist_worker.h src/runtime/worker.h $(BUILD)/core_host.o $(NAVIGATION) src/runtime/core_bridge.cpp $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/motion_batch.h tests/runtime/test_journal.cpp $(ADAPTER) src/runtime/loader.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(ASSIST_WORKER) $(RUNTIME_SUPPORT) $(LDS_SIDEBAND) $(LDS_REQUEST_SOURCE) $(LDS_ASSOCIATION) $(ADAPTER) $(NAVIGATION) src/runtime/core_bridge.cpp $(BUILD)/core_host.o src/runtime/loader.cpp tests/runtime/test_journal.cpp -ldl -lpthread -lrt -lm -o $@
$(BUILD)/test_worker_session: $(ASSIST_WORKER) src/runtime/assist_worker.h src/runtime/worker.h $(BUILD)/core_host.o $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/model_session.h src/runtime/session_trace.h tests/runtime/test_worker_session.cpp $(ADAPTER) src/runtime/loader.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(ASSIST_WORKER) $(RUNTIME_SUPPORT) $(LDS_SIDEBAND) $(LDS_REQUEST_SOURCE) $(LDS_ASSOCIATION) $(ADAPTER) $(NAVIGATION) src/runtime/core_bridge.cpp $(BUILD)/core_host.o src/runtime/loader.cpp tests/runtime/test_worker_session.cpp -ldl -lpthread -lrt -lm -o $@
$(BUILD)/test_model_session_reset: $(ASSIST_WORKER) src/runtime/assist_worker.h src/runtime/worker.h $(BUILD)/core_host.o $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/model_session.h src/runtime/session_trace.h tests/runtime/test_model_session_reset.cpp $(ADAPTER) src/runtime/loader.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(ASSIST_WORKER) $(RUNTIME_SUPPORT) $(LDS_SIDEBAND) $(LDS_REQUEST_SOURCE) $(LDS_ASSOCIATION) $(ADAPTER) $(NAVIGATION) src/runtime/core_bridge.cpp $(BUILD)/core_host.o src/runtime/loader.cpp tests/runtime/test_model_session_reset.cpp -ldl -lpthread -lrt -lm -o $@
$(BUILD)/test_model_session_input: $(ASSIST_WORKER) src/runtime/assist_worker.h src/runtime/worker.h $(BUILD)/core_host.o $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/model_session.h src/runtime/session_trace.h tests/runtime/test_model_session_input.cpp $(ADAPTER) src/runtime/loader.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(ASSIST_WORKER) $(RUNTIME_SUPPORT) $(LDS_SIDEBAND) $(LDS_REQUEST_SOURCE) $(LDS_ASSOCIATION) $(ADAPTER) $(NAVIGATION) src/runtime/core_bridge.cpp $(BUILD)/core_host.o src/runtime/loader.cpp tests/runtime/test_model_session_input.cpp -ldl -lpthread -lrt -lm -o $@
$(BUILD)/mx5dr-collector-host: src/collector/collector.cpp src/runtime/config.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(HOST_DBUS_FLAGS) $(filter-out %.h,$^) $(HOST_DBUS_LIBS) -lpthread -lrt -o $@
$(BUILD)/test_collector: src/collector/collector.cpp src/runtime/config.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) -DMX5_COLLECTOR_TESTING $(HOST_DBUS_FLAGS) $(filter-out %.h,$^) $(HOST_DBUS_LIBS) -lpthread -lrt -o $@
$(BUILD)/test_collector_journal: tests/collector/test_journal.cpp src/collector/collector.cpp src/runtime/config.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(HOST_DBUS_FLAGS) tests/collector/test_journal.cpp src/runtime/config.cpp $(HOST_DBUS_LIBS) -lpthread -lrt -o $@
test-collector: $(BUILD)/test_collector $(BUILD)/mx5dr-collector-host $(BUILD)/test_collector_journal $(BUILD)/test_journal $(BUILD)/test_adapter
	$(BUILD)/test_collector_journal
	@set -e; for scenario in startup during query invalid healthy; do $(BUILD)/test_collector_journal --storage $$scenario; done
	MX5DR_TEST_BUILD=$(abspath $(BUILD)) $(PYTHON) -m unittest discover -s tests/collector -v
test-core: $(BUILD)/test_core $(BUILD)/replay
	$(BUILD)/test_core
	$(PYTHON) tests/core/test_replay.py $(BUILD)/replay
$(BUILD)/test_cold_patch: tests/adapter/cold_patch_test.cpp src/adapter/cold_patch.h src/adapter/adapter.h | $(BUILD)
	$(CXX) $(CXX_WARN) $< -o $@
$(BUILD)/test_session_hooks: $(ADAPTER) src/adapter/adapter.h src/adapter/session_hooks.h src/runtime/session_trace.h src/runtime/request_trace.h src/adapter/request_hooks.h src/runtime/request_observer.h tests/adapter/session_hooks_test.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -ldl -pthread -o $@
$(BUILD)/test_session_early_init: $(ADAPTER) src/adapter/adapter.h src/adapter/session_hooks.h src/runtime/session_trace.h src/runtime/request_trace.h src/adapter/request_hooks.h src/runtime/request_observer.h tests/adapter/session_early_init_test.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -ldl -pthread -o $@
$(BUILD)/test_session_request: tests/adapter/session_request_test.cpp src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/adapter.cpp src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/adapter/request_hooks.cpp src/adapter/request_hooks.h src/adapter/session_hooks.h src/adapter/adapter.h src/runtime/request_observer.h src/runtime/request_trace.h src/runtime/session_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h src/adapter/request_hooks.cpp,$^) -pthread -o $@
$(BUILD)/test_request_wire: tests/adapter/request_wire_test.cpp src/adapter/request_hooks.cpp src/adapter/request_hooks.h src/runtime/request_observer.cpp src/runtime/request_observer.h src/runtime/request_trace.cpp src/runtime/request_trace.h src/runtime/session_trace.h src/runtime/bus_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) -no-pie $(filter-out %.h src/adapter/request_hooks.cpp,$^) -pthread -o $@
$(BUILD)/test_bus_endpoint: tests/adapter/bus_endpoint_test.cpp tests/adapter/bus_endpoint_relay.S src/adapter/bus_hooks.cpp src/adapter/adapter.cpp src/adapter/request_hooks.cpp src/adapter/request_hooks.h src/adapter/bus_hooks.h src/runtime/request_observer.cpp src/runtime/request_observer.h src/runtime/request_trace.cpp src/runtime/request_trace.h src/runtime/request_log.h | $(BUILD)
	$(CXX) $(CXX_WARN) -no-pie $(filter-out %.h src/adapter/request_hooks.cpp,$^) -pthread -o $@
$(BUILD)/test_bus_hooks: tests/adapter/bus_hooks_test.cpp src/adapter/bus_hooks.cpp src/adapter/adapter.cpp src/runtime/request_trace.cpp src/adapter/bus_hooks.h src/adapter/adapter.h src/runtime/request_trace.h src/runtime/session_trace.h src/runtime/bus_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
$(BUILD)/test_lds_bus_hooks: tests/adapter/lds_bus_hooks_test.cpp src/adapter/bus_hooks.cpp src/runtime/request_trace.cpp src/adapter/bus_hooks.h src/runtime/request_trace.h src/runtime/bus_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
$(BUILD)/test_bus_early_init: tests/adapter/bus_early_init_test.cpp src/adapter/bus_hooks.cpp src/adapter/adapter.cpp src/runtime/request_trace.cpp src/adapter/bus_hooks.h src/adapter/adapter.h src/runtime/request_trace.h src/runtime/session_trace.h src/runtime/bus_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
$(BUILD)/test_install_policy: tests/adapter/test_install_policy.cpp src/adapter/install_policy.h | $(BUILD)
	$(CXX) $(CXX_WARN) tests/adapter/test_install_policy.cpp -o $@
test-adapter: $(BUILD)/test_context_pool_atfork_failure $(BUILD)/test_context_pool_association $(BUILD)/test_association_context $(BUILD)/test_bus_endpoint $(BUILD)/test_bus_hooks $(BUILD)/test_lds_bus_hooks $(BUILD)/test_bus_early_init $(BUILD)/test_adapter $(BUILD)/test_provenance_context $(BUILD)/test_context_pool $(BUILD)/test_cold_patch $(BUILD)/test_session_hooks $(BUILD)/test_session_early_init $(BUILD)/test_session_request $(BUILD)/test_request_wire $(BUILD)/test_install_policy
	$(BUILD)/test_install_policy
	@set -e; for case in observe scrub native malformed nested assist assist_beta epoch reacquire expiry encoder backend request beta_disallowed beta_replace beta_accuracy beta_branches beta_hold; do $(BUILD)/test_adapter $$case; done
	@set -e; for scenario in captured nested mutate_after nested_missing wrong_call wrong_generation wrong_request wrong_worker wrong_stage unavailable missing malformed request_failed reader_conflict reader_mismatch frame_reuse provenance_failed presence_empty presence_present legacy_layout invalid_presence presence_without_origin status_empty status_a status_v status_other invalid_status status_without_origin legacy_layout_v2; do $(BUILD)/test_association_context $$scenario; done
	@set -e; for case in captured nested failure missing invalidate unqualified malformed; do $(BUILD)/test_provenance_context $$case; done
	$(BUILD)/test_context_pool saturation
	@set -e; for scenario in capacity capacity_raw reuse nested concurrent fork_full fork_live fork_nested fork_unavailable fork_depth9 fork_generation early; do $(BUILD)/test_context_pool_association $$scenario; done
	$(BUILD)/test_context_pool_atfork_failure
	$(BUILD)/test_cold_patch
	@set -e; for case in normal failure overlap same_storage closing_create creating_during_destroy null_success output_race late_destroy distinct_storage capacity callback_bad callback_null readers throw_create throw_destroy throw_status cancel_create cancel_destroy cancel_status prediction_destroy prediction_recreate prediction_create_failure prediction_destroy_failure prediction_status prediction_create_inflight prediction_destroy_inflight prediction_status_inflight prediction_cached_inflight; do result=0; $(BUILD)/test_session_hooks $$case || result=$$?; [ "$$result" -eq 0 ] || { [ "$$result" -eq 77 ] && [ "$$(uname -s)" = Darwin ]; }; done
	$(BUILD)/test_session_early_init
	$(BUILD)/test_session_request
	$(BUILD)/test_session_request bus_recreated
	$(BUILD)/test_request_wire
	@set -e; for case in normal missing_guid missing_unique empty long failed_register failed_connect early_close wrong_raw wrong_caller duplicate nested getter_nested register_throw getter_throw getter_cancel connect_throw raw_mismatch reconnect_before_send reconnect_in_send transition_send reconnect address_reuse readers no_api; do $(BUILD)/test_bus_endpoint $$case; done
	$(BUILD)/test_bus_early_init
	$(BUILD)/test_lds_bus_hooks
	@set -e; for case in normal position_source position_sources_concurrent signal signal_reuse failure early_close unobserved overlap cancel readers capacity collision bad_callback throw_create throw_connect throw_disconnect throw_free throw_closed prediction_entry_create prediction_entry_connect prediction_entry_disconnect prediction_entry_free prediction_entry_closed prediction_entry_signal prediction_exit_create prediction_exit_connect prediction_exit_disconnect prediction_exit_free prediction_exit_closed prediction_exit_signal; do result=0; $(BUILD)/test_bus_hooks $$case || result=$$?; [ "$$result" -eq 0 ] || { [ "$$result" -eq 77 ] && [ "$$(uname -s)" = Darwin ]; }; done
$(BUILD)/test_worker_thread: tests/runtime/test_worker_thread.cpp src/runtime/worker_thread.h | $(BUILD)
	$(CXX) $(CXX_WARN) -Isrc tests/runtime/test_worker_thread.cpp -pthread -o $@
test-runtime: $(BUILD)/test_worker_thread $(BUILD)/test_runtime_lds_association $(BUILD)/test_assist_worker $(BUILD)/test_runtime_assist $(BUILD)/test_runtime $(BUILD)/test_request_trace $(BUILD)/test_request_observer $(BUILD)/test_request_handoff $(BUILD)/test_request_status $(BUILD)/test_journal_queue $(BUILD)/test_journal $(BUILD)/test_model_session $(BUILD)/test_model_session_reset $(BUILD)/test_model_session_input $(BUILD)/test_worker_session test-request-publication test-journal-boundaries
	$(BUILD)/test_worker_thread
	@set -e; for scenario in adopted freeze audit journal_failure pre_stopped fork journal bounds drain_bus drain_session; do $(BUILD)/test_runtime_lds_association $$scenario; done
	$(BUILD)/test_assist_worker
	@set -e; for scenario in publication source_fault unqualified recovery audit journal_failure pre_stopped unhooked shadow; do $(BUILD)/test_runtime_assist $$scenario; done
	$(BUILD)/test_runtime
	$(BUILD)/test_request_trace
	$(BUILD)/test_request_observer
	$(BUILD)/test_request_handoff
	$(BUILD)/test_request_status
	$(BUILD)/test_journal_queue
	$(BUILD)/test_journal
	$(BUILD)/test_journal --adapter-fault
	$(BUILD)/test_journal --late-adapter-fault
	@set -e; for scenario in startup during query invalid healthy; do $(BUILD)/test_journal --storage $$scenario; done
	$(BUILD)/test_model_session
	$(BUILD)/test_model_session_reset
	$(BUILD)/test_model_session_reset bus
	$(BUILD)/test_model_session_input
	@set -e; for case in destroy recreate status failed_create ambiguous inflight bus_disconnect bus_reconnect bus_reuse bus_closed bus_signal bus_ambiguous bus_inflight bus_free_inflight bus_late_same; do $(BUILD)/test_worker_session $$case; done
	MX5DR_TEST_STALE_RAW=1 $(BUILD)/test_worker_session bus_reuse
	MX5DR_TEST_SLOW_YAW=1 $(BUILD)/test_worker_session bus_reuse
	@set -e; for case in bus_disconnect bus_reconnect bus_reuse bus_closed bus_signal bus_ambiguous bus_inflight; do MX5DR_TEST_PREGAP=1 $(BUILD)/test_worker_session $$case; done
test-journal-boundaries:
	@result=0; MX5DR_JOURNAL_BOUNDARY_BUILD=$(BUILD)/journal-boundaries CXX="$(CXX)" CC="$(CC)" sh tests/runtime/run_journal_boundaries.sh || result=$$?; [ "$$result" -eq 0 ] || [ "$$result" -eq 77 ]
test-request-publication:
	@CXX="$(CXX)" MX5DR_PUBLICATION_BUILD="$(abspath $(BUILD))/request-publication" sh tests/runtime/run_request_publication.sh; result=$$?; test $$result -eq 0 -o $$result -eq 77
test-packaging: $(BUILD)/test_collector
	MX5DR_TEST_BUILD=$(abspath $(BUILD)) $(PYTHON) -m unittest discover -s tests/packaging -v
test-tools:
	$(PYTHON) -m unittest discover -s tests/tools -v
test-build-deps:
	$(PYTHON) -m unittest discover -s tests/build -v
test-integration: $(BUILD)/test_pipeline $(BUILD)/test_assist_publication
	$(BUILD)/test_pipeline
	@set -e; for scenario in straight quality_gap quality_cycle same_mode_no_anchor anchor_only_gap wrong_anchor_token same_time_old_anchor turn reverse expiry reacquire native_return stale_control fault_recovery core_reject reinit failed_reinit failed_model_reinit owner_exit unverified continuous_reacquire anchor_first_reacquire separate_reacquire native_reacquire quality_reacquire single_gap_reacquire ready_quality_reanchor early_measured_reanchor; do $(BUILD)/test_assist_publication $$scenario; done

$(BUILD)/test_assist_publication: tests/adapter/assist_publication_test.cpp $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp src/runtime/core_bridge.h $(ADAPTER) src/adapter/adapter.h $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -ldl -pthread -lm -o $@
$(BUILD)/core_host.o: $(CORE) src/core/dr_core.h | $(BUILD)
	$(CC) $(C_WARN) -c $(CORE) -o $@
$(BUILD)/test_pipeline: $(BUILD)/core_host.o src/runtime/core_bridge.cpp tests/integration/test_pipeline.cpp $(ADAPTER)
	$(CXX) $(CXX_WARN) src/runtime/core_bridge.cpp tests/integration/test_pipeline.cpp $(ADAPTER) $(BUILD)/core_host.o -lm -ldl -pthread -o $@
test-loader:
	$(PYTHON) tests/runtime/test_loader_interposer.py

test-recovery:
	$(PYTHON) tests/recovery/test_guard.py

test-navigation: $(BUILD)/test_navigation $(BUILD)/test_channel $(BUILD)/test_live_pipeline $(BUILD)/test_gyro_bias $(BUILD)/test_gps_wheel $(BUILD)/test_holdout $(BUILD)/test_beta
	$(BUILD)/test_navigation
	$(BUILD)/test_live_pipeline
	$(BUILD)/test_gyro_bias
	$(BUILD)/test_gps_wheel
	$(BUILD)/test_holdout
	$(BUILD)/test_beta
	@$(BUILD)/test_channel; result=$$?; test $$result -eq 0 -o $$result -eq 77
$(BUILD)/test_navigation: tests/navigation/test_navigation.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@
ifeq ($(shell uname -s),Linux)
CHANNEL_TEST_FLAGS = -DMX5DR_CHANNEL_RECVMSG_WRAP -Wl,--wrap=recvmsg
endif
$(BUILD)/test_channel: tests/navigation/test_channel.cpp tests/navigation/receive_clock_fixture.h src/navigation/channel.cpp $(NAV_HEADERS) | $(BUILD)
	$(CXX) $(CXX_WARN) $(CHANNEL_TEST_FLAGS) $(filter %.cpp,$^) -pthread -o $@

test-sensors: $(BUILD)/test_vim_source $(BUILD)/test_vim_tap $(BUILD)/test_lds_lineage $(BUILD)/test_nmea_course_token $(BUILD)/test_lds_course_lineage
	$(BUILD)/test_vim_source
	$(BUILD)/test_vim_tap
	$(BUILD)/test_lds_lineage
	@set -e; for scenario in presence not_validity fields frames capacity limit immutable guard_page status status_bounds; do $(BUILD)/test_nmea_course_token $$scenario; done
	@set -e; for scenario in assignment exact_read unknown_reset invalid status_inheritance status_invalid; do $(BUILD)/test_lds_course_lineage $$scenario; done
$(BUILD)/test_lds_lineage: tests/sensors/test_lds_lineage.cpp src/sensors/lds_lineage.cpp src/sensors/lds_lineage.h src/sensors/nmea_course_token.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -o $@
$(BUILD)/test_nmea_course_token: tests/sensors/test_nmea_course_token.cpp src/sensors/nmea_course_token.cpp src/sensors/nmea_course_token.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -o $@
$(BUILD)/test_lds_course_lineage: tests/sensors/test_lds_course_lineage.cpp src/sensors/lds_lineage.cpp src/sensors/lds_lineage.h src/sensors/nmea_course_token.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -o $@
$(BUILD)/test_vim_source: tests/sensors/test_vim_source.cpp src/sensors/vim_source.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $^ -o $@

test: test-build-deps test-motion-journal test-sensors test-navigation test-recovery test-loader test-core test-adapter test-runtime test-lds test-collector test-packaging test-tools test-integration

$(BUILD)/test_motion_batch: tests/runtime/test_motion_batch.cpp src/runtime/motion_batch.h | $(BUILD)
	$(CXX) $(CXX_WARN) $< -o $@
test-motion-journal: $(BUILD)/test_motion_batch $(BUILD)/test_shadow_log $(BUILD)/test_journal $(BUILD)/test_lds_sideband $(BUILD)/test_lds_hooks
	$(BUILD)/test_motion_batch
	$(BUILD)/test_shadow_log
	MX5DR_MOTION_FIXTURE=$(abspath $(BUILD)/test_motion_batch) MX5DR_SHADOW_FIXTURE=$(abspath $(BUILD)/test_shadow_log) MX5DR_JOURNAL_FIXTURE=$(abspath $(BUILD)/test_journal) MX5DR_LDS_FIXTURE=$(abspath $(BUILD)/test_lds_sideband) MX5DR_LDS_HOOK_FIXTURE=$(abspath $(BUILD)/test_lds_hooks) $(PYTHON) -m unittest discover -s tests/journal -v

$(BUILD)/arm/src/runtime/runtime.o: src/runtime/motion_batch.h

# Only wrappers that surround OEM calls need C++ cleanup/unwind tables.
# A caller's exception or deferred cancellation must cross the shim intact.
$(BUILD)/arm/src/adapter/adapter.o $(BUILD)/arm/src/adapter/arm_entry.o $(BUILD)/arm/src/adapter/request_hooks.o $(BUILD)/arm/src/adapter/bus_hooks.o $(BUILD)/arm/src/adapter/session_hooks.o $(BUILD)/arm/src/adapter/lds_hooks.o $(BUILD)/arm/src/runtime/request_observer.o: override ARM_CXXFLAGS += -fexceptions

arm: $(BUILD)/libmx5dr-vimtap.so $(BUILD)/libmx5dr-ldstap.so $(BUILD)/libmx5dr.so $(BUILD)/mx5dr-collector $(BUILD)/mx5dr-guard $(BUILD)/mx5dr-sha256
$(BUILD)/arm/%.o: %.cpp
	@test -n "$(ARM_PREFIX)" -a -n "$(ARM_SYSROOT)" || { echo 'Set ARM_PREFIX and ARM_SYSROOT'; exit 1; }
	mkdir -p $(dir $@)
	$(ARM_PREFIX)g++ $(ARM_CXXFLAGS) $(ARM_CPPFLAGS) $(ARM_DEPFLAGS) -c $< -o $@
	mv $(@:.o=.d).tmp $(@:.o=.d)
$(BUILD)/arm/%.o: %.c
	mkdir -p $(dir $@)
	$(ARM_PREFIX)gcc -std=c99 -Os -Wall -Wextra -Werror -pedantic -fPIC -fvisibility=hidden $(ARM_FLAGS) $(ARM_DEPFLAGS) -c $< -o $@
	mv $(@:.o=.d).tmp $(@:.o=.d)
$(BUILD)/arm/%.o: %.S
	mkdir -p $(dir $@)
	$(ARM_PREFIX)gcc -fPIC $(ARM_FLAGS) $(ARM_DEPFLAGS) -c $< -o $@
	mv $(@:.o=.d).tmp $(@:.o=.d)
$(BUILD)/libmx5dr.so: $(ARM_OBJECTS)
	$(ARM_PREFIX)g++ -shared $(ARM_FLAGS) -Wl,-z,relro,-z,now,-z,noexecstack,--no-undefined,--exclude-libs,ALL -Wl,-soname,libmx5dr.so -static-libstdc++ -static-libgcc $(ARM_OBJECTS) -ldl -lpthread -lrt -lm -o $@
$(BUILD)/mx5dr-collector: $(COLLECTOR_OBJECTS)
	$(ARM_PREFIX)g++ $(ARM_FLAGS) -Wl,-z,relro,-z,now,-z,noexecstack,--no-undefined -static-libstdc++ -static-libgcc $^ -ldbus-1 -lpthread -lrt -o $@
clean:
	rm -rf $(BUILD)

$(BUILD)/mx5dr-guard: $(GUARD_OBJECTS) | $(BUILD)
	@test -n "$(ARM_PREFIX)" -a -n "$(ARM_SYSROOT)" || { echo "Set ARM_PREFIX and ARM_SYSROOT"; exit 1; }
	$(ARM_PREFIX)g++ $(ARM_CXXFLAGS) -Wl,-z,relro,-z,now,-z,noexecstack -static-libstdc++ -static-libgcc $^ -o $@

$(BUILD)/mx5dr-sha256: $(HASH_OBJECTS) | $(BUILD)
	@test -n "$(ARM_PREFIX)" -a -n "$(ARM_SYSROOT)" || { echo "Set ARM_PREFIX and ARM_SYSROOT"; exit 1; }
	$(ARM_PREFIX)g++ $(ARM_CXXFLAGS) -static -Wl,-z,noexecstack $^ -o $@

$(BUILD)/libmx5dr-vimtap.so: $(SENSOR_OBJECTS)
	$(ARM_PREFIX)g++ -shared $(ARM_FLAGS) -Wl,-z,relro,-z,now,-z,noexecstack,--no-undefined,--exclude-libs,ALL -Wl,-soname,libmx5dr-vimtap.so -static-libstdc++ -static-libgcc $^ -ldl -lpthread -lrt -o $@

# The LDS process has its own loader and immutable hook state. Its bus wrapper
# tracks connection lifetimes without linking the AA prediction/TLS adapter.
# Pinned binutils 2.22 has an ARM unwind assertion with section GC here.
$(LDS_OBJECTS): override ARM_CXXFLAGS += -fexceptions
$(BUILD)/arm-lds/src/runtime/loader.o: override ARM_CPPFLAGS += -DMX5_LOADER_TARGET='"/jci/lds/svcjcilds.so"'
$(BUILD)/arm-lds/%.o: %.cpp
	@test -n "$(ARM_PREFIX)" -a -n "$(ARM_SYSROOT)" || { echo "Set ARM_PREFIX and ARM_SYSROOT"; exit 1; }
	mkdir -p $(dir $@)
	$(ARM_PREFIX)g++ $(ARM_CXXFLAGS) $(ARM_CPPFLAGS) $(ARM_DEPFLAGS) -c $< -o $@
	mv $(@:.o=.d).tmp $(@:.o=.d)
$(BUILD)/libmx5dr-ldstap.so: $(LDS_OBJECTS)
	$(ARM_PREFIX)g++ -shared $(ARM_FLAGS) -Wl,-z,relro,-z,now,-z,noexecstack,--no-undefined,--exclude-libs,ALL -Wl,-soname,libmx5dr-ldstap.so -static-libstdc++ -static-libgcc $^ -ldl -lpthread -lrt -o $@

$(BUILD)/test_vim_tap: tests/sensors/test_vim_tap.cpp src/sensors/vim_tap.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp $(RUNTIME_SUPPORT) | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out src/sensors/vim_tap.cpp,$^) -ldl -pthread -lrt -o $@

$(BUILD)/test_live_pipeline: tests/navigation/test_live_pipeline.cpp src/sensors/vim_source.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@

$(BUILD)/test_gyro_bias: tests/navigation/test_gyro_bias.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@
$(BUILD)/test_holdout: tests/navigation/test_holdout.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@
$(BUILD)/test_beta: tests/navigation/test_beta.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@
$(BUILD)/test_shadow_log: tests/runtime/test_shadow_log.cpp src/runtime/shadow_log.h $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@

$(BUILD)/test_navigation $(BUILD)/test_live_pipeline $(BUILD)/test_journal $(BUILD)/test_gyro_bias $(BUILD)/test_gps_wheel $(BUILD)/test_holdout $(BUILD)/test_beta $(BUILD)/test_shadow_log: $(NAV_HEADERS)
$(BUILD)/test_journal $(BUILD)/arm/src/runtime/runtime.o: src/runtime/shadow_log.h
$(BUILD)/test_holdout $(BUILD)/test_worker_session: src/runtime/shadow_log.h
$(BUILD)/test_journal $(BUILD)/test_collector_journal: tests/runtime/storage_fixture.h
$(BUILD)/test_journal $(BUILD)/test_worker_session $(BUILD)/test_model_session_reset $(BUILD)/test_model_session_input $(BUILD)/test_collector $(BUILD)/test_collector_journal $(BUILD)/mx5dr-collector-host: $(STORAGE_HEADERS)
$(BUILD)/test_journal: src/runtime/request_log.h src/adapter/request_hooks.h
$(BUILD)/test_journal: src/runtime/worker_tick.h src/runtime/journal_queue.h
$(BUILD)/test_adapter $(BUILD)/test_pipeline $(BUILD)/test_navigation $(BUILD)/test_live_pipeline $(BUILD)/test_journal $(BUILD)/test_gyro_bias $(BUILD)/test_gps_wheel $(BUILD)/test_holdout $(BUILD)/test_shadow_log: src/adapter/cold_patch.h src/adapter/request_hooks.h src/runtime/request_observer.h
$(BUILD)/test_adapter $(BUILD)/test_pipeline $(BUILD)/test_navigation $(BUILD)/test_live_pipeline $(BUILD)/test_journal $(BUILD)/test_gyro_bias $(BUILD)/test_gps_wheel $(BUILD)/test_holdout $(BUILD)/test_shadow_log: src/adapter/adapter.h src/runtime/request_trace.h
$(BUILD)/test_adapter $(BUILD)/test_pipeline $(BUILD)/test_navigation $(BUILD)/test_live_pipeline $(BUILD)/test_journal $(BUILD)/test_gyro_bias $(BUILD)/test_gps_wheel $(BUILD)/test_holdout $(BUILD)/test_shadow_log: src/adapter/session_hooks.h src/runtime/session_trace.h
$(BUILD)/test_request_trace $(BUILD)/test_request_status $(BUILD)/test_request_observer $(BUILD)/test_request_handoff $(BUILD)/test_journal_queue: src/runtime/session_trace.h
$(BUILD)/arm/src/navigation/pipeline.o $(BUILD)/arm/src/navigation/holdout.o $(BUILD)/arm/src/runtime/runtime.o: $(NAV_HEADERS)

$(BUILD)/test_gps_wheel: tests/navigation/test_gps_wheel.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@

# Keep compiler-discovered transitive headers, including for guard/hash objects.
# An old build made before dependency tracking (or with a deleted .d file) must
# compile once instead of silently treating its existing object as current.
-include $(ALL_ARM_OBJECTS:.o=.d)
ARM_MISSING_DEPS := $(foreach object,$(ALL_ARM_OBJECTS),$(if $(wildcard $(object:.o=.d)),,$(object)))
ifneq ($(strip $(ARM_MISSING_DEPS)),)
.PHONY: FORCE_ARM_DEPS
$(ARM_MISSING_DEPS): FORCE_ARM_DEPS
FORCE_ARM_DEPS:
endif

# Bus snapshots are carried by every request and observation value.
$(BUILD)/test_model_session_reset $(BUILD)/test_model_session_input: src/adapter/bus_hooks.h src/runtime/bus_trace.h
$(BUILD)/test_adapter $(BUILD)/test_pipeline $(BUILD)/test_navigation $(BUILD)/test_live_pipeline $(BUILD)/test_journal $(BUILD)/test_gyro_bias $(BUILD)/test_gps_wheel $(BUILD)/test_holdout $(BUILD)/test_shadow_log $(BUILD)/test_worker_session $(BUILD)/test_request_trace $(BUILD)/test_request_status $(BUILD)/test_request_observer $(BUILD)/test_request_handoff $(BUILD)/test_journal_queue $(BUILD)/test_model_session $(BUILD)/test_session_hooks $(BUILD)/test_session_early_init $(BUILD)/test_session_request: src/adapter/bus_hooks.h src/runtime/bus_trace.h

$(BUILD)/test_worker_session $(BUILD)/test_model_session_reset: tests/runtime/model_bus_fixture.h src/runtime/model_bus.h
$(BUILD)/test_journal $(BUILD)/test_model_session_input: src/runtime/model_bus.h

$(BUILD)/test_assist_worker: tests/runtime/test_assist_worker.cpp $(ASSIST_WORKER) src/runtime/assist_worker.h $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp src/runtime/core_bridge.h $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -ldl -pthread -lm -o $@

$(BUILD)/test_runtime_assist: tests/adapter/runtime_assist_test.cpp src/runtime/runtime.cpp src/runtime/worker.h $(ASSIST_WORKER) src/runtime/assist_worker.h $(RUNTIME_SUPPORT) $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp $(ADAPTER) src/runtime/loader.cpp $(BUILD)/core_host.o $(STORAGE_HEADERS)
	$(CXX) $(CXX_WARN) $(filter-out %.h src/runtime/runtime.cpp,$^) -ldl -pthread -lrt -lm -o $@

# Sideband receipt is a separate bounded journal input. Keep every fixture that
# compiles the real worker linked against the same production implementation.
$(BUILD)/test_journal $(BUILD)/test_worker_session $(BUILD)/test_model_session_reset $(BUILD)/test_model_session_input $(BUILD)/test_runtime_assist $(BUILD)/test_worker_lds $(BUILD)/test_worker_lds_source $(BUILD)/test_runtime_lds_association: $(LDS_ASSOCIATION) $(LDS_SIDEBAND) $(LDS_HEADERS) $(LDS_REQUEST_SOURCE) src/runtime/lds_request_source.h src/runtime/lds_source_bus.h

$(BUILD)/test_data_patch: tests/adapter/data_patch_test.cpp src/adapter/data_patch.h src/adapter/adapter.h | $(BUILD)
	$(CXX) $(CXX_WARN) $< -o $@
$(BUILD)/test_lds_hooks: tests/adapter/lds_hooks_test.cpp tests/adapter/lds_relay.S $(LDS_HOOKS) $(LDS_SIDEBAND) src/adapter/lds_hooks.h $(LDS_HEADERS) | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -lrt -o $@
$(BUILD)/test_lds_sideband: tests/runtime/test_lds_sideband.cpp $(LDS_SIDEBAND) $(LDS_HEADERS) src/runtime/request_log.h src/runtime/request_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -lrt -o $@
$(BUILD)/test_worker_lds: tests/runtime/test_worker_lds.cpp src/runtime/runtime.cpp src/runtime/worker.h $(ASSIST_WORKER) src/runtime/assist_worker.h $(RUNTIME_SUPPORT) $(LDS_SIDEBAND) $(LDS_HEADERS) $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp $(ADAPTER) src/runtime/loader.cpp $(BUILD)/core_host.o $(STORAGE_HEADERS)
	$(CXX) $(CXX_WARN) $(filter-out %.h src/runtime/runtime.cpp,$^) -ldl -pthread -lrt -lm -o $@
$(BUILD)/test_worker_lds_source: tests/runtime/test_worker_lds_source.cpp src/runtime/runtime.cpp src/runtime/worker.h $(ASSIST_WORKER) src/runtime/assist_worker.h $(RUNTIME_SUPPORT) $(LDS_SIDEBAND) $(LDS_HEADERS) $(LDS_REQUEST_SOURCE) src/runtime/lds_request_source.h $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp $(ADAPTER) src/runtime/loader.cpp $(BUILD)/core_host.o $(STORAGE_HEADERS)
	$(CXX) $(CXX_WARN) $(filter-out %.h src/runtime/runtime.cpp,$^) -ldl -pthread -lrt -lm -o $@
$(BUILD)/test_lds_request_source: tests/runtime/test_lds_request_source.cpp $(LDS_REQUEST_SOURCE) src/runtime/lds_request_source.h $(LDS_SIDEBAND) $(LDS_HEADERS) src/runtime/request_trace.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -lrt -o $@
$(BUILD)/test_lds_source_bus: tests/runtime/test_lds_source_bus.cpp src/runtime/lds_source_bus.h src/runtime/bus_trace.h $(LDS_REQUEST_SOURCE) src/runtime/lds_request_source.h $(LDS_SIDEBAND) $(LDS_HEADERS) src/runtime/request_trace.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -lrt -o $@
$(BUILD)/test_lds_install: tests/adapter/lds_install_test.cpp src/adapter/lds_install.cpp src/adapter/lds_install.h src/adapter/data_patch.h src/adapter/lds_hooks.h $(LDS_HEADERS) | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -ldl -o $@
$(BUILD)/test_lds_tap: tests/sensors/test_lds_tap.cpp src/sensors/lds_tap.cpp $(RUNTIME_SUPPORT) $(LDS_SIDEBAND) $(LDS_ASSOCIATION) src/adapter/lds_install.h src/runtime/loader.h src/runtime/config.h src/runtime/sha256.h $(LDS_HEADERS) | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h src/sensors/lds_tap.cpp,$^) -pthread -lrt -o $@

test-lds: $(BUILD)/test_lds_association_channel $(BUILD)/test_data_patch $(BUILD)/test_lds_hooks $(BUILD)/test_lds_sideband $(BUILD)/test_worker_lds $(BUILD)/test_lds_request_source $(BUILD)/test_lds_source_bus $(BUILD)/test_worker_lds_source $(BUILD)/test_lds_install $(BUILD)/test_lds_tap
	@set -e; for scenario in basic exact capacity invalidation fork stop allocation scalar_bits old_offer loss borrowing exhaustion fd_validation drain_bound malformed_map concurrent retirement_scheduled heading_presence rmc_status protocol_version; do $(BUILD)/test_lds_association_channel $$scenario; done
	$(BUILD)/test_data_patch
	$(BUILD)/test_lds_sideband
	$(BUILD)/test_lds_request_source
	$(BUILD)/test_lds_source_bus
	$(BUILD)/test_lds_install
	@set -e; for scenario in off invalid missing_mode missing_config disabled marker_symlink marker_error observe scrub install_failed rollback_failed cold_lost normal late_receiver sender_failed unrequested null_handle repeated association association_fork association_failed association_invalidate; do $(BUILD)/test_lds_tap $$scenario; done
	@set -e; for scenario in chain prepare inactive register inline retained read_copy snapshot missing_read wrong_pointer lifetime late unwind cancel chain_mismatch endpoint endpoint_post nested_path failed_send failed_build path_unwind all_ids initialize_overlap initialize_unwind locked locked_pair locked_unknown locked_unthreaded locked_null locked_generation locked_mutex locked_native_pc locked_message_pc locked_native_fail locked_no_native locked_native_twice locked_message_twice locked_reply locked_destination locked_endpoint locked_failed_send locked_unwind locked_extra_send locked_inactive locked_clock locked_type locked_zero_request locked_endpoint_changed locked_snapshot locked_nested_send course_values course_bindings course_nested_callback course_boundary_overlap course_identity course_parse_nested course_unwind course_cancel course_open course_close course_reset course_write_boundary course_saved_read course_routes course_callback_parse course_inactive course_one_commit course_copy_twice status_values status_inheritance status_no_commit; do $(BUILD)/test_lds_hooks $$scenario; done
	@set -e; for scenario in capture occupied pre_stopped bounded malformed wrong_uid; do $(BUILD)/test_worker_lds $$scenario; done
	@set -e; for scenario in position_first sideband_first mismatch late_conflict pre_stopped malformed_recovery first_bus startup_bus bus_reconnect; do $(BUILD)/test_worker_lds_source $$scenario; done

$(BUILD)/test_association_context: tests/adapter/association_context_test.cpp src/adapter/adapter.cpp src/adapter/bus_hooks.cpp src/runtime/request_trace.cpp src/adapter/adapter.h $(LDS_HEADERS) | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
$(BUILD)/test_context_pool_association: tests/adapter/context_pool_association_test.cpp src/adapter/adapter.cpp src/adapter/bus_hooks.cpp src/runtime/request_trace.cpp src/adapter/adapter.h $(LDS_HEADERS) | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
$(BUILD)/test_context_pool_atfork_failure: tests/adapter/context_pool_atfork_failure_test.cpp src/adapter/adapter.cpp src/adapter/bus_hooks.cpp src/runtime/request_trace.cpp src/adapter/adapter.h $(LDS_HEADERS) | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -Wl,--wrap=pthread_atfork -pthread -o $@
$(BUILD)/test_runtime_lds_association: tests/adapter/runtime_lds_association_test.cpp tests/runtime/model_bus_fixture.h src/runtime/runtime.cpp src/runtime/worker.h $(ASSIST_WORKER) src/runtime/assist_worker.h $(RUNTIME_SUPPORT) $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp $(ADAPTER) src/runtime/loader.cpp $(BUILD)/core_host.o $(STORAGE_HEADERS)
	$(CXX) $(CXX_WARN) $(filter-out %.h src/runtime/runtime.cpp,$^) -ldl -pthread -lrt -lm -o $@
$(BUILD)/test_lds_association_channel: tests/runtime/test_lds_association_channel.cpp $(LDS_ASSOCIATION) $(LDS_HEADERS) src/adapter/adapter.h src/adapter/lds_hooks.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -Wl,--wrap=pwrite -Wl,--wrap=mmap -pthread -lrt -o $@

# adapter.cpp consumes the shared association protocol version. Host fixtures
# compile sources directly, so record this prerequisite as well as ARM .d files.
HOST_ADAPTER_TESTS = \
    test_adapter test_provenance_context test_context_pool \
    test_journal test_worker_session test_model_session_reset \
    test_model_session_input test_session_hooks test_session_early_init \
    test_session_request test_bus_endpoint test_bus_hooks \
    test_bus_early_init test_assist_publication test_pipeline \
    test_navigation test_live_pipeline test_gyro_bias \
    test_holdout test_beta test_shadow_log test_gps_wheel \
    test_assist_worker test_runtime_assist test_worker_lds \
    test_worker_lds_source test_association_context test_context_pool_association \
    test_context_pool_atfork_failure test_runtime_lds_association
$(addprefix $(BUILD)/,$(HOST_ADAPTER_TESTS)): src/runtime/lds_association_protocol.h
