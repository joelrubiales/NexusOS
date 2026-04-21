#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Cargador ELF64 para NexusOS (PT_LOAD, ET_EXEC / ET_DYN estático).
 * Requiere GDT ring 3 (boot.S), EFER.NXE y VMM_PAGE_USER/NX (memory.h).
 *
 * Espacio virtual: el mapa de arranque cubre [0, 4 GiB); el cargador exige
 * que imagen + pila quepan ahí (pila fija cerca de 2 GiB).
 */

/* Selectores GDT (RPL=3): orden compatible STAR/SYSRET (boot.S). */
#define SEL_UDATA   0x2Bu
#define SEL_UCODE64 0x33u

#define ELF_EHDR_MAGIC0 0x7Fu
#define ELF_EHDR_MAGIC1 'E'
#define ELF_EHDR_MAGIC2 'L'
#define ELF_EHDR_MAGIC3 'F'

#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define EV_CURRENT   1

#define ET_NONE 0
#define ET_EXEC 2
#define ET_DYN  3

#define EM_NONE   0
#define EM_X86_64 62

#define PT_LOAD 1

#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

typedef struct __attribute__((packed)) {
    unsigned char e_ident[16];
    uint16_t      e_type;
    uint16_t      e_machine;
    uint32_t      e_version;
    uint64_t      e_entry;
    uint64_t      e_phoff;
    uint64_t      e_shoff;
    uint32_t      e_flags;
    uint16_t      e_ehsize;
    uint16_t      e_phentsize;
    uint16_t      e_phnum;
    uint16_t      e_shentsize;
    uint16_t      e_shnum;
    uint16_t      e_shstrndx;
} Elf64_Ehdr;

typedef struct __attribute__((packed)) {
    uint32_t   p_type;
    uint32_t   p_flags;
    uint64_t   p_offset;
    uint64_t   p_vaddr;
    uint64_t   p_paddr;
    uint64_t   p_filesz;
    uint64_t   p_memsz;
    uint64_t   p_align;
} Elf64_Phdr;

_Static_assert(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr");
_Static_assert(sizeof(Elf64_Phdr) == 56, "Elf64_Phdr");

typedef enum {
    ELF_LOAD_OK = 0,
    ELF_LOAD_ERR_NULL = -1,
    ELF_LOAD_ERR_TRUNC = -2,
    ELF_LOAD_ERR_MAGIC = -3,
    ELF_LOAD_ERR_CLASS = -4,
    ELF_LOAD_ERR_DATAENC = -5,
    ELF_LOAD_ERR_VERSION = -6,
    ELF_LOAD_ERR_TYPE = -7,
    ELF_LOAD_ERR_MACHINE = -8,
    ELF_LOAD_ERR_PHDR = -9,
    ELF_LOAD_ERR_BOUNDS = -10,
    ELF_LOAD_ERR_RANGE = -11,
    ELF_LOAD_ERR_OVERLAP = -12,
    ELF_LOAD_ERR_MEM = -13,
} ElfLoadErr;

typedef struct {
    int      err;
    uint64_t entry;
    uint64_t user_rsp;
    uint64_t load_bias;
} ElfLoadResult;

/* Carga segmentos PT_LOAD en el espacio actual (CR3). */
int elf_load_binary(const uint8_t* file_data, size_t file_size, ElfLoadResult* out);

/*
 * Variante con base y pila explícitas (init + spawn de más binarios PIE).
 * map_base: para ET_DYN, load_bias = map_base - min_vaddr_phdr.
 * stack_top: RSP inicial = stack_top - 0x10; pila en [stack_top - stack_pages, stack_top).
 */
int elf_load_binary_ex(const uint8_t* file_data, size_t file_size, ElfLoadResult* out,
                       uint64_t map_base, uint64_t stack_top);

/* Salto a Ring 3 vía IRETQ (no retorna). RFLAGS típico: 0x2 (IF=0). */
void user_iretq_enter(uint64_t rip, uint64_t rsp, uint64_t rflags) __attribute__((noreturn));

static inline void elf_jump_user(uint64_t entry, uint64_t rsp, uint64_t rflags) {
    user_iretq_enter(entry, rsp, rflags);
}

#endif
