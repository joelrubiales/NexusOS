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

COBJS := kernel.o idt.o pit.o keyboard.o pantalla.o teclado.o pci.o hda.o xhci.o nic.o e1000.o net.o vga.o gfx.o compositor.o gui.o font8x8.o \
	syscalls.o nexus_userland.o ext2.o shm.o ipc.o \
	mouse.o memory.o paging.o kmalloc.o disk.o multitasking.o scheduler.o shell.o gui_installer.o vesa.o window.o desktop.o installer_ui.o mouse_gui.o \
	apps.o top_panel.o dock_icons.o icons_data.o vfs.o tar.o event_queue.o event_system.o ui_manager.o font_aa.o elf_loader.o smp.o
SOBJ  := boot.o
AOBJS := isr.o task_switch.o sched_switch.o user_jump.o syscall_entry.o
TRAMP_BIN_O := smp_trampoline_bin.o
OBJS  := $(SOBJ) $(COBJS) font_aa_data.o $(AOBJS) $(TRAMP_BIN_O)

.PHONY: all clean dist clean-dist

all: $(ISO_IMAGE)

# ── Fuente AA: generación automática desde font8x8.c ─────────────────────
# font_aa_data.h/.c se regeneran cuando cambia el script o la fuente 8×8.
font_aa_data.c font_aa_data.h: gen_font_aa.py font8x8.c
	python3 gen_font_aa.py

# Todos los .o del kernel dependen del header generado para poder incluirlo.
$(COBJS) font_aa_data.o: font_aa_data.h

# Regla explícita para el .c generado (no sigue la regla %.o: %.c genérica).
font_aa_data.o: font_aa_data.c font_aa_data.h
	$(CC) $(CFLAGS) -c font_aa_data.c -o $@

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

# Userland (PIE) — ELF en /bin del initrd.
USER_PKG  := userland
USER_LD   := $(LD)
USER_ULIB := $(LIBGCC)
USER_ELFS := $(USER_PKG)/init.elf $(USER_PKG)/dock.elf $(USER_PKG)/installer.elf

$(USER_PKG)/init.elf: $(USER_PKG)/init.c $(USER_PKG)/nexus_ulib.h $(USER_PKG)/user.ld
	$(CC) -m64 -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fpie -O2 \
		-I$(USER_PKG) -I. -c $(USER_PKG)/init.c -o $(USER_PKG)/init.o
	$(USER_LD) -m elf_x86_64 -T $(USER_PKG)/user.ld -nostdlib -o $@ $(USER_PKG)/init.o $(USER_ULIB)

$(USER_PKG)/dock.elf: $(USER_PKG)/dock.c $(USER_PKG)/nexus_ulib.h $(USER_PKG)/user.ld
	$(CC) -m64 -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fpie -O2 \
		-I$(USER_PKG) -I. -c $(USER_PKG)/dock.c -o $(USER_PKG)/dock.o
	$(USER_LD) -m elf_x86_64 -T $(USER_PKG)/user.ld -nostdlib -o $@ $(USER_PKG)/dock.o $(USER_ULIB)

$(USER_PKG)/installer.elf: $(USER_PKG)/installer.c $(USER_PKG)/nexus_ulib.h $(USER_PKG)/user.ld
	$(CC) -m64 -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fpie -O2 \
		-I$(USER_PKG) -I. -c $(USER_PKG)/installer.c -o $(USER_PKG)/installer.o
	$(USER_LD) -m elf_x86_64 -T $(USER_PKG)/user.ld -nostdlib -o $@ $(USER_PKG)/installer.o $(USER_ULIB)

# Initrd: genera los assets BMP y los empaqueta en un USTAR TAR.
INITRD := $(ISO_DIR)/boot/initrd.tar
$(INITRD): gen_initrd.py $(USER_ELFS) | $(ISO_DIR)/boot/grub
	python3 gen_initrd.py $@

boot.o: boot.S multiboot2_asm.h
	$(CC) $(CFLAGS) -c $< -o $@

smp_trampoline.o: smp_trampoline.asm
	$(NASM) -f elf64 $< -o $@

smp_trampoline.elf: smp_trampoline.o smp_trampoline.ld
	$(LD) -m elf_x86_64 -T smp_trampoline.ld -o $@ smp_trampoline.o

smp_trampoline.bin: smp_trampoline.elf
	objcopy -O binary $< $@

$(TRAMP_BIN_O): smp_trampoline.bin
	$(LD) -m elf_x86_64 -r -b binary -o $@ $<

# gfx.c: intrínsecos SSE4.1 (-nostdinc: headers del propio GCC vía -isystem …/include).
GCC_INTRIN_INC := $(shell $(CC) $(CFLAGS) -print-file-name=include)
GFX_CFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -msse2 -msse3 -mssse3 -msse4.1 \
	-isystem $(GCC_INTRIN_INC)

$(filter-out gfx.o,$(COBJS)): %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

gfx.o: gfx.c font_aa_data.h
	$(CC) $(GFX_CFLAGS) -c $< -o $@

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
	rm -f $(USER_PKG)/*.o $(USER_ELFS)
	rm -rf $(ISO_DIR)
	rm -f kernel.bin
	rm -f font_aa_data.c font_aa_data.h
	rm -f smp_trampoline.o smp_trampoline.elf smp_trampoline.bin $(TRAMP_BIN_O)

clean-dist:
	rm -rf $(DIST_DIR)
