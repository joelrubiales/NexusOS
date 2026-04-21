OUTPUT_FORMAT(elf64-x86-64)
ENTRY(trampoline_entry)
SECTIONS
{
    . = 0x8000;
    .text : { *(.text) }
}
