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
RUNTIME_SUPPORT = src/runtime/config.cpp src/runtime/sha256.cpp
HOST_DBUS_FLAGS = $(shell pkg-config --cflags dbus-1)
HOST_DBUS_LIBS = $(shell pkg-config --libs dbus-1)
ARM_PREFIX ?=
ARM_SYSROOT ?=
ARM_FLAGS = -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm
ARM_CPPFLAGS = -Isrc -I$(ARM_SYSROOT)/usr/include/dbus-1.0 -I$(ARM_SYSROOT)/usr/lib/dbus-1.0/include
ARM_CXXFLAGS = -std=c++11 -Os -Wall -Wextra -Werror -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fno-omit-frame-pointer -ftls-model=initial-exec $(ARM_FLAGS)
ARM_SOURCES = src/runtime/loader.cpp $(ADAPTER) $(RUNTIME_SUPPORT) src/runtime/runtime.cpp src/runtime/core_bridge.cpp
ARM_OBJECTS = $(patsubst %.cpp,$(BUILD)/arm/%.o,$(ARM_SOURCES)) $(BUILD)/arm/src/core/dr_core.o $(BUILD)/arm/src/adapter/arm_veneer.o

.PHONY: all test test-loader test-core test-adapter test-runtime test-packaging test-tools test-integration arm clean
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
$(BUILD)/test_journal: $(RUNTIME_SUPPORT) src/runtime/runtime.cpp tests/runtime/test_journal.cpp $(ADAPTER) src/runtime/loader.cpp | $(BUILD)
	$(CXX) $(CXX_WARN) $(HOST_DBUS_FLAGS) $(RUNTIME_SUPPORT) $(ADAPTER) src/runtime/loader.cpp tests/runtime/test_journal.cpp $(HOST_DBUS_LIBS) -ldl -lpthread -lrt -lm -o $@
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

test: test-loader test-core test-adapter test-runtime test-packaging test-tools test-integration

arm: $(BUILD)/libmx5dr.so
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
	$(ARM_PREFIX)g++ -shared $(ARM_FLAGS) -Wl,-z,relro,-z,now,-z,noexecstack,--no-undefined -Wl,-soname,libmx5dr.so -static-libstdc++ -static-libgcc $(ARM_OBJECTS) -ldbus-1 -ldl -lpthread -lrt -lm -o $@
clean:
	rm -rf $(BUILD)
