#ifndef NEXUS_VESA_H
#define NEXUS_VESA_H

#include <stdint.h>
#include "boot_info.h"

/*
 * VBE 3.0 — offsets dentro del Mode Information Block (INT 10h, AX=4F01h).
 * boot2 escribe el bloque temporal en NEXUS_VBE_MODEINFO_SCRATCH_PHYS (boot_info.h),
 * luego copia campos a NexusBootInfo @ NEXUS_BOOT_INFO_PHYS.
 */
#define NEXUS_BOOT_VBE_MODEINFO_PHYS NEXUS_VBE_MODEINFO_SCRATCH_PHYS

#define VBE_MODEINFO_BYTES_PER_SCANLINE     0x10u /* pitch plano “legacy” */
#define VBE_MODEINFO_XRES                   0x12u
#define VBE_MODEINFO_YRES                   0x14u
#define VBE_MODEINFO_PHYS_BASE_PTR          0x28u /* LFB físico (PhysBasePtr) */
/* VBE 3.0: en modos lineales muchas BIOS (p. ej. VirtualBox) rellenan solo esto: */
#define VBE_MODEINFO_LIN_BYTES_PER_SCANLINE 0x32u

#endif
