/* NexusOS — ELF64 Multiboot2 (GRUB). VMA=LMA desde 1 MiB.
 *
 * La cabecera MB2 debe ser lo primero en la imagen cargable: va al inicio de
 * la sección de salida .text (antes de cualquier instrucción). */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    /* KEEP(.multiboot) primero: un solo byte de código antes rompe el escaneo de GRUB. */
    .text : ALIGN(0x1000) {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
        *(.rdata .rdata.*)
    }

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    }

    .bss : ALIGN(0x1000) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(0x1000);
    __kernel_image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame .eh_frame_hdr)
        *(.debug*)
    }
}
