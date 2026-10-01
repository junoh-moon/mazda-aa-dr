// Authored call site with a stable return PC; exercises the actual wrapper's
// caller check on host and the pinned ARM ABI without a production test bypass.
#if defined(__arm__)
    .syntax unified
    .arm
    .text
    .global endpoint_test_register_call
    .type endpoint_test_register_call,%function
endpoint_test_register_call:
    .fnstart
    .save {r4,lr}
    push {r4,lr}
    ldr r3,.Lregister_target
    ldr r3,[r3]
    blx r3
    .global endpoint_test_register_return
endpoint_test_register_return:
    pop {r4,pc}
    .align 2
.Lregister_target:
    .word endpoint_test_register_target
    .fnend
    .size endpoint_test_register_call,.-endpoint_test_register_call
#elif defined(__x86_64__)
    .text
    .global endpoint_test_register_call
    .type endpoint_test_register_call,@function
endpoint_test_register_call:
    .cfi_startproc
    push %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp,-16
    mov %rsp,%rbp
    .cfi_def_cfa_register %rbp
    call *endpoint_test_register_target(%rip)
    .global endpoint_test_register_return
endpoint_test_register_return:
    pop %rbp
    .cfi_def_cfa %rsp,8
    ret
    .cfi_endproc
    .size endpoint_test_register_call,.-endpoint_test_register_call
#else
#error Unsupported endpoint fixture architecture
#endif
    .section .note.GNU-stack,"",%progbits
