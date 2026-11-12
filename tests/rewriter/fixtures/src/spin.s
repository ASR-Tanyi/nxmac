// Spin waits and loops that are none
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

FN spin_load
1:  ldr     w8, [x0]
    cbz     w8, 1b
    ret
END spin_load
FN spin_flags
1:  ldrb    w8, [x0, #4]
    cmp     w8, #2
    b.ne    1b
    ret
END spin_flags
FN spin_two_edges
1:  ldr     w8, [x0]
    tbnz    w8, #0, 1b
    ldr     w9, [x0, #4]
    cmp     w9, w1
    b.eq    1b
    ret
END spin_two_edges
FN spin_b
1:  ldr     x8, [x0, #8]
    cbnz    x8, 2f
    yield
    b       1b
2:  ret
END spin_b
// A counter: no spin wait
FN count_loop
1:  ldr     w8, [x0]
    add     w1, w1, #1
    cbz     w8, 1b
    ret
END count_loop
// A call inside: none
FN call_loop
    stp     x29, x30, [sp, #-16]!
1:  bl      spin_load
    cbz     w0, 1b
    ldp     x29, x30, [sp], #16
    ret
END call_loop
// An exclusive add that every path back undoes
FN spin_rmw
1:  ldaxr   w8, [x0]
    add     w9, w8, #1
    stlxr   w10, w9, [x0]
    cbnz    w10, 1b
    ldr     w11, [x1]
    cbnz    w11, 3f
2:  ldaxr   w8, [x0]
    sub     w9, w8, #1
    stlxr   w10, w9, [x0]
    cbnz    w10, 2b
    b       1b
3:  ret
END spin_rmw
// A spin lock taken and released on every pass
FN spin_lock
1:  ldaxr   w8, [x19]
    cbz     w8, 2f
    clrex
    b       1b
2:  stlxr   w9, w20, [x19]
    cbnz    w9, 1b
    ldr     w10, [x21]
    cbnz    w10, 4f
    stlr    wzr, [x19]
    b       1b
4:  stlr    wzr, [x19]
    ret
END spin_lock
// A test loop inside a region, and a store of a fixed value
FN spin_store
1:  ldr     w8, [x0]
    cbnz    w8, 3f
    str     wzr, [x1, #8]
2:  ldrb    w9, [x2]
    cmp     w9, #1
    b.eq    2b
    b       1b
3:  ret
END spin_store
// A store whose value changes each pass: none
FN store_loop
1:  ldr     w8, [x0]
    add     w3, w3, #1
    str     w3, [x1]
    cbz     w8, 1b
    ret
END store_loop
// x18 in a polling loop: its sites are rewrite sites, so no region
FN spin_x18
1:  ldr     w18, [x0]
    cbz     w18, 1b
    ret
END spin_x18
FN spin_adr
1:  adrp    x8, spin_flag
    ldr     w8, [x8, :lo12:spin_flag]
    adr     x9, spin_adr
    cbz     w8, 1b
    ret
END spin_adr
// The completer of Trinity's rendezvous
FN trinity
    stp     x29, x30, [sp, #-0x20]!
    stp     x20, x19, [sp, #0x10]
    mov     x29, sp
    ldr     x8, [x0, #0x28]
    mov     w9, #1
    strb    w9, [x8, #0x40]
    ldrb    w9, [x8, #0x41]
    cbz     w9, 1f
    ldr     x20, [x8, #0x10]
    add     x9, x20, #0x8
    ldar    w8, [x9]
    cmp     w8, #2
    b.ne    1f
    nop
1:  ldp     x20, x19, [sp, #0x10]
    ldp     x29, x30, [sp], #0x20
    ret
END trinity

    .data
    .globl spin_flag
spin_flag:
    .word 0
