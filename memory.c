#include "memory.h"
#include <stdint.h>

extern char __kernel_image_end[];

/* ── Bitmap PMM: 1 bit por página 4 KiB ───────────────────────────── */
#define BITMAP_BYTES ((PMM_PAGES + 7) / 8)

static unsigned char pmm_bitmap[BITMAP_BYTES];

static inline void bm_set(unsigned pn) {
    pmm_bitmap[pn / 8] |= (unsigned char)(1u << (pn % 8));
}
static inline void bm_clr(unsigned pn) {
    pmm_bitmap[pn / 8] &= (unsigned char)~(1u << (pn % 8));
}
static inline int bm_tst(unsigned pn) {
    return (pmm_bitmap[pn / 8] >> (pn % 8)) & 1;
}

void pmm_reserve_multiboot_info(uint32_t mbi_phys) {
    const volatile uint32_t* m = (const volatile uint32_t*)(uintptr_t)mbi_phys;
    uint32_t total;
    size_t len;
    if (mbi_phys == 0)
        return;
    total = m[0];
    if (total < 8u || total > (1024u * 1024u))
        return;
    len = (size_t)total;
    pmm_reserve_phys_range((uintptr_t)mbi_phys, len);
}

/* Mapa identidad PML4→PT 4 KiB para todo el búfer lineal del framebuffer (evita #PF en LFB alto). */
void memory_map_framebuffer_identity(uint64_t framebuffer_phys, uint32_t pitch, uint32_t height,
                                     uint64_t pte_flags) {
    uint64_t nbytes, q;
    if (framebuffer_phys == 0 || pitch < 4u || height == 0)
        return;
    q = (uint64_t)pitch * (uint64_t)height;
    if (q / (uint64_t)pitch != (uint64_t)height)
        return;
    nbytes = q;
    vmm_identity_map_range(framebuffer_phys, nbytes, pte_flags);
    __asm__ volatile("mfence" ::: "memory");
}

void pmm_reserve_phys_range(uintptr_t start, size_t len) {
    uintptr_t end;
    if (len == 0 || start == 0) return;
    end = start + len;
    if (end < start) end = (uintptr_t)-1;
    for (uintptr_t a = start & ~(PAGE_SIZE - 1); a < end && a < ((uintptr_t)PMM_PAGES * PAGE_SIZE);
         a += PAGE_SIZE) {
        unsigned pn = (unsigned)(a / PAGE_SIZE);
        if (pn < PMM_PAGES) bm_set(pn);
    }
}

void memory_init(void) {
    unsigned i;
    for (i = 0; i < BITMAP_BYTES; i++) pmm_bitmap[i] = 0xFF;
    /* 32 MiB .. fin: candidatos libres */
    for (i = (unsigned)(32ULL * 1024ULL * 1024ULL / PAGE_SIZE); i < PMM_PAGES; i++)
        bm_clr(i);
    /* Arena kmalloc @ 32 MiB (motor gráfico + doble búfer; ver KHEAP_SIZE). */
    {
        unsigned h0 = (unsigned)(KHEAP_PHYS_START / PAGE_SIZE);
        unsigned hn = (unsigned)((KHEAP_PHYS_START + KHEAP_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
        for (i = h0; i < hn && i < PMM_PAGES; i++) bm_set(i);
    }
    kheap_init();

    /* Imagen del kernel @ KERNEL_LOAD_PHYS .. __kernel_image_end. */
    {
        uintptr_t kbeg = KERNEL_LOAD_PHYS;
        uintptr_t kend = (uintptr_t)&__kernel_image_end;
        if (kend > kbeg)
            pmm_reserve_phys_range(kbeg, (size_t)(kend - kbeg));
    }

    /* Stage2: tablas en ~0x70000, BootInfo 0x5000, scratch VBE 0x6200 — todo < 32 MiB ya
     * marcado; reservas explícitas documentan el contrato. */
    pmm_reserve_phys_range(0x5000ull, 4096);
    pmm_reserve_phys_range(0x6200ull, 4096);
    pmm_reserve_phys_range(0x70000ull, 6 * 4096);
}

uintptr_t pmm_alloc_page(void) {
    unsigned i;
    for (i = (unsigned)(32ULL * 1024ULL * 1024ULL / PAGE_SIZE); i < PMM_PAGES; i++) {
        if (!bm_tst(i)) {
            bm_set(i);
            return (uintptr_t)i * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free_page(uintptr_t phys) {
    if (phys == 0 || (phys & (PAGE_SIZE - 1))) return;
    unsigned pn = (unsigned)(phys / PAGE_SIZE);
    if (pn >= PMM_PAGES) return;
    bm_clr(pn);
}

unsigned pmm_free_pages(void) {
    unsigned c = 0, i;
    for (i = 0; i < PMM_PAGES; i++)
        if (!bm_tst(i)) c++;
    return c;
}

unsigned pmm_total_pages(void) { return PMM_PAGES; }
