// Jump tables through x18, and x18 as an indirect tail call
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

// A byte table: target = L + 4 * entry
FN jt_bytes
    cmp     w16, #3
    b.hi    9f
    adrp    x17, jt_bytes_tab
    add     x17, x17, :lo12:jt_bytes_tab
    adr     x18, 1f
    ldrb    w0, [x17, x16]
    add     x18, x18, x0, lsl #2
    br      x18
1:  mov     w0, #10
    ret
    mov     w0, #11
    ret
    mov     w0, #12
    ret
    mov     w0, #13
    ret
9:  mov     w0, #0
    ret
END jt_bytes
// A halfword table whose size the bounds check gives
FN jt_halfs
    cmp     w16, #3
    b.hi    9f
    adrp    x17, jt_halfs_tab
    add     x17, x17, :lo12:jt_halfs_tab
    ldrh    w0, [x17, w16, uxtw #1]
    adr     x18, 1f
    add     x18, x18, x0, lsl #2
    br      x18
1:  mov     w0, #10
    ret
    mov     w0, #11
    ret
    mov     w0, #12
    ret
    mov     w0, #13
    ret
9:  mov     w0, #0
    ret
END jt_halfs
// The offset-table form
FN jt_offsets
    cmp     w18, #4
    b.hi    9f
    adrp    x4, jt_offsets_tab
    add     x4, x4, :lo12:jt_offsets_tab
    ldrsw   x18, [x4, x18, lsl #2]
    add     x18, x18, x4
    br      x18
.Ljt_offsets_1:
     mov     w0, #10
    ret
.Ljt_offsets_2:
     mov     w0, #11
    ret
.Ljt_offsets_3:
     mov     w0, #12
    ret
9:  mov     w0, #0
    ret
END jt_offsets
// The index another register, the check on a register copied into it, the entry loaded into another register
FN jt_offsets_copy
    sub     w0, w18, #0xd1
    cmp     w0, #2
    b.hs    9f
    adrp    x17, jt_copy_tab
    add     x17, x17, :lo12:jt_copy_tab
    mov     w3, w0
    ldrsw   x5, [x17, x3, lsl #2]
    add     x18, x5, x17
    br      x18
.Ljt_offsets_copy_1:
     mov     w0, #10
    ret
.Ljt_offsets_copy_2:
     mov     w0, #11
    ret
9:  mov     w0, #0
    ret
END jt_offsets_copy
// The check on a masked copy
FN jt_masked
    and     w16, w18, #0x3f
    cmp     w16, #2
    b.hi    9f
    adrp    x15, jt_masked_tab
    add     x15, x15, :lo12:jt_masked_tab
    and     x18, x18, #0x3f
    ldrsw   x18, [x15, x18, lsl #2]
    add     x18, x18, x15
    br      x18
.Ljt_masked_1:
     mov     w0, #10
    ret
.Ljt_masked_2:
     mov     w0, #11
    ret
.Ljt_masked_3:
     mov     w0, #12
    ret
9:  mov     w0, #0
    ret
END jt_masked
// Entries relative to an adr
FN jt_adr
    cmp     w0, #2
    b.hi    9f
    adrp    x17, jt_adr_tab
    add     x17, x17, :lo12:jt_adr_tab
    adr     x18, 1f
    ldrsw   x1, [x17, x0, lsl #2]
    add     x18, x18, x1
    br      x18
1:  mov     w0, #10
    ret
2:  mov     w0, #11
    ret
3:  mov     w0, #12
    ret
9:  mov     w0, #0
    ret
END jt_adr
// b.ls into the dispatch
FN jt_ls
    cmp     w18, #1
    b.ls    1f
    mov     w0, #0
    ret
1:  adrp    x4, jt_ls_tab
    add     x4, x4, :lo12:jt_ls_tab
    ldrsw   x18, [x4, x18, lsl #2]
    add     x18, x18, x4
    br      x18
.Ljt_ls_2:
     mov     w0, #10
    ret
.Ljt_ls_3:
     mov     w0, #11
    ret
END jt_ls
// The base set far back, in the function's first block
FN jt_far_base
    adrp    x24, jt_far_tab
    add     x24, x24, :lo12:jt_far_tab
    .rept 300
    add     x1, x1, #1
    .endr
    cmp     w18, #1
    b.hi    9f
    ldrsw   x18, [x24, x18, lsl #2]
    add     x18, x18, x24
    br      x18
.Ljt_far_base_1:
     mov     w0, #10
    ret
.Ljt_far_base_2:
     mov     w0, #11
    ret
9:  mov     w0, #0
    ret
END jt_far_base
// The base built further back and copied on its way
FN jt_copied_base
    adrp    x7, jt_cb_tab
    add     x7, x7, #:lo12:jt_cb_tab
    mov     x21, x7
    .rept 280
    add     x1, x1, #1
    .endr
    mov     x7, x21
    cmp     w18, #1
    b.hi    9f
    ldrsw   x18, [x7, x18, lsl #2]
    add     x18, x18, x7
    br      x18
.Ljt_copied_base_1:
     mov     w0, #10
    ret
.Ljt_copied_base_2:
     mov     w0, #11
    ret
9:  mov     w0, #0
    ret
END jt_copied_base
// A zero-guarded complemented index
FN jt_masked_not
    tst     w3, #0x3
    b.eq    9f
    mvn     w3, w3
    and     x5, x3, #0x3
    adrp    x6, jt_mn_tab
    add     x6, x6, :lo12:jt_mn_tab
    ldrsw   x7, [x6, x5, lsl #2]
    add     x18, x7, x6
    br      x18
.Ljt_masked_not_1:
     mov     w0, #10
    ret
.Ljt_masked_not_2:
     mov     w0, #11
    ret
.Ljt_masked_not_3:
     mov     w0, #12
    ret
9:  mov     w0, #0
    ret
END jt_masked_not
// The checked value zero-extended into the index
FN jt_zero_ext
    adrp    x6, jt_ze_tab
    add     x6, x6, :lo12:jt_ze_tab
    cmp     w3, #2
    b.hi    9f
    and     x5, x3, #0xffffffff
    ldrsw   x7, [x6, x5, lsl #2]
    add     x18, x7, x6
    br      x18
.Ljt_zero_ext_1:
     mov     w0, #10
    ret
.Ljt_zero_ext_2:
     mov     w0, #11
    ret
.Ljt_zero_ext_3:
     mov     w0, #12
    ret
9:  mov     w0, #0
    ret
END jt_zero_ext
// The index copied from the checked register before the check, far back
FN jt_copy_first
    adrp    x11, jt_cf_tab
    add     x11, x11, :lo12:jt_cf_tab
    mov     w0, w18
    .rept 20
    add     x1, x1, #1
    .endr
    cmp     w18, #1
    b.hi    9f
    ldrsw   x18, [x11, x0, lsl #2]
    add     x18, x18, x11
    br      x18
.Ljt_copy_first_1:
     mov     w0, #10
    ret
.Ljt_copy_first_2:
     mov     w0, #11
    ret
9:  mov     w0, #0
    ret
END jt_copy_first
// The byte table with its add further back
FN jt_bytes_far
    adrp    x17, jt_bytes_tab
    add     x17, x17, :lo12:jt_bytes_tab
    adr     x18, 1f
    ldrb    w0, [x17, x15]
    add     x18, x18, x0, lsl #2
    .rept 17
    mov     x1, x2
    .endr
    br      x18
1:  mov     w0, #10
    ret
    mov     w0, #11
    ret
END jt_bytes_far
// A pointer load and br x18 that ends the function: a tail call
FN tc_end
    ldr     x18, [x0, #0x18]
    ldr     x16, [sp]
    br      x18
END tc_end
// An unframed thunk with zero padding after it
FN tc_padded
    mov     x1, x0
    ldr     x18, [x1, #8]
    br      x18
    .word   0, 0, 0
END tc_padded
// A 16-byte thunk frame: guards and argument moves, its error block after the br
FN tc_thunk
    stp     x29, x30, [sp, #-0x10]!
    mov     x29, sp
    cbz     x0, 1f
    ldr     x18, [x0, #0x10]
    mov     x1, x0
    ldp     x29, x30, [sp], #0x10
    br      x18
1:  mov     x0, #0
    bl      tc_end
END tc_thunk
// An ordinary frame given back right before the br
FN tc_frame
    sub     sp, sp, #0x30
    stp     x29, x30, [sp, #0x20]
    add     x29, sp, #0x20
    str     x0, [sp, #8]
    cbz     x0, 1f
    bl      tc_end
1:  ldr     x18, [sp, #8]
    ldr     x18, [x18, #0x20]
    ldp     x29, x30, [sp, #0x20]
    add     sp, sp, #0x30
    br      x18
    mov     w0, #0
    ret
END tc_frame

// A halfword table with no bounds check in reach: every entry gets a pad
FN jt_halfs_open
    adrp    x17, jt_halfs_tab
    add     x17, x17, :lo12:jt_halfs_tab
    ldrh    w30, [x17, x16, lsl #1]
    adr     x18, 1f
    add     x18, x18, x30, lsl #2
    br      x18
1:  mov     w0, #10
    ret
    mov     w0, #11
    ret
END jt_halfs_open

    .section .rodata
    .balign 4
jt_bytes_tab:
    .byte   0, 2, 4, 6
    .balign 4
jt_halfs_tab:
    .hword  0, 2, 4, 6
    .balign 4
jt_adr_tab:
    .word   0, 8, 16
jt_offsets_tab:
    .word   .Ljt_offsets_1 - jt_offsets_tab
    .word   .Ljt_offsets_2 - jt_offsets_tab
    .word   .Ljt_offsets_3 - jt_offsets_tab
    .word   .Ljt_offsets_1 - jt_offsets_tab
    .word   .Ljt_offsets_2 - jt_offsets_tab
jt_copy_tab:
    .word   .Ljt_offsets_copy_1 - jt_copy_tab
    .word   .Ljt_offsets_copy_2 - jt_copy_tab
jt_masked_tab:
    .word   .Ljt_masked_1 - jt_masked_tab
    .word   .Ljt_masked_2 - jt_masked_tab
    .word   .Ljt_masked_3 - jt_masked_tab
jt_ls_tab:
    .word   .Ljt_ls_2 - jt_ls_tab
    .word   .Ljt_ls_3 - jt_ls_tab
jt_far_tab:
    .word   .Ljt_far_base_1 - jt_far_tab
    .word   .Ljt_far_base_2 - jt_far_tab
jt_cb_tab:
    .word   .Ljt_copied_base_1 - jt_cb_tab
    .word   .Ljt_copied_base_2 - jt_cb_tab
jt_mn_tab:
    .word   .Ljt_masked_not_1 - jt_mn_tab
    .word   .Ljt_masked_not_2 - jt_mn_tab
    .word   .Ljt_masked_not_3 - jt_mn_tab
jt_ze_tab:
    .word   .Ljt_zero_ext_1 - jt_ze_tab
    .word   .Ljt_zero_ext_2 - jt_ze_tab
    .word   .Ljt_zero_ext_3 - jt_ze_tab
jt_cf_tab:
    .word   .Ljt_copy_first_1 - jt_cf_tab
    .word   .Ljt_copy_first_2 - jt_cf_tab
