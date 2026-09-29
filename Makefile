# Host checks are independent of the CMU. ARM_PREFIX must select a compatible
# ARMv7 softfp toolchain and sysroot, not the host compiler.
CC ?= cc
CXX ?= c++
PYTHON ?= python3
BUILD ?= build
C_WARN = -std=c99 -O2 -Wall -Wextra -Werror -pedantic
CXX_WARN = -std=c++11 -O2 -Wall -Wextra -Werror -Isrc
CORE = src/core/dr_core.c
ADAPTER = src/adapter/adapter.cpp src/adapter/v74_install.cpp
NAVIGATION = src/navigation/pipeline.cpp src/navigation/channel.cpp
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
ARM_SOURCES = src/runtime/loader.cpp $(ADAPTER) $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/core_bridge.cpp $(NAVIGATION)
ARM_OBJECTS = $(patsubst %.cpp,$(BUILD)/arm/%.o,$(ARM_SOURCES)) $(BUILD)/arm/src/core/dr_core.o $(BUILD)/arm/src/adapter/arm_veneer.o

.PHONY: all test test-recovery test-loader test-core test-adapter test-runtime test-collector test-packaging test-tools test-integration test-navigation test-sensors arm clean
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
$(BUILD)/test_journal: $(BUILD)/core_host.o $(NAVIGATION) src/runtime/core_bridge.cpp $(RUNTIME_SUPPORT) src/runtime/runtime.cpp tests/runtime/test_journal.cpp $(ADAPTER) src/runtime/loader.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(RUNTIME_SUPPORT) $(ADAPTER) $(NAVIGATION) src/runtime/core_bridge.cpp $(BUILD)/core_host.o src/runtime/loader.cpp tests/runtime/test_journal.cpp -ldl -lpthread -lrt -lm -o $@
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
test-adapter: $(BUILD)/test_adapter
	@set -e; for case in observe scrub native malformed nested assist epoch reacquire expiry encoder backend; do $(BUILD)/test_adapter $$case; done
test-runtime: $(BUILD)/test_runtime $(BUILD)/test_journal
	$(BUILD)/test_runtime
	$(BUILD)/test_journal
test-packaging:
	$(PYTHON) -m unittest discover -s tests/packaging -v
test-tools:
	$(PYTHON) -m unittest discover -s tests/tools -v
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

test-navigation: $(BUILD)/test_navigation $(BUILD)/test_channel $(BUILD)/test_live_pipeline
	$(BUILD)/test_navigation
	$(BUILD)/test_live_pipeline
	@$(BUILD)/test_channel; result=$$?; test $$result -eq 0 -o $$result -eq 77
$(BUILD)/test_navigation: tests/navigation/test_navigation.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $^ -lm -ldl -pthread -o $@
$(BUILD)/test_channel: tests/navigation/test_channel.cpp src/navigation/channel.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $^ -o $@

test-sensors: $(BUILD)/test_vim_source $(BUILD)/test_vim_tap
	$(BUILD)/test_vim_source
	$(BUILD)/test_vim_tap
$(BUILD)/test_vim_source: tests/sensors/test_vim_source.cpp src/sensors/vim_source.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $^ -o $@

test: test-sensors test-navigation test-recovery test-loader test-core test-adapter test-runtime test-collector test-packaging test-tools test-integration

arm: $(BUILD)/libmx5dr-vimtap.so $(BUILD)/libmx5dr.so $(BUILD)/mx5dr-collector $(BUILD)/mx5dr-guard
$(BUILD)/arm/%.o: %.cpp
	@test -n "$(ARM_PREFIX)" -a -n "$(ARM_SYSROOT)" || { echo 'Set ARM_PREFIX and ARM_SYSROOT'; exit 1; }
	mkdir -p $(dir $@)
	$(ARM_PREFIX)g++ $(ARM_CXXFLAGS) $(ARM_CPPFLAGS) -c $< -o $@
$(BUILD)/arm/%.o: %.c
	mkdir -p $(dir $@)
	$(ARM_PREFIX)gcc -std=c99 -Os -Wall -Wextra -Werror -pedantic -fPIC -fvisibility=hidden $(ARM_FLAGS) -c $< -o $@
$(BUILD)/arm/%.o: %.S
	mkdir -p $(dir $@)
	$(ARM_PREFIX)gcc -fPIC $(ARM_FLAGS) -c $< -o $@
$(BUILD)/libmx5dr.so: $(ARM_OBJECTS)
	$(ARM_PREFIX)g++ -shared $(ARM_FLAGS) -Wl,-z,relro,-z,now,-z,noexecstack,--no-undefined -Wl,-soname,libmx5dr.so -static-libstdc++ -static-libgcc $(ARM_OBJECTS) -ldl -lpthread -lrt -lm -o $@
$(BUILD)/mx5dr-collector: $(BUILD)/arm/src/collector/collector.o $(BUILD)/arm/src/runtime/config.o
	$(ARM_PREFIX)g++ $(ARM_FLAGS) -Wl,-z,relro,-z,now,-z,noexecstack,--no-undefined -static-libstdc++ -static-libgcc $^ -ldbus-1 -lpthread -lrt -o $@
clean:
	rm -rf $(BUILD)

$(BUILD)/mx5dr-guard: src/guard/guard.cpp src/runtime/sha256.cpp | $(BUILD)
	@test -n "$(ARM_PREFIX)" -a -n "$(ARM_SYSROOT)" || { echo "Set ARM_PREFIX and ARM_SYSROOT"; exit 1; }
	$(ARM_PREFIX)g++ $(ARM_CXXFLAGS) -Wl,-z,relro,-z,now,-z,noexecstack -static-libstdc++ -static-libgcc $^ -o $@

$(BUILD)/libmx5dr-vimtap.so: $(SENSOR_OBJECTS)
	$(ARM_PREFIX)g++ -shared $(ARM_FLAGS) -Wl,-z,relro,-z,now,-z,noexecstack,--no-undefined -Wl,-soname,libmx5dr-vimtap.so -static-libstdc++ -static-libgcc $^ -ldl -lpthread -lrt -o $@

$(BUILD)/test_vim_tap: tests/sensors/test_vim_tap.cpp src/sensors/vim_tap.cpp src/sensors/vim_source.cpp src/navigation/channel.cpp $(RUNTIME_SUPPORT) | $(BUILD)
	$(CXX) $(CXX_WARN) $(filter-out src/sensors/vim_tap.cpp,$^) -ldl -pthread -lrt -o $@

$(BUILD)/test_live_pipeline: tests/navigation/test_live_pipeline.cpp src/sensors/vim_source.cpp $(NAVIGATION) src/runtime/core_bridge.cpp $(ADAPTER) $(BUILD)/core_host.o
	$(CXX) $(CXX_WARN) $^ -lm -ldl -pthread -o $@
