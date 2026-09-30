#if defined(__arm__) && !defined(__ARM_PCS_VFP)
    .syntax unified
    .arm
    .text
    .align 2
    .global mx5_position_veneer
    .hidden mx5_position_veneer
    .hidden mx5_position_trampoline
    .hidden mx5_position_call
    .type mx5_position_veneer, %function
mx5_position_veneer:
    .fnstart
    // Stock caller passes this/input in r0/r1, no stack arguments. Keep an
    // 8-byte aligned private frame and preserve the original caller's LR.
    .save {r4, lr}
    push {r4, lr}
    .pad #16
    sub sp, sp, #16
    stmia sp, {r0-r3}
    mov r0, sp
    bl mx5_position_call
    // The C++ scope owns cleanup during normal or exceptional return. The
    // original return registers were saved before that cleanup ran.
    ldmia sp, {r0-r3}
    add sp, sp, #16
    pop {r4, pc}
    .fnend
    .size mx5_position_veneer, .-mx5_position_veneer

    // Invoke a verified ARM softfp target with r0-r3 only (NO stack args).
    // Its result is an opaque register set, not a guessed C++ return type.
    .global mx5_arm_invoke
    .hidden mx5_arm_invoke
    .type mx5_arm_invoke, %function
mx5_arm_invoke:
    .fnstart
    .save {r4, lr}
    push {r4, lr}
    mov r4, r0
    mov r12, r1
    ldmia r4, {r0-r3}
    blx r12
    stmia r4, {r0-r3}
    pop {r4, pc}
    .fnend
    .size mx5_arm_invoke, .-mx5_arm_invoke
#endif
    .section .note.GNU-stack,"",%progbits
