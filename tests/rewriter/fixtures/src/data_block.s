// A block of data in the text that no function covers: words there that read as rewrite sites are left alone
    .text
    .globl before_data
    .type before_data, %function
before_data:
    .cfi_startproc
    add     x18, x18, #1
    ret
    .cfi_endproc
    .balign 16
    .rept 300
    .word   0xffffffff, 0xd53bd040, 0xe7ffdefe, 0x91000652, 0xffff0000, 0xd4000021, 0x00000000, 0xfefefefe
    .endr
    .globl after_data
    .type after_data, %function
after_data:
    .cfi_startproc
    add     x18, x18, #2
    svc     #1
    ret
    .cfi_endproc
    // Code without unwind information stays code: it decodes
    .rept 1100
    add     x1, x1, #1
    .endr
    add     x18, x18, #3
    ret
