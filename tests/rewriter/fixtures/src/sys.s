// System instructions and plain x18 sites of every kind
    .text
    .macro FN name
    .globl \name
    .type \name, %function
    .balign 4
\name:
    .cfi_startproc
    .endm
    .macro END name
    .cfi_endproc
    .size \name, . - \name
    .endm

FN sys_svc
    svc     #0x1f
    ret
END sys_svc
FN sys_svc2
    mov     x8, x0
    svc     #0x7
    svc     #0x7f
    ret
END sys_svc2
FN sys_tls
    mrs     x0, tpidr_el0
    mrs     x16, tpidr_el0
    mrs     x17, tpidrro_el0
    mrs     x18, tpidr_el0
    mrs     x18, tpidrro_el0
    mrs     x3, tpidrro_el0
    msr     tpidr_el0, x1
    msr     tpidr_el0, x16
    msr     tpidr_el0, x17
    msr     tpidr_el0, x18
    msr     tpidr_el0, x0
    ret
END sys_tls
FN sys_timers
    mrs     x9, cntpct_el0
    mrs     x18, cntpct_el0
    mrs     x1, cntvct_el0
    mrs     x2, ctr_el0
    mrs     x18, ctr_el0
    mrs     x30, cntpct_el0
    ret
END sys_timers
FN sys_alloca
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, x1
    sub     sp, sp, x17
    mov     x18, sp
    sub     x18, x18, x0
    mov     sp, x18
    sub     sp, sp, x18
    add     sp, sp, x18
    mov     sp, x29
    ldp     x29, x30, [sp], #16
    ret
END sys_alloca
FN plain_sites
    add     x18, x18, #1
    adds    w18, w18, #1
    add     x18, x18, x18
    madd    x18, x18, x18, x18
    stp     x18, x30, [sp, #-32]!
    str     x18, [sp, #16]
    ldr     w18, [x0, x18, lsl #2]
    ldp     x18, x30, [sp], #32
    str     x18, [sp, #-16]!
    ldr     x18, [sp], #16
    stp     x0, x18, [sp, #-96]!
    ldp     x0, x18, [sp], #96
    fmov    d0, x18
    scvtf   s1, w18
    csel    x0, x18, x1, eq
    mov     x0, x18
    mov     x18, x0
    movk    x18, #0x1234, lsl #16
    ldrb    w18, [x18]
    prfm    pldl1keep, [x18]
    cmp     x18, #4
    ccmp    x18, x0, #0, ne
    ret
END plain_sites
FN cond_sites
    cbz     x18, 1f
    cbnz    w18, 2f
    tbz     w18, #3, 1f
    tbnz    x18, #40, 2f
    nop
1:  mov     x0, #1
    ret
2:  mov     x0, #2
    cbz     x18, 1b
    ret
END cond_sites
FN pcrel_sites
    adr     x18, plain_sites
    adrp    x18, sys_data
    add     x18, x18, :lo12:sys_data
    ldr     x0, [x18]
    adrp    x0, sys_data
    ret
END pcrel_sites
FN call_sites
    stp     x29, x30, [sp, #-16]!
    ldr     x18, [x0]
    blr     x18
    mov     x18, x0
    blr     x18
    ldp     x29, x30, [sp], #16
    ret
END call_sites
// Words in the text that are no instructions: literal pools
FN literals
    ldr     x0, 1f
    ret
1:  .quad   0x123456789abcdef0
    .word   0xe7ffdefe
    ret
END literals

    .data
    .globl sys_data
sys_data:
    .quad 0, 1, 2, 3
