#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>

/* NexusOS — gestor de memoria física (bitmap 4 KiB) + heap kmalloc/kfree.
 * Arquitectura: x86-64 Long Mode, mapa identidad (bootloader). */

#define PAGE_SHIFT   12
#define PAGE_SIZE    4096ULL
/* Carga física del kernel (Multiboot2 / linker.ld). */
#define KERNEL_LOAD_PHYS 0x100000ULL
#define MAX_PHYS_RAM (128ULL * 1024ULL * 1024ULL) /* supuesto; PMM cubre hasta aquí */
#define PMM_PAGES    ((unsigned)(MAX_PHYS_RAM / PAGE_SIZE))

/* Heap del kernel: RAM identidad >= 32 MiB, tamaño >= 16 MiB (32 MiB por defecto). */
#define KHEAP_PHYS_START 0x02000000ULL
#define KHEAP_SIZE       (32ULL * 1024ULL * 1024ULL)

void  memory_init(void);
void  kheap_init(void);
void  kheap_panic_nomem(const char* msg);

/* Paginación x86-64 (identidad): flags bajos tipo PTE (p. ej. VMM_PAGE_PRESENT|VMM_PAGE_RW). */
#define VMM_PAGE_PRESENT (1ULL << 0)
#define VMM_PAGE_RW      (1ULL << 1)

void vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
void vmm_identity_map_range(uint64_t phys_start, uint64_t len, uint64_t flags);

/* Identity-map del LFB completo (pitch×height, PTE 4 KiB); usar tras memory_init + PMM listo. */
void memory_map_framebuffer_identity(uint64_t framebuffer_phys, uint32_t pitch, uint32_t height,
                                     uint64_t pte_flags);

/* Reserva la región del Multiboot2 Information Structure (tags de GRUB); no debe usarla kmalloc/PMM. */
void pmm_reserve_multiboot_info(uint32_t mbi_phys);

/* Reserva RAM física [start, start+len) para no usarla como RAM genérica (p. ej. LFB). */
void  pmm_reserve_phys_range(uintptr_t start, size_t len);

uintptr_t pmm_alloc_page(void);
void      pmm_free_page(uintptr_t phys);

void* kmalloc(uint64_t size);
void  kfree(void* ptr);

unsigned pmm_free_pages(void);
unsigned pmm_total_pages(void);

#endif
