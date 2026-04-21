OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS {
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS {
  . = 0;
  .text : {
    *(.text .text.*)
  } :text
  .rodata : {
    *(.rodata .rodata.*)
  } :text
  .data : {
    *(.data .data.*)
  } :data
  .bss : {
    *(.bss .bss.*)
    *(COMMON)
  } :data
  /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
