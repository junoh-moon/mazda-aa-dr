#if defined(__arm__) && !defined(__ARM_PCS_VFP)
    .syntax unified
    .arm
    .text
    .align 2
    .global mx5_position_veneer
    .hidden mx5_position_veneer
    .hidden mx5_position_trampoline
    .hidden mx5_position_enter
    .hidden mx5_position_leave
    .type mx5_position_veneer, %function
mx5_position_veneer:
    // Stock caller passes this/input in r0/r1, no stack arguments. Keep an
    // 8-byte aligned private frame and preserve the original caller's LR.
    push {r4, lr}
    push {r0-r3, r12, lr}
    bl mx5_position_enter
    pop {r0-r3, r12, lr}
    ldr r12, .Ltrampoline_delta
.Ltrampoline_pc:
    add r12, pc, r12
    ldr r12, [r12]
    blx r12
    // No guessed C++ return type: keep all soft-float return registers intact.
    push {r0-r3, r12, lr}
    bl mx5_position_leave
    pop {r0-r3, r12, lr}
    pop {r4, pc}
    .align 2
.Ltrampoline_delta:
    .word mx5_position_trampoline - (.Ltrampoline_pc + 8)
    .size mx5_position_veneer, .-mx5_position_veneer
#endif
    .section .note.GNU-stack,"",%progbits
