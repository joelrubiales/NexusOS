/*
 * NexusOS — arranque Multiboot2 (GNU as, sintaxis AT&T).
 *
 * GRUB (BIOS o UEFI) carga este módulo en protegido 32-bit y salta a _start con:
 *   %eax = 0x36d76289  (magic de la estructura de información Multiboot2)
 *   %ebx = dirección FÍSICA de la Multiboot2 Information Structure (MBI)
 *
 * Flujo: validar magic → guardar EAX/EBX para el kernel → pila 16 KiB → tablas
 * de paginación (PML4→PDPT→PD, mapa identidad 2 MiB × N) → GDT → activar PAE,
 * EFER.LME y CR0.PG → ret largo a segmento de código 64-bit → call kernel_main.
 */
#include "multiboot2_asm.h"

/* CR3 apunta aquí; debajo de 1 MiB, cubierto por el primer huge frame 2 MiB. */
.set PAGE_TABLE_ROOT, 0x70000

.set MB2_BOOT_MAGIC, 0x36d76289

/* ─── Cabecera Multiboot2 (debe aparecer en los primeros 32 KiB del segmento PT_LOAD) ─── */
.section .multiboot, "a"
.align 8
multiboot_header:
	/* Campos fijos: magic, arquitectura i386 (GRUB entra en 32-bit), longitud, checksum. */
	.long MB2_HEADER_MAGIC
	.long MB2_ARCHITECTURE_I386
	.long (header_end - multiboot_header)
	.long -(MB2_HEADER_MAGIC + MB2_ARCHITECTURE_I386 + (header_end - multiboot_header))

	/* Tag 5: petición de framebuffer (width, height, depth en bits). */
	.word 5
	.word 0
	.long 20
	.long 1024
	.long 768
	.long 32

	/* Cada tag debe estar alineado a 8 bytes; el tag anterior ocupa 20 bytes → relleno. */
	.align 8

	/* Tag 0: fin de la lista de tags. */
	.word 0
	.word 0
	.long 8
header_end:

/* Valores guardados para pasarlos a kernel_main(magic, mb2_info_phys) en SysV x86-64. */
.section .data
.align 4
.globl multiboot_info_phys
multiboot_info_phys:
	.long 0

.globl mb2_boot_magic
mb2_boot_magic:
	.long 0

/* Pila del arranque: al menos 16 KiB; crece hacia direcciones menores desde stack_top. */
.section .bss
.align 16
stack_bottom:
	.space 16384
stack_top:

/* ─── GDT: código/datos 32-bit para compatibilidad, código 64-bit (L=1) para Long Mode ─── */
.section .rodata
.align 8
gdt_start:
	.quad 0
	/* 32-bit code ring 0 (selector 0x08). */
	.quad 0x00cf9a000000ffff
	/* 32-bit data ring 0 (selector 0x10). */
	.quad 0x00cf92000000ffff
	/* 64-bit code ring 0 (selector 0x18): base/limit ignorados; bit L en flags. */
	.quad 0x00209a0000000000
gdt_end:

.align 4
gdt_desc:
	.word gdt_end - gdt_start - 1
	.long gdt_start

.section .text
.code32

.extern kernel_main

.globl _start
_start:
	/*
	 * Paso 0 — Entorno inicial (32-bit, paginación de GRUB irrelevante para nuestro mapa).
	 * Sin IDT propia aún: mantener IF=0 para no recibir IRQ antes de preparar el kernel.
	 */
	cli

	cmpl	$MB2_BOOT_MAGIC, %eax
	jne	.Lhang

	/*
	 * Paso 1 — Preservar el protocolo Multiboot2 antes de tocar EAX/EBX.
	 * El kernel C recibirá estos valores como primer y segundo argumento (RDI, RSI).
	 */
	movl	%eax, mb2_boot_magic
	movl	%ebx, multiboot_info_phys

	/* Paso 2 — Pila en zona BSS (16 KiB); %esp = tope (alineado en .bss). */
	movl	$stack_top, %esp

	/*
	 * Paso 3 — Mapa identidad con páginas de 2 MiB (PD con PS=1).
	 * PML4[0] → PDPT; PDPT[0..3] → cuatro PD; cada entrada PD mapea 512 × 2 MiB.
	 */
	call	setup_page_tables

	/* Paso 4 — Cargar nuestra GDT y registrar la raíz de niveles de paginación en CR3. */
	lgdt	gdt_desc
	movl	$PAGE_TABLE_ROOT, %eax
	movl	%eax, %cr3

	/*
	 * Paso 5 — Activar PAE (bit 5 de CR4): obligatorio antes de usar tablas de 64-bit
	 * y páginas de 2 MiB en modo IA-32e.
	 */
	movl	%cr4, %eax
	orl	$(1 << 5), %eax
	movl	%eax, %cr4

	/*
	 * Paso 6 — EFER.LME (bit 8): habilita “long mode” pendiente; solo toma efecto
	 * completo al activar paginación (CR0.PG).
	 */
	movl	$0xC0000080, %ecx
	rdmsr
	orl	$(1 << 8), %eax
	wrmsr

	/*
	 * Paso 7 — CR0.PG: enciende paginación. Con EFER.LME=1 y CR4.PAE=1 el CPU pasa a
	 * modo de compatibilidad 32-bit bajo IA-32e (aún no ejecutamos código 64-bit puro).
	 */
	movl	%cr0, %eax
	orl	$(1 << 31 | 1), %eax
	movl	%eax, %cr0

	/*
	 * Paso 8 — Saltar al segmento de código 64-bit (selector 0x18).
	 * Empujar CS y offset y ejecutar lret: carga CS con el descriptor L=1 y
	 * %eip = .Lentry64 → primera instrucción en 64-bit (Long Mode activo).
	 */
	pushl	$0x18
	pushl	$.Lentry64
	lret

.Lhang:
	cli
	hlt
	jmp	.Lhang

/*
 * Limpia 6 páginas, rellena PML4/PDPT/PD y marca entradas PD como huge (present|rw|ps).
 */
setup_page_tables:
	movl	$PAGE_TABLE_ROOT, %edi
	xorl	%eax, %eax
	movl	$(6 * 4096 / 4), %ecx
	rep stosl

	movl	$PAGE_TABLE_ROOT, %edi
	movl	$(PAGE_TABLE_ROOT + 0x1000 + 3), %eax
	movl	%eax, (%edi)
	movl	$0, 4(%edi)

	movl	$(PAGE_TABLE_ROOT + 0x1000), %edi
	movl	$(PAGE_TABLE_ROOT + 0x2000 + 3), %eax
	movl	$4, %ecx
.Lpdpt:
	movl	%eax, (%edi)
	movl	$0, 4(%edi)
	addl	$0x1000, %eax
	addl	$8, %edi
	loop	.Lpdpt

	movl	$(PAGE_TABLE_ROOT + 0x2000), %edi
	xorl	%ecx, %ecx
.Lpd:
	movl	%ecx, %eax
	shll	$21, %eax
	orl	$0x83, %eax
	stosl
	xorl	%eax, %eax
	stosl
	incl	%ecx
	cmpl	$2048, %ecx
	jb	.Lpd
	ret

/* ─── Long Mode: selectores de datos nulos en práctica; RSP = misma pila física ─── */
.code64
.Lentry64:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss
	movq	$stack_top, %rsp

	/* SysV AMD64: RDI = magic, RSI = dirección física MBI (zero-extend desde 32-bit). */
	movl	mb2_boot_magic(%rip), %edi
	movl	multiboot_info_phys(%rip), %esi

	movabs	$kernel_main, %rax
	call	*%rax

.Lhalt:
	cli
	hlt
	jmp	.Lhalt
