#include "smp.h"
#include "memory.h"
#include "nexus.h"
#include <stddef.h>
#include <stdint.h>

#define SMP_TRAMP_PHYS   0x8000ULL
#define SMP_PARAM_PHYS   0x8FE0ULL
#define SMP_TRAMP_MAGIC  0x4E55584Eu /* 'NXUN' */

#define MB2_TAG_ACPI_OLD 14u
#define MB2_TAG_ACPI_NEW 15u

#define IA32_APIC_BASE_MSR 0x1Bu

#define LAPIC_REG_ID     0x20u
#define LAPIC_REG_VER    0x30u
#define LAPIC_REG_SVR    0xF0u
#define LAPIC_REG_ICRLO  0x300u
#define LAPIC_REG_ICRHI  0x310u

/* ICR: bits 8-10 delivery; 14 nivel; 15 trigger. INIT=101, Start-up=110. */
#define ICR_DM_INIT    0x500u
#define ICR_DM_STARTUP 0x600u
#define ICR_DEST_PHYS  0u
#define ICR_LEVEL_ASSERT   0x4000u
#define ICR_LEVEL_DEASSERT 0u
#define ICR_EDGE       0u
#define SIPI_PAGE_VEC  0x08u /* vector en bits 0-7 → 0x8000 */

#define MADT_TYPE_LAPIC 0u
#define MADT_TYPE_IOAPIC 1u

typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} AcpiTableHeader;

typedef struct __attribute__((packed)) {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} AcpiRsdp20;

typedef struct __attribute__((packed)) {
    AcpiTableHeader h;
    uint32_t        lapic_mmio;
    uint32_t        flags;
} AcpiMadt;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t cr3;
    uint64_t stack_top;
    uint64_t entry64;
} SmpTrampParams;

unsigned     smp_cpu_count = 1u;
uint32_t     smp_ioapic_phys;
spinlock_t   smp_kmalloc_lock;
spinlock_t   smp_console_lock;

static uint32_t       lapic_mmio_base = 0xFEE00000u;
static uint8_t        cpu_apic_ids[SMP_MAX_CPUS];
static unsigned       cpu_list_len;
static uint32_t       bsp_apic_id;
static volatile uint32_t smp_boot_done;

extern void smp_ap_entry(void);

extern char _binary_smp_trampoline_bin_start[];
extern char _binary_smp_trampoline_bin_end[];

static uint8_t ap_stack_pool[(SMP_MAX_CPUS - 1u) * 16384] __attribute__((aligned(16)));

static uint32_t acpi_byte_sum(const void* p, uint32_t len) {
    const uint8_t* b = (const uint8_t*)p;
    uint32_t s = 0, i;
    for (i = 0; i < len; i++)
        s += b[i];
    return s & 0xFFu;
}

static int acpi_valid(const void* p, uint32_t len) {
    if (!p || len < 2)
        return 0;
    return (acpi_byte_sum(p, len) & 0xFFu) == 0u;
}

static void rdmsr_u32(uint32_t msr, uint32_t* lo, uint32_t* hi) {
    uint32_t a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(msr));
    *lo = a;
    *hi = d;
}

static void wrmsr_u32(uint32_t msr, uint32_t lo, uint32_t hi) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static uintptr_t read_cr3_phys(void) {
    uintptr_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v & 0x000FFFFFFFFFF000ULL;
}

static void mem_cpy(void* d, const void* s, size_t n) {
    uint8_t* a = (uint8_t*)d;
    const uint8_t* b = (const uint8_t*)s;
    while (n--)
        *a++ = *b++;
}

static uintptr_t mb2_find_rsdp(uint32_t mbi_phys) {
    const volatile uint32_t* m = (const volatile uint32_t*)(uintptr_t)mbi_phys;
    uint32_t total_size;
    size_t   off;

    if (mbi_phys == 0 || (mbi_phys & 7u) != 0)
        return 0;

    total_size = m[0];
    if (total_size < 16u || total_size > (1024u * 1024u))
        return 0;

    for (off = 8; off + 8 <= (size_t)total_size;) {
        uint16_t tag_type = *(const volatile uint16_t*)(uintptr_t)(mbi_phys + off);
        uint32_t tag_size = *(const volatile uint32_t*)(uintptr_t)(mbi_phys + off + 4);
        size_t   next;

        if (tag_size < 8u || off + (size_t)tag_size > (size_t)total_size)
            break;
        next = ((size_t)tag_size + 7u) & ~(size_t)7u;
        if (next == 0)
            break;

        if (((uint32_t)tag_type == MB2_TAG_ACPI_OLD || (uint32_t)tag_type == MB2_TAG_ACPI_NEW) &&
            tag_size >= 28u) {
            /* RSDP copiado tras cabecera del tag (8 bytes). */
            return (uintptr_t)(mbi_phys + off + 8);
        }
        if ((uint32_t)tag_type == 0u)
            break;
        off += next;
    }
    return 0;
}

static uintptr_t scan_rsdp_bios(void) {
    uintptr_t a;
    /* Región BIOS ROM (GRUB/sin tag ACPI en MBI). */
    for (a = 0xE0000u; a < 0x100000u; a += 16u) {
        const char* s = (const char*)(uintptr_t)a;
        if (s[0] == 'R' && s[1] == 'S' && s[2] == 'D' && s[3] == ' ' && s[4] == 'P' && s[5] == 'T' && s[6] == 'R' &&
            s[7] == ' ')
            return a;
    }
    return 0;
}

static const AcpiTableHeader* acpi_map_tbl(uintptr_t phys) {
    if (phys == 0)
        return 0;
    return (const AcpiTableHeader*)(uintptr_t)phys;
}

static int rsdp_ok(const AcpiRsdp20* r) {
    if (!r)
        return 0;
    if (r->signature[0] != 'R' || r->signature[1] != 'S' || r->signature[2] != 'D' || r->signature[3] != ' ' ||
        r->signature[4] != 'P' || r->signature[5] != 'T' || r->signature[6] != 'R' || r->signature[7] != ' ')
        return 0;
    if ((acpi_byte_sum(r, 20u) & 0xFFu) != 0u)
        return 0;
    if (r->revision >= 2u) {
        if (r->length < 36u)
            return 0;
        if ((acpi_byte_sum(r, r->length) & 0xFFu) != 0u)
            return 0;
    }
    return 1;
}

static const AcpiMadt* acpi_find_madt(const AcpiRsdp20* rsdp) {
    uintptr_t rsdt;
    uint32_t  n;
    size_t    i;

    if (!rsdp_ok(rsdp))
        return 0;

    if (rsdp->revision >= 2u && rsdp->xsdt_address != 0ull) {
        const AcpiTableHeader* xh = acpi_map_tbl((uintptr_t)rsdp->xsdt_address);
        if (!xh || !acpi_valid(xh, xh->length))
            goto use_rsdt;
        n = (xh->length - sizeof(AcpiTableHeader)) / 8u;
        for (i = 0; i < n; i++) {
            uint64_t ent = *(const uint64_t*)((const uint8_t*)xh + sizeof(AcpiTableHeader) + i * 8u);
            const AcpiTableHeader* th = acpi_map_tbl((uintptr_t)ent);
            if (th && th->signature[0] == 'A' && th->signature[1] == 'P' && th->signature[2] == 'I' &&
                th->signature[3] == 'C' && acpi_valid(th, th->length))
                return (const AcpiMadt*)th;
        }
    }
use_rsdt:
    rsdt = (uintptr_t)rsdp->rsdt_address;
    {
        const AcpiTableHeader* rh = acpi_map_tbl(rsdt);
        if (!rh || !acpi_valid(rh, rh->length))
            return 0;
        n = (rh->length - sizeof(AcpiTableHeader)) / 4u;
        for (i = 0; i < n; i++) {
            uint32_t ent = *(const uint32_t*)((const uint8_t*)rh + sizeof(AcpiTableHeader) + i * 4u);
            const AcpiTableHeader* th = acpi_map_tbl((uintptr_t)ent);
            if (th && th->signature[0] == 'A' && th->signature[1] == 'P' && th->signature[2] == 'I' &&
                th->signature[3] == 'C' && acpi_valid(th, th->length))
                return (const AcpiMadt*)th;
        }
    }
    return 0;
}

static void madt_collect(const AcpiMadt* madt, uint32_t* ioapic_out) {
    const uint8_t* p = (const uint8_t*)madt + sizeof(AcpiMadt);
    const uint8_t* end = (const uint8_t*)madt + madt->h.length;

    cpu_list_len = 0;
    *ioapic_out = 0;
    lapic_mmio_base = madt->lapic_mmio;
    if (lapic_mmio_base == 0u)
        lapic_mmio_base = 0xFEE00000u;

    while (p + 2 <= end) {
        uint8_t t = p[0], l = p[1];
        if (l < 2u || (size_t)(end - p) < (size_t)l)
            break;
        if (t == MADT_TYPE_LAPIC && l >= 8u) {
            uint8_t apic_id = p[3];
            uint32_t flags = *(const uint32_t*)(p + 4);
            if ((flags & 1u) && cpu_list_len < SMP_MAX_CPUS) {
                cpu_apic_ids[cpu_list_len] = apic_id;
                cpu_list_len++;
            }
        } else if (t == MADT_TYPE_IOAPIC && l >= 12u && *ioapic_out == 0u) {
            *ioapic_out = *(const uint32_t*)(p + 4);
        }
        p += l;
    }
}

static void lapic_mmio_write(uint32_t reg, uint32_t v) {
    volatile uint32_t* mm = (volatile uint32_t*)(uintptr_t)((uint64_t)lapic_mmio_base + reg);
    *mm = v;
}

static uint32_t lapic_mmio_read(uint32_t reg) {
    volatile uint32_t* mm = (volatile uint32_t*)(uintptr_t)((uint64_t)lapic_mmio_base + reg);
    return *mm;
}

static void lapic_icr_wait(void) {
    while (lapic_mmio_read(LAPIC_REG_ICRLO) & (1u << 12))
        __asm__ volatile("pause");
}

static void lapic_send_ipi(uint8_t dest_apic_id, uint32_t lo_val) {
    lapic_icr_wait();
    lapic_mmio_write(LAPIC_REG_ICRHI, (uint32_t)dest_apic_id << 24);
    lapic_mmio_write(LAPIC_REG_ICRLO, lo_val);
    lapic_icr_wait();
}

static void lapic_bsp_enable(void) {
    uint32_t lo, hi;
    rdmsr_u32(IA32_APIC_BASE_MSR, &lo, &hi);
    lo &= 0xFFFFF000u;
    lo |= (1u << 11); /* EN */
    wrmsr_u32(IA32_APIC_BASE_MSR, lo, hi);
    lapic_mmio_base = lo & 0xFFFFF000u;

    lapic_mmio_write(LAPIC_REG_SVR, (1u << 8) | 0xFFu);
}

uint32_t smp_lapic_read_id(void) {
    return lapic_mmio_read(LAPIC_REG_ID) >> 24;
}

uint32_t smp_bsp_apic_id(void) { return bsp_apic_id; }

void spinlock_init(spinlock_t* lk) {
    if (lk)
        lk->v = 0;
}

void spinlock_lock(spinlock_t* lk) {
    uint32_t old;
    if (!lk)
        return;
    for (;;) {
        while (lk->v)
            __asm__ volatile("pause");
        old = 1u;
        __asm__ volatile("lock xchg %0, %1" : "+r"(old), "+m"(lk->v) : : "memory");
        if (old == 0u)
            return;
    }
}

void spinlock_unlock(spinlock_t* lk) {
    if (!lk)
        return;
    __asm__ volatile("movl $0, %0" : "=m"(lk->v) : : "memory");
}

static void udelay(unsigned us) {
    volatile unsigned n = us * 2000u;
    while (n--)
        __asm__ volatile("pause");
}

void smp_ap_entry(void) {
    uint32_t lo, hi;
    /* Mismo APIC base MSR que el BSP (mapa identidad). */
    rdmsr_u32(IA32_APIC_BASE_MSR, &lo, &hi);
    lapic_mmio_base = lo & 0xFFFFF000u;
    lapic_mmio_write(LAPIC_REG_SVR, (1u << 8) | 0xFFu);

    __asm__ volatile("mfence" ::: "memory");
    smp_boot_done = 1u;
    for (;;) {
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
}

static void install_trampoline_and_boot(uint32_t dest_apic_id, void* stack_top) {
    SmpTrampParams* tp = (SmpTrampParams*)(uintptr_t)SMP_PARAM_PHYS;
    size_t tsz = (size_t)((uintptr_t)_binary_smp_trampoline_bin_end - (uintptr_t)_binary_smp_trampoline_bin_start);

    if (tsz > 4096u)
        return;

    mem_cpy((void*)(uintptr_t)SMP_TRAMP_PHYS, _binary_smp_trampoline_bin_start, tsz);

    tp->magic = SMP_TRAMP_MAGIC;
    tp->cr3 = (uint32_t)read_cr3_phys();
    tp->stack_top = (uint64_t)(uintptr_t)stack_top;
    tp->entry64 = (uint64_t)(uintptr_t)(void*)&smp_ap_entry;

    __asm__ volatile("mfence" ::: "memory");

    smp_boot_done = 0u;

    lapic_send_ipi((uint8_t)dest_apic_id,
                   ICR_DM_INIT | ICR_DEST_PHYS | ICR_LEVEL_ASSERT | ICR_EDGE);
    udelay(10000);
    lapic_send_ipi((uint8_t)dest_apic_id, ICR_DM_INIT | ICR_DEST_PHYS | ICR_LEVEL_DEASSERT | ICR_EDGE);
    udelay(10000);

    lapic_send_ipi((uint8_t)dest_apic_id,
                   ICR_DM_STARTUP | ICR_DEST_PHYS | ICR_LEVEL_ASSERT | ICR_EDGE | (uint32_t)SIPI_PAGE_VEC);
    udelay(200);
    lapic_send_ipi((uint8_t)dest_apic_id,
                   ICR_DM_STARTUP | ICR_DEST_PHYS | ICR_LEVEL_ASSERT | ICR_EDGE | (uint32_t)SIPI_PAGE_VEC);

    {
        unsigned spin = 1000000u;
        while (smp_boot_done == 0u && spin--)
            __asm__ volatile("pause");
    }
}

void smp_init(uint32_t mb2_info_phys) {
    uintptr_t rsdp_pa = mb2_find_rsdp(mb2_info_phys);
    const AcpiRsdp20* rsdp;
    const AcpiMadt* madt;
    uint32_t ioapic_phys = 0;
    unsigned ap_idx, i;
    uint64_t rflags_save;

    __asm__ volatile("pushfq; pop %0" : "=r"(rflags_save));
    __asm__ volatile("cli");

    spinlock_init(&smp_kmalloc_lock);
    spinlock_init(&smp_console_lock);

    if (rsdp_pa == 0)
        rsdp_pa = scan_rsdp_bios();
    if (rsdp_pa == 0) {
        smp_cpu_count = 1u;
        goto smp_done;
    }

    rsdp = (const AcpiRsdp20*)(uintptr_t)rsdp_pa;
    madt = acpi_find_madt(rsdp);
    if (!madt) {
        smp_cpu_count = 1u;
        goto smp_done;
    }

    madt_collect(madt, &ioapic_phys);
    smp_ioapic_phys = ioapic_phys;

    if (cpu_list_len == 0u) {
        smp_cpu_count = 1u;
        goto smp_done;
    }

    smp_cpu_count = cpu_list_len;
    lapic_bsp_enable();
    bsp_apic_id = smp_lapic_read_id();

    ap_idx = 0;
    for (i = 0; i < cpu_list_len; i++) {
        uint32_t aid = cpu_apic_ids[i];
        if (aid == bsp_apic_id)
            continue;
        if (ap_idx >= SMP_MAX_CPUS - 1u)
            break;
        {
            uint8_t* stk_base = ap_stack_pool + ap_idx * 16384u;
            void* stack_top = (void*)(stk_base + 16384u - 16u);
            install_trampoline_and_boot(aid, stack_top);
        }
        ap_idx++;
    }

smp_done:
    __asm__ volatile("push %0; popfq" : : "r"(rflags_save));
}
