// A literal load into x18: a site the rewriter cannot rewrite, so the rewrite stops
    .text
    .globl fail_literal
    .type fail_literal, %function
fail_literal:
    .cfi_startproc
    add     x18, x18, #1
    ldr     x18, 1f
    ret
1:  .quad   0x123456789abcdef0
    .cfi_endproc
