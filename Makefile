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
ADAPTER = src/adapter/adapter.cpp src/adapter/v74_install.cpp src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp $(REQUEST)
NAVIGATION = src/navigation/pipeline.cpp src/navigation/channel.cpp src/navigation/holdout.cpp
NAV_HEADERS = src/navigation/pipeline.h src/navigation/gyro_bias.h src/navigation/gps_wheel.h src/navigation/holdout.h
SENSOR_TAP = src/sensors/vim_tap.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp src/runtime/config.cpp src/runtime/sha256.cpp
SENSOR_OBJECTS = $(patsubst %.cpp,$(BUILD)/arm/%.o,$(SENSOR_TAP))
RUNTIME_SUPPORT = src/runtime/config.cpp src/runtime/sha256.cpp
HOST_DBUS_FLAGS = $(shell pkg-config --cflags dbus-1)
HOST_DBUS_LIBS = $(shell pkg-config --libs dbus-1)
ARM_PREFIX ?=
ARM_SYSROOT ?=
ARM_FLAGS = -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm
ARM_CPPFLAGS = -Isrc -I$(ARM_SYSROOT)/usr/include/dbus-1.0 -I$(ARM_SYSROOT)/usr/lib/dbus-1.0/include
ARM_CXXFLAGS = -std=c++11 -Os -Wall -Wextra -Werror -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fno-omit-frame-pointer -ftls-model=initial-exec $(ARM_FLAGS)
ARM_DEPFLAGS = -MMD -MP -MF $(@:.o=.d).tmp -MT $@
ARM_SOURCES = src/runtime/loader.cpp $(ADAPTER) $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/core_bridge.cpp $(NAVIGATION)
ARM_OBJECTS = $(patsubst %.cpp,$(BUILD)/arm/%.o,$(ARM_SOURCES)) $(BUILD)/arm/src/core/dr_core.o $(BUILD)/arm/src/adapter/arm_veneer.o $(BUILD)/arm/src/adapter/request_veneer.o
COLLECTOR_OBJECTS = $(BUILD)/arm/src/collector/collector.o $(BUILD)/arm/src/runtime/config.o
GUARD_OBJECTS = $(BUILD)/arm/src/guard/guard.o $(BUILD)/arm/src/runtime/sha256.o
HASH_OBJECTS = $(BUILD)/arm/src/tools/sha256_main.o $(BUILD)/arm/src/runtime/sha256.o
ALL_ARM_OBJECTS = $(sort $(ARM_OBJECTS) $(SENSOR_OBJECTS) $(COLLECTOR_OBJECTS) $(GUARD_OBJECTS) $(HASH_OBJECTS))

.PHONY: all test test-build-deps test-motion-journal test-recovery test-loader test-core test-adapter test-runtime test-request-publication test-journal-boundaries test-collector test-packaging test-tools test-integration test-navigation test-sensors arm clean
all: test
$(BUILD):
	mkdir -p $@
$(BUILD)/test_core: $(CORE) src/core/dr_core.h tests/core/test_core.c | $(BUILD)
	$(CC) $(C_WARN) -Isrc/core $(CORE) tests/core/test_core.c -lm -o $@
$(BUILD)/replay: $(CORE) src/core/dr_core.h tests/core/replay.c | $(BUILD)
	$(CC) $(C_WARN) -Isrc/core $(CORE) tests/core/replay.c -lm -o $@
$(BUILD)/test_adapter: $(ADAPTER) src/adapter/adapter.h tests/adapter/adapter_test.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(ADAPTER) tests/adapter/adapter_test.cpp -ldl -pthread -o $@
$(BUILD)/test_runtime: $(RUNTIME_SUPPORT) tests/runtime/test_runtime.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(RUNTIME_SUPPORT) tests/runtime/test_runtime.cpp -o $@
$(BUILD)/test_request_trace: src/runtime/request_trace.cpp src/runtime/request_trace.h tests/runtime/test_request_trace.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) src/runtime/request_trace.cpp tests/runtime/test_request_trace.cpp -pthread -o $@
$(BUILD)/test_model_session: tests/runtime/test_model_session.cpp src/runtime/model_bus.h src/runtime/model_session.h src/runtime/session_trace.h src/adapter/adapter.h src/runtime/request_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $< -o $@
$(BUILD)/test_request_status: src/runtime/request_trace.cpp src/runtime/request_trace.h tests/runtime/test_request_status.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) src/runtime/request_trace.cpp tests/runtime/test_request_status.cpp -pthread -o $@
$(BUILD)/test_request_observer: src/runtime/request_trace.cpp src/runtime/request_trace.h src/runtime/request_observer.cpp src/runtime/request_observer.h tests/runtime/test_request_observer.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
$(BUILD)/test_request_handoff: tests/runtime/test_request_handoff.cpp src/runtime/request_trace.cpp src/runtime/request_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $< -pthread -o $@
$(BUILD)/test_journal_queue: tests/runtime/test_journal_queue.cpp src/runtime/journal_queue.h src/adapter/adapter.h src/runtime/request_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $< -pthread -o $@
$(BUILD)/test_journal: $(BUILD)/core_host.o $(NAVIGATION) src/runtime/core_bridge.cpp $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/motion_batch.h tests/runtime/test_journal.cpp $(ADAPTER) src/runtime/loader.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(RUNTIME_SUPPORT) $(ADAPTER) $(NAVIGATION) src/runtime/core_bridge.cpp $(BUILD)/core_host.o src/runtime/loader.cpp tests/runtime/test_journal.cpp -ldl -lpthread -lrt -lm -o $@
$(BUILD)/test_worker_session: $(BUILD)/core_host.o $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/model_session.h src/runtime/session_trace.h tests/runtime/test_worker_session.cpp $(ADAPTER) src/runtime/loader.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(RUNTIME_SUPPORT) $(ADAPTER) $(NAVIGATION) src/runtime/core_bridge.cpp $(BUILD)/core_host.o src/runtime/loader.cpp tests/runtime/test_worker_session.cpp -ldl -lpthread -lrt -lm -o $@
$(BUILD)/test_model_session_reset: $(BUILD)/core_host.o $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/model_session.h src/runtime/session_trace.h tests/runtime/test_model_session_reset.cpp $(ADAPTER) src/runtime/loader.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(RUNTIME_SUPPORT) $(ADAPTER) $(NAVIGATION) src/runtime/core_bridge.cpp $(BUILD)/core_host.o src/runtime/loader.cpp tests/runtime/test_model_session_reset.cpp -ldl -lpthread -lrt -lm -o $@
$(BUILD)/test_model_session_input: $(BUILD)/core_host.o $(NAVIGATION) $(NAV_HEADERS) src/runtime/core_bridge.cpp $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/model_session.h src/runtime/session_trace.h tests/runtime/test_model_session_input.cpp $(ADAPTER) src/runtime/loader.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(RUNTIME_SUPPORT) $(ADAPTER) $(NAVIGATION) src/runtime/core_bridge.cpp $(BUILD)/core_host.o src/runtime/loader.cpp tests/runtime/test_model_session_input.cpp -ldl -lpthread -lrt -lm -o $@
$(BUILD)/mx5dr-collector-host: src/collector/collector.cpp src/runtime/config.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(HOST_DBUS_FLAGS) $^ $(HOST_DBUS_LIBS) -lpthread -lrt -o $@
$(BUILD)/test_collector: src/collector/collector.cpp src/runtime/config.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) -DMX5_COLLECTOR_TESTING $(HOST_DBUS_FLAGS) $^ $(HOST_DBUS_LIBS) -lpthread -lrt -o $@
$(BUILD)/test_collector_journal: tests/collector/test_journal.cpp src/collector/collector.cpp src/runtime/config.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(HOST_DBUS_FLAGS) tests/collector/test_journal.cpp src/runtime/config.cpp $(HOST_DBUS_LIBS) -lpthread -lrt -o $@
test-collector: $(BUILD)/test_collector $(BUILD)/mx5dr-collector-host $(BUILD)/test_collector_journal $(BUILD)/test_journal $(BUILD)/test_adapter
	$(BUILD)/test_collector_journal
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
$(BUILD)/test_bus_hooks: tests/adapter/bus_hooks_test.cpp src/adapter/bus_hooks.cpp src/adapter/adapter.cpp src/adapter/bus_hooks.h src/adapter/adapter.h src/runtime/request_trace.h src/runtime/session_trace.h src/runtime/bus_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
$(BUILD)/test_bus_early_init: tests/adapter/bus_early_init_test.cpp src/adapter/bus_hooks.cpp src/adapter/adapter.cpp src/adapter/bus_hooks.h src/adapter/adapter.h src/runtime/request_trace.h src/runtime/session_trace.h src/runtime/bus_trace.h | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -pthread -o $@
test-adapter: $(BUILD)/test_bus_hooks $(BUILD)/test_bus_early_init $(BUILD)/test_adapter $(BUILD)/test_cold_patch $(BUILD)/test_session_hooks $(BUILD)/test_session_early_init $(BUILD)/test_session_request
	@set -e; for case in observe scrub native malformed nested assist epoch reacquire expiry encoder backend request; do $(BUILD)/test_adapter $$case; done
	$(BUILD)/test_cold_patch
	@set -e; for case in normal failure overlap same_storage closing_create creating_during_destroy null_success output_race late_destroy distinct_storage capacity callback_bad callback_null readers throw_create throw_destroy throw_status cancel_create cancel_destroy cancel_status prediction_destroy prediction_recreate prediction_create_failure prediction_destroy_failure prediction_status prediction_create_inflight prediction_destroy_inflight prediction_status_inflight prediction_cached_inflight; do result=0; $(BUILD)/test_session_hooks $$case || result=$$?; [ "$$result" -eq 0 ] || { [ "$$result" -eq 77 ] && [ "$$(uname -s)" = Darwin ]; }; done
	$(BUILD)/test_session_early_init
	$(BUILD)/test_session_request
	$(BUILD)/test_session_request bus_recreated
	$(BUILD)/test_bus_early_init
	@set -e; for case in normal position_source signal signal_reuse failure early_close unobserved overlap cancel readers capacity collision bad_callback throw_create throw_connect throw_disconnect throw_free throw_closed prediction_entry_create prediction_entry_connect prediction_entry_disconnect prediction_entry_free prediction_entry_closed prediction_entry_signal prediction_exit_create prediction_exit_connect prediction_exit_disconnect prediction_exit_free prediction_exit_closed prediction_exit_signal; do result=0; $(BUILD)/test_bus_hooks $$case || result=$$?; [ "$$result" -eq 0 ] || { [ "$$result" -eq 77 ] && [ "$$(uname -s)" = Darwin ]; }; done
test-runtime: $(BUILD)/test_runtime $(BUILD)/test_request_trace $(BUILD)/test_request_status $(BUILD)/test_request_observer $(BUILD)/test_request_handoff $(BUILD)/test_journal_queue $(BUILD)/test_journal $(BUILD)/test_model_session $(BUILD)/test_model_session_reset $(BUILD)/test_model_session_input $(BUILD)/test_worker_session test-request-publication test-journal-boundaries
	$(BUILD)/test_runtime
	$(BUILD)/test_request_trace
	$(BUILD)/test_request_status
	$(BUILD)/test_request_observer
	$(BUILD)/test_request_handoff
	$(BUILD)/test_journal_queue
	$(BUILD)/test_journal
	$(BUILD)/test_model_session
	$(BUILD)/test_model_session_reset
	$(BUILD)/test_model_session_reset bus
	$(BUILD)/test_model_session_input
	@set -e; for case in destroy recreate status failed_create ambiguous inflight bus_disconnect bus_reconnect bus_reuse bus_closed bus_signal bus_ambiguous bus_inflight; do $(BUILD)/test_worker_session $$case; done
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
test-integration: $(BUILD)/test_pipeline
	$(BUILD)/test_pipeline
$(BUILD)/core_host.o: $(CORE) src/core/dr_core.h | $(BUILD)
	$(CC) $(C_WARN) -c $(CORE) -o $@
$(BUILD)/test_pipeline: $(BUILD)/core_host.o src/runtime/core_bridge.cpp tests/integration/test_pipeline.cpp $(ADAPTER)
	$(CXX) $(CXX_WARN) src/runtime/core_bridge.cpp tests/integration/test_pipeline.cpp $(ADAPTER) $(BUILD)/core_host.o -lm -ldl -pthread -o $@
test-loader:
	$(PYTHON) tests/runtime/test_loader_interposer.py

test-recovery:
	$(PYTHON) tests/recovery/test_guard.py

test-navigation: $(BUILD)/test_navigation $(BUILD)/test_channel $(BUILD)/test_live_pipeline $(BUILD)/test_gyro_bias $(BUILD)/test_gps_wheel $(BUILD)/test_holdout
	$(BUILD)/test_navigation
	$(BUILD)/test_live_pipeline
	$(BUILD)/test_gyro_bias
	$(BUILD)/test_gps_wheel
	$(BUILD)/test_holdout
	@$(BUILD)/test_channel; result=$$?; test $$result -eq 0 -o $$result -eq 77
$(BUILD)/test_navigation: tests/navigation/test_navigation.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@
$(BUILD)/test_channel: tests/navigation/test_channel.cpp src/navigation/channel.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $^ -pthread -o $@

test-sensors: $(BUILD)/test_vim_source $(BUILD)/test_vim_tap
	$(BUILD)/test_vim_source
	$(BUILD)/test_vim_tap
$(BUILD)/test_vim_source: tests/sensors/test_vim_source.cpp src/sensors/vim_source.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $^ -o $@

test: test-build-deps test-motion-journal test-sensors test-navigation test-recovery test-loader test-core test-adapter test-runtime test-collector test-packaging test-tools test-integration

$(BUILD)/test_motion_batch: tests/runtime/test_motion_batch.cpp src/runtime/motion_batch.h | $(BUILD)
	$(CXX) $(CXX_WARN) $< -o $@
test-motion-journal: $(BUILD)/test_motion_batch $(BUILD)/test_shadow_log $(BUILD)/test_journal
	$(BUILD)/test_motion_batch
	$(BUILD)/test_shadow_log
	MX5DR_MOTION_FIXTURE=$(abspath $(BUILD)/test_motion_batch) MX5DR_SHADOW_FIXTURE=$(abspath $(BUILD)/test_shadow_log) MX5DR_JOURNAL_FIXTURE=$(abspath $(BUILD)/test_journal) $(PYTHON) -m unittest discover -s tests/journal -v

$(BUILD)/arm/src/runtime/runtime.o: src/runtime/motion_batch.h

# Only wrappers that surround OEM calls need C++ cleanup/unwind tables.
# A caller's exception or deferred cancellation must cross the shim intact.
$(BUILD)/arm/src/adapter/adapter.o $(BUILD)/arm/src/adapter/request_hooks.o $(BUILD)/arm/src/adapter/bus_hooks.o $(BUILD)/arm/src/adapter/session_hooks.o $(BUILD)/arm/src/runtime/request_observer.o: override ARM_CXXFLAGS += -fexceptions

arm: $(BUILD)/libmx5dr-vimtap.so $(BUILD)/libmx5dr.so $(BUILD)/mx5dr-collector $(BUILD)/mx5dr-guard $(BUILD)/mx5dr-sha256
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
	$(ARM_PREFIX)g++ -shared $(ARM_FLAGS) -Wl,-z,relro,-z,now,-z,noexecstack,--no-undefined -Wl,-soname,libmx5dr-vimtap.so -static-libstdc++ -static-libgcc $^ -ldl -lpthread -lrt -o $@

$(BUILD)/test_vim_tap: tests/sensors/test_vim_tap.cpp src/sensors/vim_tap.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp $(RUNTIME_SUPPORT) | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out src/sensors/vim_tap.cpp,$^) -ldl -pthread -lrt -o $@

$(BUILD)/test_live_pipeline: tests/navigation/test_live_pipeline.cpp src/sensors/vim_source.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@

$(BUILD)/test_gyro_bias: tests/navigation/test_gyro_bias.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@
$(BUILD)/test_holdout: tests/navigation/test_holdout.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@
$(BUILD)/test_shadow_log: tests/runtime/test_shadow_log.cpp src/runtime/shadow_log.h $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $(filter-out %.h,$^) -lm -ldl -pthread -o $@

$(BUILD)/test_navigation $(BUILD)/test_live_pipeline $(BUILD)/test_journal $(BUILD)/test_gyro_bias $(BUILD)/test_gps_wheel $(BUILD)/test_holdout $(BUILD)/test_shadow_log: $(NAV_HEADERS)
$(BUILD)/test_journal $(BUILD)/arm/src/runtime/runtime.o: src/runtime/shadow_log.h
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
$(BUILD)/test_adapter $(BUILD)/test_pipeline $(BUILD)/test_navigation $(BUILD)/test_live_pipeline $(BUILD)/test_journal $(BUILD)/test_gyro_bias $(BUILD)/test_gps_wheel $(BUILD)/test_holdout $(BUILD)/test_shadow_log $(BUILD)/test_worker_session $(BUILD)/test_request_trace $(BUILD)/test_request_status $(BUILD)/test_request_observer $(BUILD)/test_request_handoff $(BUILD)/test_journal_queue $(BUILD)/test_model_session $(BUILD)/test_session_hooks $(BUILD)/test_session_early_init $(BUILD)/test_session_request $(BUILD)/test_model_session_reset $(BUILD)/test_model_session_input: src/adapter/bus_hooks.h src/runtime/bus_trace.h

$(BUILD)/test_worker_session $(BUILD)/test_model_session_reset: tests/runtime/model_bus_fixture.h src/runtime/model_bus.h
$(BUILD)/test_journal $(BUILD)/test_model_session_input: src/runtime/model_bus.h
