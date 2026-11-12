PHDRS { text PT_LOAD FLAGS(5); ro PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); dyn PT_DYNAMIC FLAGS(6); tls PT_TLS FLAGS(4); }
SECTIONS {
  . = 0;
  .text : { KEEP(*(.crt0)) *(.text .text.*) } :text
  .plt : { *(.plt) *(.iplt) } :text
  . = ALIGN(0x1000);
  .rodata : { *(.rodata .rodata.*) } :ro
  .mod0 : { KEEP(*(.mod0)) } :ro
  .eh_frame_hdr : { HIDDEN(__eh_frame_hdr_start = .); *(.eh_frame_hdr) HIDDEN(__eh_frame_hdr_end = .); } :ro
  .eh_frame : { KEEP(*(.eh_frame)) } :ro
  .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :ro
  .dynsym : { *(.dynsym) } :ro
  .dynstr : { *(.dynstr) } :ro
  .hash : { *(.hash) } :ro
  .rela.dyn : { *(.rela.dyn) } :ro
  .rela.plt : { *(.rela.plt) } :ro
  . = ALIGN(0x1000);
  .tdata : { *(.tdata .tdata.*) } :data :tls
  .tbss : { *(.tbss .tbss.*) } :data :tls
  .init_array : { KEEP(*(.init_array .init_array.*)) } :data
  .fini_array : { KEEP(*(.fini_array .fini_array.*)) } :data
  .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
  .dynamic : { HIDDEN(__dynamic_start = .); *(.dynamic) } :data :dyn
  .got : { *(.got) } :data
  .got.plt : { *(.got.plt) } :data
  .data : { *(.data .data.*) } :data
  HIDDEN(__bss_start = .);
  .bss : { *(.bss .bss.*) *(COMMON) } :data
  HIDDEN(__bss_end = .);
  /DISCARD/ : { *(.comment) *(.note*) *(.gnu.hash) }
}
