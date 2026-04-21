#include "memory.h"
#include <stdint.h>

/* x86-64: PML4 → PDPT → PD → PT. El stage2 usa páginas de 2 MiB (PTE_PS); al mapear 4 KiB
 * hay que partir la entrada PD si hace falta. Mapa identidad: el kernel usa phys == virt. */

#define PTE_P   (1ULL << 0)
#define PTE_RW  (1ULL << 1)
#define PTE_PS  (1ULL << 7)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define MASK_2M       0x000FFFFFFFFFE000ULL

static uintptr_t vmm_read_cr3(void) {
    uintptr_t x;
    __asm__ volatile("mov %%cr3, %0" : "=r"(x));
    return x & PTE_ADDR_MASK;
}

static void vmm_invlpg(uintptr_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"((void*)va) : "memory");
}

static void vmm_page_zero(uintptr_t p) {
    uint64_t* q = (uint64_t*)p;
    unsigned i;
    for (i = 0; i < 512; i++) q[i] = 0;
}

static uint64_t* vmm_tbl(uintptr_t phys) { return (uint64_t*)phys; }

/* Convierte entrada PD de 2 MiB en tabla PT con 512 entradas 4 KiB (misma cobertura). */
static int vmm_split_pd_if_huge(uint64_t* pd, unsigned pd_idx) {
    uint64_t e = pd[pd_idx];
    if (!(e & PTE_P) || !(e & PTE_PS))
        return 1;
    {
        uint64_t base = e & MASK_2M;
        uintptr_t pt_phys = pmm_alloc_page();
        unsigned i;
        uint64_t* pt;
        if (!pt_phys)
            return 0;
        vmm_page_zero(pt_phys);
        pt = vmm_tbl(pt_phys);
        for (i = 0; i < 512; i++)
            pt[i] = (base + ((uint64_t)i << 12)) | PTE_P | PTE_RW;
        pd[pd_idx] = pt_phys | PTE_P | PTE_RW;
    }
    return 1;
}

static uintptr_t vmm_ensure_child(uint64_t* parent, unsigned idx, uint64_t entry_flags) {
    uint64_t e = parent[idx];
    if (e & PTE_P)
        return (uintptr_t)(e & PTE_ADDR_MASK);
    {
        uintptr_t n = pmm_alloc_page();
        if (!n)
            return 0;
        vmm_page_zero(n);
        parent[idx] = n | entry_flags;
        return n;
    }
}

void vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    unsigned i4 = (unsigned)((virt_addr >> 39) & 0x1FFu);
    unsigned i3 = (unsigned)((virt_addr >> 30) & 0x1FFu);
    unsigned i2 = (unsigned)((virt_addr >> 21) & 0x1FFu);
    unsigned i1 = (unsigned)((virt_addr >> 12) & 0x1FFu);
    uint64_t* pml4 = vmm_tbl(vmm_read_cr3());
    uintptr_t pdpt_p, pd_p, pt_p;
    uint64_t* pdpt;
    uint64_t* pd;
    uint64_t* pt;
    uint64_t pte_val =
        (phys_addr & PTE_ADDR_MASK) | PTE_P | (flags & 0xFFFULL) | (flags & (1ULL << 63));

    pdpt_p = vmm_ensure_child(pml4, i4, PTE_P | PTE_RW);
    if (!pdpt_p) return;
    pdpt = vmm_tbl(pdpt_p);

    pd_p = vmm_ensure_child(pdpt, i3, PTE_P | PTE_RW);
    if (!pd_p) return;
    pd = vmm_tbl(pd_p);

    if ((pd[i2] & PTE_P) && (pd[i2] & PTE_PS)) {
        if (!vmm_split_pd_if_huge(pd, i2))
            return;
    }

    pt_p = vmm_ensure_child(pd, i2, PTE_P | PTE_RW);
    if (!pt_p) return;
    pt = vmm_tbl(pt_p);

    pt[i1] = pte_val;
    vmm_invlpg((uintptr_t)virt_addr);
}

void vmm_identity_map_range(uint64_t phys_start, uint64_t len, uint64_t flags) {
    uint64_t a, end_abs, end_pg;
    if (len == 0) return;
    a = phys_start & ~(PAGE_SIZE - 1);
    end_abs = phys_start + len;
    if (end_abs < phys_start)
        end_abs = (uint64_t)-1;
    /* Redondeo superior a página para cubrir el último byte de la región. */
    end_pg = (end_abs + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    while (a < end_pg) {
        vmm_map_page(a, a, flags);
        a += PAGE_SIZE;
    }
}
