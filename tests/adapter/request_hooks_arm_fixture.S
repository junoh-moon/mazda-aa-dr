    .syntax unified
    .arm
    .text
    .align 2
    .global request_test_call4
    .type request_test_call4,%function
request_test_call4:
    .fnstart
    .save {r4,lr}
    push {r4,lr}
    mov r12,r0
    mov r4,r1
    ldmia r4,{r0-r3}
    blx r12
    stmia r4,{r0-r3}
    pop {r4,pc}
    .fnend
    .size request_test_call4,.-request_test_call4
    .macro TARGET name, body
    .global \name
    .type \name,%function
\name:
    .fnstart
    .save {r4,lr}
    push {r4,lr}
    bl \body
    mov r0,#0x11
    mov r1,#0x22
    mov r2,#0x33
    mov r3,#0x44
    pop {r4,pc}
    .fnend
    .size \name,.-\name
    .endm
    TARGET request_test_post,request_test_post_body
    TARGET request_test_work,request_test_work_body
    TARGET request_test_destroy,request_test_destroy_body
    .section .note.GNU-stack,"",%progbits
