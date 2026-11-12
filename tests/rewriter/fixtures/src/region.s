// The SDK functions the address region and StandardAllocator gates go on, by their symbols
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

FN _ZN2nn2os6detail11VammManager21AllocateAddressRegionEPmm
    stp     x29, x30, [sp, #-32]!
    stp     x20, x19, [sp, #16]
    mov     x29, sp
    mov     x19, x1
    mov     x20, x2
    add     x8, x0, x20
    cbz     x8, 1f
    str     x8, [x19]
    mov     w0, #1
    b       2f
1:  mov     w0, #0
2:  ldp     x20, x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret
END _ZN2nn2os6detail11VammManager21AllocateAddressRegionEPmm
FN _ZN2nn2os6detail11VammManager17FreeAddressRegionEm
    mov     x8, x1
    str     xzr, [x0, #8]
    ret
END _ZN2nn2os6detail11VammManager17FreeAddressRegionEm
FN _ZN2nn3mem17StandardAllocator10InitializeEPvm
    mov     w3, #0
    b       _ZN2nn3mem17StandardAllocator10InitializeEPvmb
END _ZN2nn3mem17StandardAllocator10InitializeEPvm
FN _ZN2nn3mem17StandardAllocator10InitializeEPvmb
    str     x1, [x0]
    str     x2, [x0, #8]
    strb    w3, [x0, #16]
    ret
END _ZN2nn3mem17StandardAllocator10InitializeEPvmb
