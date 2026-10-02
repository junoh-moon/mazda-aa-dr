/* Authored call sites: product caller checks see the same fixed return labels
 * on host and ARM. No test-only bypass is compiled into the hooks. */
#if defined(__x86_64__)
.macro RELAY name,target
.text
.globl \name
.type \name,@function
\name:
.cfi_startproc
push %rbp
.cfi_def_cfa_offset 16
.cfi_offset %rbp,-16
mov %rsp,%rbp
.cfi_def_cfa_register %rbp
call \target
.globl \name\()_return
\name\()_return:
pop %rbp
.cfi_def_cfa %rsp,8
ret
.cfi_endproc
.size \name,.-\name
.endm
#elif defined(__arm__)
.syntax unified
.arm
.macro RELAY name,target
.text
.align 2
.global \name
.type \name,%function
\name:
.fnstart
.save {r4,lr}
push {r4,lr}
bl \target
.global \name\()_return
\name\()_return:
pop {r4,pc}
.fnend
.size \name,.-\name
.endm
#else
#error Unsupported focused LDS relay architecture
#endif
RELAY lds_test_read_rmc,mx5_lds_read
RELAY lds_test_read_gga,mx5_lds_read
RELAY lds_test_read_gsa,mx5_lds_read
RELAY lds_test_update_rmc,mx5_lds_update
RELAY lds_test_update_gga,mx5_lds_update
RELAY lds_test_update_gsa,mx5_lds_update
RELAY lds_test_lock_read,mx5_lds_mutex_lock
RELAY lds_test_lock_update,mx5_lds_mutex_lock
RELAY lds_test_lock_service,mx5_lds_mutex_lock
RELAY lds_test_unlock_read,mx5_lds_mutex_unlock
RELAY lds_test_unlock_update,mx5_lds_mutex_unlock
RELAY lds_test_unlock_service,mx5_lds_mutex_unlock
RELAY lds_test_copy_read,mx5_lds_mem_copy
RELAY lds_test_copy_update,mx5_lds_mem_copy
RELAY lds_test_message_lock,mx5_lds_message_lock
RELAY lds_test_native_lock,mx5_lds_native_mutex_lock
RELAY lds_test_parse,mx5_lds_parse_sentence
#if defined(__x86_64__)
RELAY lds_test_checkpoint,mx5_lds_select
.text
.globl lds_test_dispatch
.type lds_test_dispatch,@function
lds_test_dispatch:
.cfi_startproc
push %rbp
.cfi_def_cfa_offset 16
.cfi_offset %rbp,-16
mov %rsp,%rbp
.cfi_def_cfa_register %rbp
mov %rdi,%rax
mov %rsi,%rdi
call *%rax
.globl lds_test_dispatch_return
lds_test_dispatch_return:
pop %rbp
.cfi_def_cfa %rsp,8
ret
.cfi_endproc
.size lds_test_dispatch,.-lds_test_dispatch
#elif defined(__arm__)
.text
.align 2
.global lds_test_checkpoint
.type lds_test_checkpoint,%function
lds_test_checkpoint:
.fnstart
.save {r4,lr}
push {r4,lr}
ldr r4,[sp,#8]
.pad #8
sub sp,sp,#8
str r4,[sp]
bl mx5_lds_select
.global lds_test_checkpoint_return
lds_test_checkpoint_return:
add sp,sp,#8
pop {r4,pc}
.fnend
.size lds_test_checkpoint,.-lds_test_checkpoint
.global lds_test_dispatch
.type lds_test_dispatch,%function
lds_test_dispatch:
.fnstart
.save {r4,lr}
push {r4,lr}
mov ip,r0
mov r0,r1
blx ip
.global lds_test_dispatch_return
lds_test_dispatch_return:
pop {r4,pc}
.fnend
.size lds_test_dispatch,.-lds_test_dispatch
#endif
#if defined(__linux__)
.section .note.GNU-stack,"",%progbits
#endif
