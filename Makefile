# NexusOS — toolchain GCC/LD, imagen ISO con grub-mkrescue (Multiboot2).
# Requisitos: gcc, ld (bfd), nasm (apt install nasm), grub-common, xorriso.

CROSS    ?=
CC       := $(CROSS)gcc
LD       := $(CROSS)ld

# NASM: preferir copia local (.tools/bin) si no hay sudo; si no, nasm del sistema.
NASM_LOCAL := $(CURDIR)/.tools/bin/nasm
ifneq (,$(wildcard $(NASM_LOCAL)))
NASM := $(NASM_LOCAL)
else
NASM := nasm
endif

ifeq (,$(shell command -v $(NASM) 2>/dev/null))
$(error No se encontró nasm. Opciones: sudo apt install nasm  |  o dejar $(NASM_LOCAL) tras extraer el .deb de Ubuntu universe)
endif

ISO_DIR   := iso
ISO_IMAGE := NexusOS.iso
KERNEL_ELF := kernel.elf
KERNEL_BIN := $(ISO_DIR)/boot/kernel.bin
# Copia lista para subir a Drive / adjuntar en VirtualBox (un solo archivo .iso).
DIST_DIR  := dist

# Cabeceras freestanding (-nostdinc): -Iinclude antes que ningún sistema.
INCLUDES := -Iinclude -I.

# Banderas obligatorias kernel 64-bit + flags de compilación segura habitual.
CFLAGS := -m64 \
	-ffreestanding -fno-builtin -nostdlib -nostdinc \
	-mno-red-zone -mcmodel=large \
	-Wall -Wextra \
	-std=gnu11 -O2 \
	-fno-stack-protector -fno-pie -fno-pic \
	-fno-asynchronous-unwind-tables -mgeneral-regs-only \
	$(INCLUDES)

# Enlazado: sin crt; libgcc si hace falta división 64-bit u otros builtins.
LIBGCC   := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name)
LDFLAGS  := -m elf_x86_64 -T linker.ld -nostdlib -static

COBJS := kernel.o idt.o pit.o keyboard.o pantalla.o teclado.o pci.o nic.o vga.o gfx.o gui.o font8x8.o \
	mouse.o memory.o paging.o kmalloc.o disk.o multitasking.o scheduler.o shell.o gui_installer.o vesa.o window.o desktop.o installer_ui.o mouse_gui.o \
	apps.o top_panel.o dock_icons.o icons_data.o vfs.o event_system.o
SOBJ  := boot.o
AOBJS := isr.o task_switch.o sched_switch.o
OBJS  := $(SOBJ) $(COBJS) $(AOBJS)

.PHONY: all clean dist clean-dist

all: $(ISO_IMAGE)

dist: $(ISO_IMAGE)
	mkdir -p $(DIST_DIR)
	cp -f $(ISO_IMAGE) $(DIST_DIR)/NexusOS.iso
	cp -f $(ISO_IMAGE) $(DIST_DIR)/NexusOS-$$(date +%Y%m%d-%H%M).iso
	@echo "Listo: $(DIST_DIR)/NexusOS.iso (y copia con fecha para historial)"

$(ISO_DIR)/boot/grub:
	mkdir -p $(ISO_DIR)/boot/grub

# GRUB2: copiar configuración versionada (dual-boot gui/cli).
BOOT_GRUB_CFG := boot/grub/grub.cfg
$(ISO_DIR)/boot/grub/grub.cfg: $(BOOT_GRUB_CFG) | $(ISO_DIR)/boot/grub
	cp -f $(BOOT_GRUB_CFG) $@

# Initrd: genera los assets BMP y los empaqueta en un USTAR TAR.
INITRD := $(ISO_DIR)/boot/initrd.tar
$(INITRD): gen_initrd.py | $(ISO_DIR)/boot/grub
	python3 gen_initrd.py $@

boot.o: boot.S multiboot2_asm.h
	$(CC) $(CFLAGS) -c $< -o $@

$(COBJS): %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(AOBJS): %.o: %.asm
	$(NASM) -f elf64 $< -o $@

$(KERNEL_ELF): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

$(KERNEL_BIN): $(KERNEL_ELF) | $(ISO_DIR)/boot/grub
	cp -f $(KERNEL_ELF) $(KERNEL_BIN)

$(ISO_IMAGE): $(KERNEL_BIN) $(ISO_DIR)/boot/grub/grub.cfg $(INITRD)
	grub-mkrescue -o $(ISO_IMAGE) $(ISO_DIR)

clean:
	rm -f $(OBJS) $(KERNEL_ELF) $(ISO_IMAGE)
	rm -rf $(ISO_DIR)
	rm -f kernel.bin

clean-dist:
	rm -rf $(DIST_DIR)
