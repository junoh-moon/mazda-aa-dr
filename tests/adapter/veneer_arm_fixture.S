    .syntax unified
    .arm
    .text
    .align 2
    .global mx5_test_fake_target
    .type mx5_test_fake_target, %function
mx5_test_fake_target:
    push {r4, lr}
    bl mx5_test_target_body
    mov r0, #0x11
    mov r1, #0x22
    mov r2, #0x33
    mov r3, #0x44
    pop {r4, pc}
    .size mx5_test_fake_target, .-mx5_test_fake_target
    .global mx5_test_call_veneer
    .type mx5_test_call_veneer, %function
mx5_test_call_veneer:
    push {r4, lr}
    mov r4, r2
    bl mx5_position_veneer
    stmia r4, {r0-r3}
    pop {r4, pc}
    .size mx5_test_call_veneer, .-mx5_test_call_veneer
    .section .note.GNU-stack,"",%progbits
