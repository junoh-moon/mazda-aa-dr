#include "adapter.h"

#if defined(__arm__) && !defined(__ARM_PCS_VFP)
// Only the AA DSO installs the position veneer. Keep its ARM entry separate
// from adapter state used by the LDS DSO's real bus invalidation path.
// The ASM caller/invoker carry EHABI unwind records; this C++ frame performs
// scope cleanup for both C++ exceptions and deferred pthread cancellation.
#if !defined(__EXCEPTIONS)
#error "ARM adapter entry requires exception cleanup support"
#endif
extern "C" void* mx5_position_trampoline;
extern "C" void mx5_arm_invoke(uint32_t* registers, void* target);
extern "C" void mx5_position_call(uint32_t* registers) {
    struct PositionScope {
        bool completed;
        PositionScope():completed(false) {}
        ~PositionScope() {
            if(!completed)mx5::adapter::position_aborted();
            mx5::adapter::position_leave();
        }
    } scope;
    mx5::adapter::position_enter(reinterpret_cast<void*>(registers[0]),
                                reinterpret_cast<void*>(registers[1]));
    mx5_arm_invoke(registers,mx5_position_trampoline);
    scope.completed=true;
}
#endif
