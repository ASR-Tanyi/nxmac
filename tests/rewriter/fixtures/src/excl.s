// Exclusive sequences: ordering, windows that use x18, their exits
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

FN order_all
1:  ldxr    w8, [x0]
    add     w8, w8, #1
    stxr    w9, w8, [x0]
    cbnz    w9, 1b
2:  ldxrb   w8, [x0]
    stxrb   w9, w8, [x0]
    cbnz    w9, 2b
3:  ldxrh   w8, [x0]
    stxrh   w9, w8, [x0]
    cbnz    w9, 3b
4:  ldxp    x8, x10, [x0]
    stxp    w9, x8, x10, [x0]
    cbnz    w9, 4b
5:  ldaxr   x8, [x0]
    stlxr   w9, x8, [x0]
    cbnz    w9, 5b
6:  ldxr    x8, [x0]
    stxr    w9, x8, [x0]
    cbnz    w9, 6b
    ret
END order_all
FN win_simple
1:  ldxr    w18, [x0]
    add     w18, w18, #1
    stxr    w9, w18, [x0]
    cbnz    w9, 1b
    mov     w0, w18
    ret
END win_simple
FN win_branch_out
1:  ldaxr   x18, [x0]
    cmp     x18, x1
    b.ne    2f
    stlxr   w9, x2, [x0]
    cbnz    w9, 1b
    mov     w0, #1
    ret
2:  clrex
    mov     w0, #0
    ret
END win_branch_out
FN win_adrp
1:  ldaxr   x9, [x9]
    cmp     x9, x1
    b.ne    2f
    adrp    x10, excl_data
    add     x10, x10, :lo12:excl_data
    mov     x18, x26
    stlxr   w9, x8, [x10]
    cbnz    w9, 1b
2:  ret
END win_adrp
FN win_adrp18
1:  ldaxr   x9, [x0]
    adrp    x18, excl_data
    add     x18, x18, :lo12:excl_data
    stlxr   w10, x9, [x18]
    cbnz    w10, 1b
    ret
END win_adrp18
FN win_pair
1:  ldaxp   x18, x9, [x0]
    stlxp   w10, x18, x9, [x0]
    cbnz    w10, 1b
    ret
END win_pair
FN win_tb
1:  ldxr    w18, [x0]
    tbnz    w18, #0, 2f
    orr     w18, w18, #1
    stxr    w9, w18, [x0]
    cbnz    w9, 1b
2:  ret
END win_tb
FN win_inner_branch
1:  ldaxr   w8, [x0]
    cbz     w8, 2f
    sub     w18, w8, #1
    b       3f
2:  mov     w18, #7
3:  stlxr   w9, w18, [x0]
    cbnz    w9, 1b
    ret
END win_inner_branch
FN win_no_cbnz
    ldxr    x18, [x0]
    stxr    w9, x18, [x0]
    mov     w0, w9
    ret
END win_no_cbnz
// A retry block that re-enters a window at its store-exclusive
FN win_retry
    ldxr    w17, [x10]
    cmp     w15, w17
    b.ne    2f
1:  stxr    w17, w18, [x10]
    cbnz    w17, 3f
    mov     w0, #1
    ret
2:  mov     w0, #0
    ret
3:  ldxr    w17, [x10]
    cmp     w15, w17
    b.eq    1b
    mov     w0, #0
    ret
END win_retry
// An exclusive load with no store within reach, and exclusive sequences without x18
FN no_window
    ldxr    w18, [x0]
    mov     w1, w18
    ret
END no_window

    .data
    .globl excl_data
excl_data:
    .quad 0, 0
