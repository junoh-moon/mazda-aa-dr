// ARM-only executable: exercises the actual assembler veneer, TLS correlation,
// argument/return-register preservation and errno around a fake OEM target.
#include "adapter/adapter.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
using namespace mx5::adapter;
extern "C" void* mx5_position_trampoline;
extern "C" void mx5_test_fake_target();
extern "C" void mx5_test_call_veneer(void*, void*, uint32_t*);
static void* expected_this;
static void* expected_input;
static unsigned sends, observations, target_calls, target_depth;
static bool exercise_nested;
static VehicleData* original;
static uint8_t original_payload[48];
static int32_t next(void* owner, VehicleData* data) {
    assert(owner == expected_this && errno == 42);
    assert(data->type == 1 && data->length == 48);
    assert((data == original) == (target_depth == 2));
    const uint8_t* bytes = static_cast<const uint8_t*>(data->payload);
    assert(bytes[32] == (target_depth == 2 ? 0x55 : 0));
    assert(bytes[40] == (target_depth == 2 ? 0x55 : 0));
    assert(original_payload[32] == 0x55 && original_payload[40] == 0x55);
    ++sends; errno = 21; return 123;
}
static void sink(const Observation*, void*) { ++observations; errno = 99; }
extern "C" void mx5_test_target_body(void* owner, void* input) {
    assert(owner == expected_this && input == expected_input && errno == 42);
    ++target_calls; ++target_depth;
    if (exercise_nested && target_depth == 1) {
        uint32_t returns[4] = {};
        mx5_test_call_veneer(owner, input, returns);
        assert(returns[0] == 0x11 && returns[1] == 0x22);
        assert(returns[2] == 0x33 && returns[3] == 0x44);
        assert(errno == 21); errno = 42;
    }
    VehicleData data = {1, original_payload, 48}; original = &data;
    assert(send_vehicle_data(owner, &data) == 123);
    assert(errno == 21);
    --target_depth;
}
int main() {
    uint8_t position[72] = {};
    uint32_t returns[4] = {};
    int owner = 7; expected_this = &owner; expected_input = position;
    std::memset(original_payload, 0x55, sizeof original_payload);
    Options options = Options(); options.sink = sink;
    assert(configure(next, options)); assert(set_mode(SCRUB_STALE));
    mx5_position_trampoline = reinterpret_cast<void*>(&mx5_test_fake_target);
    errno = 42;
    mx5_test_call_veneer(&owner, position, returns);
    assert(returns[0] == 0x11 && returns[1] == 0x22 && returns[2] == 0x33 && returns[3] == 0x44);
    assert(sends == 1 && target_calls == 1 && observations == 2 && errno == 21);
    exercise_nested = true; errno = 42;
    mx5_test_call_veneer(&owner, position, returns);
    assert(sends == 3 && target_calls == 3 && observations == 6 && errno == 21);
    exercise_nested = false; errno = 42;
    mx5_test_call_veneer(&owner, position, returns);
    assert(sends == 4 && target_calls == 4 && observations == 8 && errno == 21);
    std::puts("PASS ARM veneer arguments, nested TLS correlation, r0-r3 return and errno");
}
