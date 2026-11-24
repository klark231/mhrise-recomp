    .text
    .align 2
    .global rise_native_entry
    .hidden rise_native_entry
    .type rise_native_entry, %function
rise_native_entry:
    .cfi_startproc
    sub sp, sp, #384
    .cfi_def_cfa_offset 384
    stp x29, x30, [sp, #336]
    .cfi_offset x29, -48
    .cfi_offset x30, -40
    str x19, [sp, #328]
    .cfi_offset x19, -56
    mov x29, sp
    .cfi_def_cfa_register x29
    stp x0, x1, [sp]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    str x8, [sp, #64]
    stp q0, q1, [sp, #80]
    stp q2, q3, [sp, #112]
    stp q4, q5, [sp, #144]
    stp q6, q7, [sp, #176]
    add x9, sp, #384
    str x9, [sp, #288]
    str x16, [sp, #320]
    mrs x9, nzcv
    mrs x10, fpsr
    mrs x11, fpcr
    stp x9, x10, [sp, #352]
    str x11, [sp, #368]
    mov x0, sp
    bl rise_native_dispatch
    ldp x0, x1, [sp, #208]
    ldp q0, q1, [sp, #224]
    ldp q2, q3, [sp, #256]
    ldr x19, [sp, #328]
    ldp x29, x30, [sp, #336]
    .cfi_def_cfa sp, 384
    add sp, sp, #384
    .cfi_def_cfa_offset 0
    ret
    .cfi_endproc
    .size rise_native_entry, .-rise_native_entry

    .global rise_native_call
    .hidden rise_native_call
    .type rise_native_call, %function
rise_native_call:
    .cfi_startproc
    stp x29, x30, [sp, #-32]!
    .cfi_def_cfa_offset 32
    .cfi_offset x29, -32
    .cfi_offset x30, -24
    stp x19, x20, [sp, #16]
    .cfi_offset x19, -16
    .cfi_offset x20, -8
    mov x29, sp
    .cfi_def_cfa_register x29
    mov x19, x0
    ldr x10, [x19, #304]
    sub sp, sp, x10
    cbz x10, 2f
    ldr x9, [x19, #296]
    mov x12, sp
1:
    ldp x13, x14, [x9], #16
    stp x13, x14, [x12], #16
    subs x10, x10, #16
    b.ne 1b
2:
    ldr x16, [x19, #72]
    ldp q0, q1, [x19, #80]
    ldp q2, q3, [x19, #112]
    ldp q4, q5, [x19, #144]
    ldp q6, q7, [x19, #176]
    ldp x9, x10, [x19, #352]
    ldr x11, [x19, #368]
    msr nzcv, x9
    msr fpsr, x10
    msr fpcr, x11
    ldp x0, x1, [x19]
    ldp x2, x3, [x19, #16]
    ldp x4, x5, [x19, #32]
    ldp x6, x7, [x19, #48]
    ldr x8, [x19, #64]
    blr x16
    stp x0, x1, [x19, #208]
    stp q0, q1, [x19, #224]
    stp q2, q3, [x19, #256]
    mov sp, x29
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    .cfi_def_cfa sp, 0
    ret
    .cfi_endproc
    .size rise_native_call, .-rise_native_call
    .section .note.GNU-stack,"",%progbits
