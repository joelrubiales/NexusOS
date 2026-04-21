#include "elf_loader.h"
#include "memory.h"
#include <stdint.h>

#define USER_LOAD_BASE 0x400000ULL
/* Tope del área de pila (exclusivo); RSP inicial unas filas más abajo, alineado 16. */
#define USER_STACK_TOP 0x80000000ULL
#define USER_STACK_PAGES 32u

#define USER_VA_MIN PAGE_SIZE
/* El mapa de arranque cubre el espacio lineal bajo 4 GiB. */
#define USER_VA_LIMIT 0x100000000ULL

static int u64_add_ok(uint64_t a, uint64_t b, uint64_t* sum) {
    if (~a < b)
        return 0;
    *sum = a + b;
    return 1;
}

static void kmemcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--)
        *d++ = *s++;
}

static void kmemset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--)
        *d++ = (unsigned char)c;
}

static uint64_t page_floor(uint64_t a) {
    return a & ~(PAGE_SIZE - 1);
}

static uint64_t page_ceil(uint64_t a) {
    return (a + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static uint64_t phdr_to_pte_flags(uint32_t p_flags) {
    uint64_t f = VMM_PAGE_PRESENT | VMM_PAGE_USER;
    if (p_flags & PF_W)
        f |= VMM_PAGE_RW;
    if (!(p_flags & PF_X))
        f |= VMM_PAGE_NX;
    return f;
}

static int elf_check_ehdr(const Elf64_Ehdr* e, size_t len) {
    if (len < sizeof(Elf64_Ehdr))
        return ELF_LOAD_ERR_TRUNC;
    if (e->e_ident[0] != ELF_EHDR_MAGIC0 || e->e_ident[1] != ELF_EHDR_MAGIC1 ||
        e->e_ident[2] != ELF_EHDR_MAGIC2 || e->e_ident[3] != ELF_EHDR_MAGIC3)
        return ELF_LOAD_ERR_MAGIC;
    if (e->e_ident[4] != ELFCLASS64)
        return ELF_LOAD_ERR_CLASS;
    if (e->e_ident[5] != ELFDATA2LSB)
        return ELF_LOAD_ERR_DATAENC;
    if (e->e_ident[6] != EV_CURRENT)
        return ELF_LOAD_ERR_VERSION;
    if (e->e_type != ET_EXEC && e->e_type != ET_DYN)
        return ELF_LOAD_ERR_TYPE;
    if (e->e_machine != EM_X86_64)
        return ELF_LOAD_ERR_MACHINE;
    if (e->e_phoff == 0 || e->e_phnum == 0 || e->e_phentsize != sizeof(Elf64_Phdr))
        return ELF_LOAD_ERR_PHDR;
    {
        uint64_t ph_end;
        if (!u64_add_ok(e->e_phoff, (uint64_t)e->e_phnum * (uint64_t)e->e_phentsize, &ph_end) ||
            ph_end > len)
            return ELF_LOAD_ERR_BOUNDS;
    }
    return ELF_LOAD_OK;
}

static int segment_fits_user(uint64_t vaddr, uint64_t memsz) {
    uint64_t end;
    if (vaddr < USER_VA_MIN || vaddr >= USER_VA_LIMIT)
        return 0;
    if (!u64_add_ok(vaddr, memsz, &end))
        return 0;
    if (end > USER_VA_LIMIT || end < vaddr)
        return 0;
    return 1;
}

/*
 * Comprueba que [a0,a0+sz) y [b0,b0+sz2) no se solapen (vacío si sz==0).
 */
static int ranges_disjoint(uint64_t a0, uint64_t sz, uint64_t b0, uint64_t sz2) {
    uint64_t a1, b1;
    if (sz == 0 || sz2 == 0)
        return 1;
    if (!u64_add_ok(a0, sz, &a1) || !u64_add_ok(b0, sz2, &b1))
        return 0;
    return (a1 <= b0 || b1 <= a0);
}

int elf_load_binary_ex(const uint8_t* file_data, size_t file_size, ElfLoadResult* out,
                       uint64_t map_base, uint64_t stack_top) {
    const Elf64_Ehdr* e;
    uint64_t load_bias = 0;
    uint64_t stack_low;
    unsigned pi;
    int err;

    if (!out)
        return ELF_LOAD_ERR_NULL;
    out->err = ELF_LOAD_ERR_NULL;
    out->entry = 0;
    out->user_rsp = 0;
    out->load_bias = 0;

    if (!file_data)
        return ELF_LOAD_ERR_NULL;

    e = (const Elf64_Ehdr*)(const void*)file_data;
    err = elf_check_ehdr(e, file_size);
    if (err != ELF_LOAD_OK) {
        out->err = (ElfLoadErr)err;
        return err;
    }

    if (e->e_type == ET_DYN) {
        uint64_t min_v = UINT64_MAX;
        uint16_t i;
        for (i = 0; i < e->e_phnum; i++) {
            const Elf64_Phdr* ph =
                (const Elf64_Phdr*)(const void*)(file_data + e->e_phoff + (uint64_t)i * e->e_phentsize);
            if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
                continue;
            if (ph->p_vaddr < min_v)
                min_v = ph->p_vaddr;
        }
        if (min_v == UINT64_MAX) {
            out->err = ELF_LOAD_ERR_BOUNDS;
            return ELF_LOAD_ERR_BOUNDS;
        }
        if (min_v > map_base) {
            out->err = ELF_LOAD_ERR_RANGE;
            return ELF_LOAD_ERR_RANGE;
        }
        load_bias = map_base - min_v;
    } else {
        /* ET_EXEC: solo en la ranura por defecto (init estático). */
        uint16_t i;
        if (map_base != USER_LOAD_BASE) {
            out->err = ELF_LOAD_ERR_RANGE;
            return ELF_LOAD_ERR_RANGE;
        }
        for (i = 0; i < e->e_phnum; i++) {
            const Elf64_Phdr* ph =
                (const Elf64_Phdr*)(const void*)(file_data + e->e_phoff + (uint64_t)i * e->e_phentsize);
            if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
                continue;
            if (ph->p_vaddr < USER_LOAD_BASE) {
                out->err = ELF_LOAD_ERR_RANGE;
                return ELF_LOAD_ERR_RANGE;
            }
        }
    }

    {
        uint64_t entry_va;
        if (!u64_add_ok(e->e_entry, load_bias, &entry_va)) {
            out->err = ELF_LOAD_ERR_RANGE;
            return ELF_LOAD_ERR_RANGE;
        }
        if (entry_va < USER_VA_MIN || entry_va >= USER_VA_LIMIT) {
            out->err = ELF_LOAD_ERR_RANGE;
            return ELF_LOAD_ERR_RANGE;
        }
    }

    stack_low = stack_top - (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    if (stack_low > stack_top) {
        out->err = ELF_LOAD_ERR_RANGE;
        return ELF_LOAD_ERR_RANGE;
    }

    /* Validación previa + comprobación de solapes entre segmentos y pila. */
    {
        uint16_t i, j;
        for (i = 0; i < e->e_phnum; i++) {
            const Elf64_Phdr* ph =
                (const Elf64_Phdr*)(const void*)(file_data + e->e_phoff + (uint64_t)i * e->e_phentsize);
            uint64_t v0, fo_end;
            if (ph->p_type != PT_LOAD)
                continue;
            if (!u64_add_ok(ph->p_vaddr, load_bias, &v0)) {
                out->err = ELF_LOAD_ERR_RANGE;
                return ELF_LOAD_ERR_RANGE;
            }
            if (!segment_fits_user(v0, ph->p_memsz)) {
                out->err = ELF_LOAD_ERR_RANGE;
                return ELF_LOAD_ERR_RANGE;
            }
            if (ph->p_filesz > ph->p_memsz) {
                out->err = ELF_LOAD_ERR_BOUNDS;
                return ELF_LOAD_ERR_BOUNDS;
            }
            if (!u64_add_ok(ph->p_offset, ph->p_filesz, &fo_end) || fo_end > file_size) {
                out->err = ELF_LOAD_ERR_BOUNDS;
                return ELF_LOAD_ERR_BOUNDS;
            }
            if (!ranges_disjoint(v0, ph->p_memsz, stack_low, stack_top - stack_low)) {
                out->err = ELF_LOAD_ERR_OVERLAP;
                return ELF_LOAD_ERR_OVERLAP;
            }
            for (j = 0; j < i; j++) {
                const Elf64_Phdr* qh = (const Elf64_Phdr*)(const void*)(file_data + e->e_phoff +
                                                                         (uint64_t)j * e->e_phentsize);
                uint64_t u0;
                if (qh->p_type != PT_LOAD)
                    continue;
                if (!u64_add_ok(qh->p_vaddr, load_bias, &u0)) {
                    out->err = ELF_LOAD_ERR_RANGE;
                    return ELF_LOAD_ERR_RANGE;
                }
                if (!ranges_disjoint(v0, ph->p_memsz, u0, qh->p_memsz)) {
                    out->err = ELF_LOAD_ERR_OVERLAP;
                    return ELF_LOAD_ERR_OVERLAP;
                }
            }
        }
    }

    /* Mapear segmentos. */
    {
        uint16_t i;
        for (i = 0; i < e->e_phnum; i++) {
            const Elf64_Phdr* ph =
                (const Elf64_Phdr*)(const void*)(file_data + e->e_phoff + (uint64_t)i * e->e_phentsize);
            uint64_t seg_begin, seg_end, pte_flags;
            uint64_t va;

            if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
                continue;

            if (!u64_add_ok(ph->p_vaddr, load_bias, &seg_begin)) {
                out->err = ELF_LOAD_ERR_RANGE;
                return ELF_LOAD_ERR_RANGE;
            }
            if (!u64_add_ok(seg_begin, ph->p_memsz, &seg_end)) {
                out->err = ELF_LOAD_ERR_RANGE;
                return ELF_LOAD_ERR_RANGE;
            }
            seg_begin = page_floor(seg_begin);
            seg_end = page_ceil(seg_end);
            if (seg_end < seg_begin) {
                out->err = ELF_LOAD_ERR_RANGE;
                return ELF_LOAD_ERR_RANGE;
            }

            pte_flags = phdr_to_pte_flags(ph->p_flags);

            for (va = seg_begin; va < seg_end; va += PAGE_SIZE) {
                uintptr_t phys = pmm_alloc_page();
                if (!phys) {
                    out->err = ELF_LOAD_ERR_MEM;
                    return ELF_LOAD_ERR_MEM;
                }
                vmm_map_page(va, (uint64_t)phys, pte_flags);
            }
        }
    }

    /* Copiar bytes del archivo y BSS (después de mapear todo). */
    {
        uint16_t i;
        for (i = 0; i < e->e_phnum; i++) {
            const Elf64_Phdr* ph =
                (const Elf64_Phdr*)(const void*)(file_data + e->e_phoff + (uint64_t)i * e->e_phentsize);
            uint64_t vbase;
            void* dst;
            if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
                continue;
            if (!u64_add_ok(ph->p_vaddr, load_bias, &vbase)) {
                out->err = ELF_LOAD_ERR_RANGE;
                return ELF_LOAD_ERR_RANGE;
            }
            dst = (void*)(uintptr_t)vbase;
            if (ph->p_filesz)
                kmemcpy(dst, file_data + ph->p_offset, (size_t)ph->p_filesz);
            if (ph->p_memsz > ph->p_filesz)
                kmemset((uint8_t*)dst + ph->p_filesz, 0, (size_t)(ph->p_memsz - ph->p_filesz));
        }
    }

    /* Pila usuario. */
    for (pi = 0; pi < USER_STACK_PAGES; pi++) {
        uint64_t va = stack_low + (uint64_t)pi * PAGE_SIZE;
        uintptr_t phys = pmm_alloc_page();
        if (!phys) {
            out->err = ELF_LOAD_ERR_MEM;
            return ELF_LOAD_ERR_MEM;
        }
        vmm_map_page(va, (uint64_t)phys, VMM_PAGE_PRESENT | VMM_PAGE_RW | VMM_PAGE_USER | VMM_PAGE_NX);
    }

    out->entry = e->e_entry + load_bias;
    out->user_rsp = stack_top - 0x10u;
    out->load_bias = load_bias;
    out->err = ELF_LOAD_OK;
    return ELF_LOAD_OK;
}

int elf_load_binary(const uint8_t* file_data, size_t file_size, ElfLoadResult* out) {
    return elf_load_binary_ex(file_data, file_size, out, USER_LOAD_BASE, USER_STACK_TOP);
}
