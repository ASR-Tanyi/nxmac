// The start of an NRO's text: a branch, the offset of MOD0, then the NRO0 header's room (filled in by elf2nro.py)
    .section .crt0, "ax"
    .globl _start
_start:
    b       1f
    .word   __mod0 - _start
    .space  0x78
1:  ret
    .section .mod0, "a"
    .balign 4
__mod0:
    .ascii  "MOD0"
    .word   __dynamic_start - __mod0
    .word   __bss_start - __mod0
    .word   __bss_end - __mod0
    .word   __eh_frame_hdr_start - __mod0
    .word   __eh_frame_hdr_end - __mod0
    .word   0
