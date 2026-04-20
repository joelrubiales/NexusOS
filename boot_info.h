#ifndef NEXUS_BOOT_INFO_H
#define NEXUS_BOOT_INFO_H

#include <stdint.h>

/*
 * Buzón físico fijo: kernel_main parsea MBI (GRUB); legacy VESA usaba boot2.
 * Mapa identidad: BOOT_INFO_ADDR y lfb_ptr son direcciones físicas válidas como punteros.
 *
 * Layout empaquetado (28 bytes): magic, geometría 32-bit, lfb_ptr 64-bit.
 */
#define BOOT_INFO_ADDR   0x5000u
#define BOOT_INFO_MAGIC  0x1337BEEFu

/* Alias legado en el árbol */
#define NEXUS_BOOT_INFO_PHYS     BOOT_INFO_ADDR
#define NEXUS_BOOT_HANDOFF_MAGIC BOOT_INFO_MAGIC

#define NEXUS_VBE_MODEINFO_SCRATCH_PHYS 0x6200u

typedef struct __attribute__((packed)) BootInfo {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint64_t lfb_ptr;
} BootInfo;

typedef BootInfo NexusBootInfo;

#endif
