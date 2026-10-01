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
#if defined(__linux__)
.section .note.GNU-stack,"",%progbits
#endif
