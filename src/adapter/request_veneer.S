#if defined(__arm__) && !defined(__ARM_PCS_VFP)
    .syntax unified
    .arm
    .text
    .align 2
    .macro REQUEST_TAIL name, helper, target
    .global \name
    .hidden \name
    .hidden \helper
    .hidden \target
    .type \name, %function
\name:
    .fnstart
    .save {lr}
    .pad #20
    push {r0-r3,r12,lr}
    bl \helper
    pop {r0-r3,r12,lr}
    ldr r12,.Ldelta\@
.Lpc\@:
    add r12,pc,r12
    ldr r12,[r12]
    bx r12
    .align 2
.Ldelta\@:
    .word \target-(.Lpc\@+8)
    .fnend
    .size \name,.-\name
    .endm
    REQUEST_TAIL mx5_request_post_veneer,mx5_request_post_enter,mx5_request_post_trampoline
    REQUEST_TAIL mx5_request_destroy_veneer,mx5_request_destroy_enter,mx5_request_destroy_trampoline

    .global mx5_request_work_veneer
    .hidden mx5_request_work_veneer
    .hidden mx5_request_work_call
    .type mx5_request_work_veneer, %function
mx5_request_work_veneer:
    .fnstart
    .save {r4,lr}
    push {r4,lr}
    .pad #16
    sub sp,sp,#16
    stmia sp,{r0-r3}
    mov r0,sp
    bl mx5_request_work_call
    ldmia sp,{r0-r3}
    add sp,sp,#16
    pop {r4,pc}
    .fnend
    .size mx5_request_work_veneer,.-mx5_request_work_veneer
#endif
    .section .note.GNU-stack,"",%progbits
