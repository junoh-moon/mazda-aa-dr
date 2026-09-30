    .syntax unified
    .arm
    .text
    .align 2
    .global authored_target_entry
    .type authored_target_entry, %function
authored_target_entry:
    .fnstart
    // Same relocatable prologue contract as the installer, followed entirely
    // by authored code. No OEM instructions beyond that public contract.
    .save {r4, fp, lr}
    push {r4, fp, lr}
    add fp, sp, #8
    .pad #28
    sub sp, sp, #28
    bl authored_target
    add sp, sp, #28
    pop {r4, fp, pc}
    .fnend
    .size authored_target_entry, .-authored_target_entry
    .section .note.GNU-stack,"",%progbits
