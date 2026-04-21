#include "xhci.h"
#include "pci.h"
#include "memory.h"
#include "nexus.h"
#include "event_queue.h"
#include <stddef.h>
#include <stdint.h>

#define MB2_TAG_ACPI_OLD 14u
#define MB2_TAG_ACPI_NEW 15u

#define USBCMD_RS    (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)
#define USBSTS_CNR   (1u << 11)

#define PORTSC_CCS   (1u << 0)
#define PORTSC_PED   (1u << 1)
#define PORTSC_PR    (1u << 4)
#define PORTSC_PP    (1u << 9)
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK  0xFu
#define PORTSC_PRC   (1u << 21)
#define PORTSC_CSC   (1u << 17)

#define XHCI_EXT_CAPS_USB_LEGACY 1u

#define TRB_LINK     6u
#define TRB_TC       (1u << 1)

#define TRB_NORMAL   1u
#define TRB_SETUP    2u
#define TRB_DATA     3u
#define TRB_STATUS   4u
#define TRB_CMD_NOOP 8u
#define TRB_CMD_ENABLE_SLOT  9u
#define TRB_CMD_ADDR_DEV     11u
#define TRB_CMD_CONF_EP      12u
#define TRB_CMD_EVAL_CTX     13u

#define TRB_EVT_TRANSFER 32u
#define TRB_EVT_COMP     33u

#define TRB_CTRL_CYC  (1u << 0)
#define TRB_CTRL_IOC  (1u << 5)
#define TRB_CTRL_CH   (1u << 4)
#define TRB_CTRL_IDT  (1u << 6)
#define TRB_CTRL_DIR_IN (1u << 16)

#define SETUP_TRT_NO   0u
#define SETUP_TRT_OUT  2u
#define SETUP_TRT_IN   3u

#define CC_SUCCESS 1u

#define LEGACY_BIOS_SEM (1u << 16)
#define LEGACY_OS_SEM   (1u << 24)

#define CMD_TRBS 256u
#define EV_TRBS  256u
#define EP0_TRBS 256u
#define EPI_TRBS 256u

#define TRB_CYC_BIT(c) ((uint32_t)(c) & 1u)

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
    uint64_t base;
    uint16_t pci_segment;
    uint8_t  bus_start;
    uint8_t  bus_end;
    uint32_t reserved;
} McfgAlloc;

typedef struct __attribute__((packed)) {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} XhciTrb;

static struct {
    int          valid;
    uint64_t     ecam_base;
    uint16_t     segment;
    uint8_t      bus_lo;
    uint8_t      bus_hi;
} g_mcfg;

volatile int xhci_usb_mouse_active = 0;

static struct {
    int                  ok;
    volatile uint32_t*   cap;
    volatile uint32_t*   op;
    volatile uint32_t*   db;
    volatile uint32_t*   rt;
    uint8_t              caplen;
    uint32_t             hcs1;
    uint32_t             hcs2;
    uint32_t             hcc;
    uint32_t             n_ports;
    uint32_t             ctx_bytes;
    uint32_t             scratch;

    volatile uint64_t*   dcbaa;
    uint64_t             dcbaa_phys;

    XhciTrb*             cr;
    uint64_t             cr_phys;
    uint32_t             cr_enq;
    int                  cr_cyc;

    XhciTrb*             er;
    uint64_t             er_phys;
    uint32_t             er_deq;
    int                  er_cyc;

    uint8_t*             dev_out_pg;
    uint64_t             dev_out_phys;
    uint8_t*             inctx_pg;
    uint64_t             inctx_phys;
    uint8_t*             ep0_ring_pg;
    uint64_t             ep0_ring_phys;
    uint8_t*             ep_in_ring_pg;
    uint64_t             ep_in_ring_phys;
    uint8_t*             ctrl_buf_pg;
    uint64_t             ctrl_buf_phys;

    uint32_t             slot_id;
    uint8_t              root_port;
    uint8_t              port_speed;
    uint16_t             ep0_mps;
    uint8_t              conf_val;
    uint8_t              ep_intr_id;
    uint16_t             ep_intr_mps;
    uint8_t              ep_interval;
    uint8_t              hid_iface;

    uint32_t             ep0_enq;
    int                  ep0_cyc;
    uint32_t             epi_enq;
    int                  epi_cyc;

    int                  scr_w;
    int                  scr_h;
} XH;

static uint32_t acpi_byte_sum(const void* p, uint32_t len) {
    const uint8_t* b = (const uint8_t*)p;
    uint32_t s = 0, i;
    for (i = 0; i < len; i++)
        s += b[i];
    return s & 0xFFu;
}

static int acpi_tbl_valid(const AcpiTableHeader* h) {
    if (!h || h->length < sizeof(AcpiTableHeader))
        return 0;
    return (acpi_byte_sum(h, h->length) & 0xFFu) == 0u;
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

        if (((uint32_t)tag_type == MB2_TAG_ACPI_OLD || (uint32_t)tag_type == MB2_TAG_ACPI_NEW) && tag_size >= 28u)
            return (uintptr_t)(mbi_phys + off + 8);
        if ((uint32_t)tag_type == 0u)
            break;
        off += next;
    }
    return 0;
}

static const AcpiTableHeader* acpi_find_mcfg_rsdt(const AcpiRsdp20* rsdp) {
    uintptr_t rsdt;
    uint32_t  n;
    size_t    i;

    if (!rsdp_ok(rsdp))
        return 0;

    if (rsdp->revision >= 2u && rsdp->xsdt_address != 0ull) {
        const AcpiTableHeader* xh = (const AcpiTableHeader*)(uintptr_t)rsdp->xsdt_address;
        if (xh && acpi_tbl_valid(xh)) {
            n = (xh->length - sizeof(AcpiTableHeader)) / 8u;
            for (i = 0; i < n; i++) {
                uint64_t ent =
                    *(const uint64_t*)((const uint8_t*)xh + sizeof(AcpiTableHeader) + i * 8u);
                const AcpiTableHeader* th = (const AcpiTableHeader*)(uintptr_t)ent;
                if (th && th->signature[0] == 'M' && th->signature[1] == 'C' && th->signature[2] == 'F' &&
                    th->signature[3] == 'G' && acpi_tbl_valid(th))
                    return th;
            }
        }
    }

    rsdt = (uintptr_t)rsdp->rsdt_address;
    {
        const AcpiTableHeader* rh = (const AcpiTableHeader*)rsdt;
        if (!rh || !acpi_tbl_valid(rh))
            return 0;
        n = (rh->length - sizeof(AcpiTableHeader)) / 4u;
        for (i = 0; i < n; i++) {
            uint32_t ent = *(const uint32_t*)((const uint8_t*)rh + sizeof(AcpiTableHeader) + i * 4u);
            const AcpiTableHeader* th = (const AcpiTableHeader*)(uintptr_t)ent;
            if (th && th->signature[0] == 'M' && th->signature[1] == 'C' && th->signature[2] == 'F' &&
                th->signature[3] == 'G' && acpi_tbl_valid(th))
                return th;
        }
    }
    return 0;
}

static void xhci_parse_mcfg(uint32_t mb2_phys) {
    uintptr_t rsdp_pa = mb2_find_rsdp(mb2_phys);
    const AcpiRsdp20* rsdp;
    const AcpiTableHeader* mcfg;
    const uint8_t* p;
    uint32_t hdr_end, rem;

    g_mcfg.valid = 0;
    if (rsdp_pa == 0)
        return;

    rsdp = (const AcpiRsdp20*)rsdp_pa;
    mcfg = acpi_find_mcfg_rsdt(rsdp);
    if (!mcfg)
        return;

    hdr_end = sizeof(AcpiTableHeader) + 8u;
    if (mcfg->length < hdr_end + sizeof(McfgAlloc))
        return;

    p = (const uint8_t*)mcfg;
    rem = mcfg->length - hdr_end;
    if (rem < sizeof(McfgAlloc))
        return;

    {
        const McfgAlloc* a = (const McfgAlloc*)(p + hdr_end);
        g_mcfg.ecam_base = a->base;
        g_mcfg.segment = a->pci_segment;
        g_mcfg.bus_lo = a->bus_start;
        g_mcfg.bus_hi = a->bus_end;
        g_mcfg.valid = (a->base != 0ull && a->bus_start <= a->bus_end) ? 1 : 0;
    }
}

static uint32_t xhci_ecam_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint64_t addr;
    if (!g_mcfg.valid || bus < g_mcfg.bus_lo || bus > g_mcfg.bus_hi)
        return 0xFFFFFFFFu;
    addr = g_mcfg.ecam_base + (((uint64_t)bus) << 20) + ((uint64_t)(slot & 0x1Fu) << 15) +
           ((uint64_t)(func & 7u) << 12) + (uint64_t)(off & 0xFFCu);
    return *(const volatile uint32_t*)(uintptr_t)addr;
}

static uint64_t xhci_probe_bar_mem(const PciFunction* f, uint32_t* size_out) {
    uint32_t lo, hi, orig_lo, orig_hi;
    uint64_t mmio;
    int      mem64;

    lo = pci_read32(f->bus, f->slot, f->func, 0x10);
    if ((lo & 1u) != 0)
        return 0;

    mem64 = ((lo >> 1) & 3u) == 2u;
    orig_lo = lo;
    if (mem64)
        orig_hi = pci_read32(f->bus, f->slot, f->func, 0x14);
    else
        orig_hi = 0;

    pci_write32(f->bus, f->slot, f->func, 0x10, 0xFFFFFFFFu);
    if (mem64)
        pci_write32(f->bus, f->slot, f->func, 0x14, 0xFFFFFFFFu);

    hi = mem64 ? pci_read32(f->bus, f->slot, f->func, 0x14) : 0;
    lo = pci_read32(f->bus, f->slot, f->func, 0x10);

    pci_write32(f->bus, f->slot, f->func, 0x10, orig_lo);
    if (mem64)
        pci_write32(f->bus, f->slot, f->func, 0x14, orig_hi);

    lo &= ~0xFULL;
    if (mem64) {
        mmio = (uint64_t)lo | ((uint64_t)hi << 32);
        {
            uint64_t sz = ((uint64_t)(hi & ~0xFULL) << 32) | (uint64_t)(lo & ~0xFULL);
            sz = (~sz) + 1ULL;
            *size_out = (sz > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)sz;
        }
    } else {
        mmio = (uint64_t)lo;
        *size_out = (~(uint32_t)(lo & ~0xFULL)) + 1u;
    }

    return mmio;
}

static void xhci_map_mmio_uncached(uint64_t phys, uint32_t size) {
    uint64_t a, end;
    if (size == 0)
        return;
    end = phys + (uint64_t)size;
    if (end < phys)
        end = (uint64_t)-1;
    for (a = phys & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE)
        vmm_map_page(a, a, VMM_PAGE_MMIO);
}

static uint32_t mmio_r32(const volatile uint32_t* base, uint32_t off) {
    return base[off / 4u];
}

static void mmio_w32(volatile uint32_t* base, uint32_t off, uint32_t v) {
    base[off / 4u] = v;
}

static void mmio_w64(volatile uint32_t* base, uint32_t off, uint64_t v) {
    base[off / 4u] = (uint32_t)v;
    base[off / 4u + 1u] = (uint32_t)(v >> 32);
}

static void xhci_legacy_handoff(volatile uint32_t* cap32, uint32_t hcc) {
    uint32_t eecp = (hcc >> 8) & 0xFFu;
    unsigned spin;

    if (eecp == 0)
        return;

    for (;;) {
        volatile uint32_t* ext = (volatile uint32_t*)((uint8_t*)cap32 + eecp * 4u);
        uint32_t id_next = ext[0];
        uint32_t id = id_next & 0xFFu;

        if (id == XHCI_EXT_CAPS_USB_LEGACY) {
            for (spin = 0; spin < 1000000u; spin++) {
                uint32_t legsup = ext[1];
                if ((legsup & LEGACY_BIOS_SEM) == 0)
                    break;
                __asm__ volatile("pause");
            }
            ext[1] |= LEGACY_OS_SEM;
            for (spin = 0; spin < 1000000u; spin++) {
                if ((ext[1] & LEGACY_BIOS_SEM) == 0)
                    break;
                __asm__ volatile("pause");
            }
            ext[2] &= ~(3u << 0);
            return;
        }
        {
            uint32_t next = (id_next >> 8) & 0xFFu;
            if (next == 0)
                return;
            eecp = next;
        }
    }
}

static int xhci_halt_reset(volatile uint32_t* op) {
    uint32_t cmd, sts;
    unsigned i;

    cmd = mmio_r32(op, 0x00);
    cmd &= ~USBCMD_RS;
    mmio_w32(op, 0x00, cmd);
    for (i = 0; i < 1000000u; i++) {
        sts = mmio_r32(op, 0x04);
        if (sts & USBSTS_HCH)
            break;
        __asm__ volatile("pause");
    }
    if (!(mmio_r32(op, 0x04) & USBSTS_HCH))
        return -1;

    cmd = mmio_r32(op, 0x00);
    cmd |= USBCMD_HCRST;
    mmio_w32(op, 0x00, cmd);
    for (i = 0; i < 1000000u; i++) {
        cmd = mmio_r32(op, 0x00);
        if ((cmd & USBCMD_HCRST) == 0)
            break;
        __asm__ volatile("pause");
    }
    for (i = 0; i < 1000000u; i++) {
        sts = mmio_r32(op, 0x04);
        if ((sts & USBSTS_CNR) == 0)
            break;
        __asm__ volatile("pause");
    }
    return 0;
}

static void* xhci_page_z(void) {
    uintptr_t p = pmm_alloc_page();
    uint64_t* q;
    unsigned i;
    if (!p)
        return 0;
    q = (uint64_t*)p;
    for (i = 0; i < 512; i++)
        q[i] = 0;
    return (void*)p;
}

static uint32_t portsc_off(unsigned port_idx0) {
    return 0x400u + (uint32_t)port_idx0 * 16u;
}

static uint32_t xhci_scratch_count(uint32_t hcs2) {
    return (((hcs2 >> 21) & 0x1Fu) << 5) | ((hcs2 >> 27) & 0x1Fu);
}

static void erdp_write(volatile uint32_t* rt0) {
    uint64_t ptr = XH.er_phys + (uint64_t)XH.er_deq * sizeof(XhciTrb);
    mmio_w64(rt0, 0x38, ptr | (1u << 3));
}

static void xhci_er_advance(void) {
    XhciTrb* te = &XH.er[XH.er_deq];
    (void)te;
    XH.er_deq++;
    if (XH.er_deq >= EV_TRBS) {
        XH.er_deq = 0;
        XH.er_cyc ^= 1;
    }
    erdp_write(XH.rt + 0x20 / 4u);
}

static int event_trb_valid(const XhciTrb* t) {
    uint32_t c = t->control;
    return ((c ^ (uint32_t)XH.er_cyc) & 1u) == 0u;
}

static int xhci_wait_cmd(const XhciTrb* cmd_trb, uint32_t* slot_out, unsigned timeout) {
    unsigned spins;
    for (spins = 0; spins < timeout; spins++) {
        XhciTrb* te = &XH.er[XH.er_deq];
        if (!event_trb_valid(te)) {
            __asm__ volatile("pause");
            continue;
        }
        uint32_t type = (te->control >> 10) & 0x3Fu;
        if (type == TRB_EVT_COMP) {
            uint32_t cc = (te->status >> 24) & 0xFFu;
            uint64_t ptr = te->parameter;
            uint32_t sid = (te->control >> 24) & 0xFFu;
            xhci_er_advance();
            if (ptr != (uint64_t)(uintptr_t)cmd_trb)
                continue;
            if (slot_out)
                *slot_out = sid;
            return (cc == CC_SUCCESS) ? 0 : -1;
        }
        if (type == TRB_EVT_TRANSFER) {
            xhci_er_advance();
            continue;
        }
        xhci_er_advance();
    }
    return -2;
}

static void cr_link_init(XhciTrb* cr) {
    unsigned i;
    for (i = 0; i < CMD_TRBS; i++) {
        cr[i].parameter = 0;
        cr[i].status = 0;
        cr[i].control = 0;
    }
    cr[CMD_TRBS - 1].parameter = XH.cr_phys;
    cr[CMD_TRBS - 1].status = 0;
    cr[CMD_TRBS - 1].control = (TRB_LINK << 10) | TRB_TC;
}

static XhciTrb* cr_enqueue_ptr(void) {
    return &XH.cr[XH.cr_enq];
}

static void cr_advance(void) {
    XH.cr_enq++;
    if (XH.cr_enq >= CMD_TRBS - 1u) {
        XH.cr_enq = 0;
        XH.cr_cyc ^= 1;
    }
}

static void ring_cmd_db(void) {
    mmio_w32(XH.db, 0, 0);
}

static int xhci_cmd_enable_slot(uint32_t* slot_id_out) {
    XhciTrb t;
    XhciTrb* my;
    t.parameter = 0;
    t.status = 0;
    t.control = (TRB_CMD_ENABLE_SLOT << 10);
    my = cr_enqueue_ptr();
    t.control |= TRB_CYC_BIT(XH.cr_cyc);
    *my = t;
    __asm__ volatile("" ::: "memory");
    cr_advance();
    ring_cmd_db();
    return xhci_wait_cmd(my, slot_id_out, 5000000u);
}

static int xhci_cmd_address_device(uint32_t slot_id, uint64_t inctx_phys) {
    XhciTrb t;
    XhciTrb* my;
    t.parameter = inctx_phys;
    t.status = 0;
    t.control = (TRB_CMD_ADDR_DEV << 10) | ((slot_id & 0xFFu) << 24);
    my = cr_enqueue_ptr();
    t.control |= TRB_CYC_BIT(XH.cr_cyc);
    *my = t;
    __asm__ volatile("" ::: "memory");
    cr_advance();
    ring_cmd_db();
    return xhci_wait_cmd(my, 0, 5000000u);
}

static int xhci_cmd_evaluate_context(uint32_t slot_id, uint64_t inctx_phys) {
    XhciTrb t;
    XhciTrb* my;
    t.parameter = inctx_phys;
    t.status = 0;
    t.control = (TRB_CMD_EVAL_CTX << 10) | ((slot_id & 0xFFu) << 24);
    my = cr_enqueue_ptr();
    t.control |= TRB_CYC_BIT(XH.cr_cyc);
    *my = t;
    __asm__ volatile("" ::: "memory");
    cr_advance();
    ring_cmd_db();
    return xhci_wait_cmd(my, 0, 5000000u);
}

static int xhci_cmd_configure_ep(uint32_t slot_id, uint64_t inctx_phys) {
    XhciTrb t;
    XhciTrb* my;
    t.parameter = inctx_phys;
    t.status = 0;
    t.control = (TRB_CMD_CONF_EP << 10) | ((slot_id & 0xFFu) << 24);
    my = cr_enqueue_ptr();
    t.control |= TRB_CYC_BIT(XH.cr_cyc);
    *my = t;
    __asm__ volatile("" ::: "memory");
    cr_advance();
    ring_cmd_db();
    return xhci_wait_cmd(my, 0, 5000000u);
}

static void inctx_clear(void) {
    uint64_t* p = (uint64_t*)XH.inctx_pg;
    unsigned i;
    for (i = 0; i < 512; i++)
        p[i] = 0;
}

static void icc_set_add(uint32_t drop, uint32_t add) {
    uint32_t* icc = (uint32_t*)XH.inctx_pg;
    icc[0] = drop;
    icc[1] = add;
}

static void* inctx_slot_va(void) {
    return XH.inctx_pg + 32;
}

static void* inctx_ep_va(unsigned ep_idx) {
    return XH.inctx_pg + 32 + (unsigned)XH.ctx_bytes * ep_idx;
}

static void fill_slot_ctx(void* vs, uint8_t speed, uint8_t root_port, uint32_t entries) {
    uint32_t* s = (uint32_t*)vs;
    s[0] = (entries << 27) | ((uint32_t)speed << 20);
    s[1] = (uint32_t)root_port << 16;
    s[2] = 0;
    s[3] = 0;
    if (XH.ctx_bytes >= 64u) {
        unsigned i;
        for (i = 4; i < XH.ctx_bytes / 4u; i++)
            s[i] = 0;
    }
}

static void fill_ep_ctx_ctrl(void* ve, uint64_t tr_phys, int dcs, uint16_t mps) {
    uint32_t* e = (uint32_t*)ve;
    uint32_t dq = (uint32_t)(tr_phys | (uint64_t)(dcs & 1));
    uint32_t dq_hi = (uint32_t)(tr_phys >> 32);
    e[0] = 0;
    e[1] = ((uint32_t)mps << 16) | (4u << 3) | (3u << 1);
    e[2] = dq;
    e[3] = dq_hi;
    e[4] = 8;
    if (XH.ctx_bytes >= 64u) {
        unsigned i;
        for (i = 5; i < XH.ctx_bytes / 4u; i++)
            e[i] = 0;
    }
}

static void fill_ep_ctx_intr_in(void* ve, uint64_t tr_phys, int dcs, uint16_t mps, uint8_t interval) {
    uint32_t* e = (uint32_t*)ve;
    uint32_t dq = (uint32_t)(tr_phys | (uint64_t)(dcs & 1));
    uint32_t dq_hi = (uint32_t)(tr_phys >> 32);
    e[0] = (uint32_t)interval << 16;
    e[1] = ((uint32_t)mps << 16) | (10u << 3) | (3u << 1);
    e[2] = dq;
    e[3] = dq_hi;
    e[4] = (uint32_t)mps;
    if (XH.ctx_bytes >= 64u) {
        unsigned i;
        for (i = 5; i < XH.ctx_bytes / 4u; i++)
            e[i] = 0;
    }
}

static void ep_ring_link_init(uint8_t* ring_pg, uint64_t ring_phys) {
    XhciTrb* r = (XhciTrb*)ring_pg;
    unsigned i;
    for (i = 0; i < EP0_TRBS; i++) {
        r[i].parameter = 0;
        r[i].status = 0;
        r[i].control = 0;
    }
    r[EP0_TRBS - 1].parameter = ring_phys;
    r[EP0_TRBS - 1].status = 0;
    r[EP0_TRBS - 1].control = (TRB_LINK << 10) | TRB_TC;
}

static void ep0_enqueue_setup(const uint8_t setup[8], uint32_t trt, int chain) {
    XhciTrb* t = &((XhciTrb*)XH.ep0_ring_pg)[XH.ep0_enq];
    t->parameter = *(const uint64_t*)setup;
    t->status = 8u;
    t->control = (TRB_SETUP << 10) | TRB_CTRL_IDT | (trt << 16) | TRB_CYC_BIT(XH.ep0_cyc);
    if (chain)
        t->control |= TRB_CTRL_CH;
    if (++XH.ep0_enq >= EP0_TRBS - 1u) {
        XH.ep0_enq = 0;
        XH.ep0_cyc ^= 1;
    }
}

static void ep0_enqueue_data(uint64_t buf_phys, uint32_t len, int is_in, int chain) {
    XhciTrb* t = &((XhciTrb*)XH.ep0_ring_pg)[XH.ep0_enq];
    t->parameter = buf_phys;
    t->status = len;
    t->control = (TRB_DATA << 10) | TRB_CYC_BIT(XH.ep0_cyc);
    if (is_in)
        t->control |= TRB_CTRL_DIR_IN;
    if (chain)
        t->control |= TRB_CTRL_CH;
    if (++XH.ep0_enq >= EP0_TRBS - 1u) {
        XH.ep0_enq = 0;
        XH.ep0_cyc ^= 1;
    }
}

static void ep0_enqueue_status(int dir_in, int ioc) {
    XhciTrb* t = &((XhciTrb*)XH.ep0_ring_pg)[XH.ep0_enq];
    t->parameter = 0;
    t->status = 0;
    t->control = (TRB_STATUS << 10) | TRB_CYC_BIT(XH.ep0_cyc);
    if (dir_in)
        t->control |= TRB_CTRL_DIR_IN;
    if (ioc)
        t->control |= TRB_CTRL_IOC;
    if (++XH.ep0_enq >= EP0_TRBS - 1u) {
        XH.ep0_enq = 0;
        XH.ep0_cyc ^= 1;
    }
}

static void ring_ep_db(uint32_t slot, unsigned ep_id) {
    mmio_w32(XH.db, slot * 4u, (uint32_t)ep_id);
}

static void ep0_sync_dequeue_from_hw(void) {
    const volatile uint32_t* oep = (const volatile uint32_t*)(XH.dev_out_pg + (size_t)XH.ctx_bytes);
    uint64_t dq = (uint64_t)oep[2] | ((uint64_t)oep[3] << 32);
    uint64_t addr = dq & ~0xFULL;
    uint64_t lo = XH.ep0_ring_phys & ~0xFULL;
    uint64_t last = lo + (uint64_t)(EP0_TRBS - 2u) * 16ull;
    if (addr < lo || addr > last)
        return;
    XH.ep0_enq = (uint32_t)((addr - lo) / 16ull);
    XH.ep0_cyc = (int)(dq & 1u);
}

static int xhci_wait_transfer_done(void) {
    unsigned timeout;
    for (timeout = 0; timeout < 8000000u; timeout++) {
        XhciTrb* te = &XH.er[XH.er_deq];
        if (!event_trb_valid(te)) {
            __asm__ volatile("pause");
            continue;
        }
        uint32_t typ = (te->control >> 10) & 0x3Fu;
        if (typ == TRB_EVT_TRANSFER) {
            uint32_t cc = (te->status >> 24) & 0xFFu;
            uint32_t ep = (te->control >> 16) & 0x1Fu;
            xhci_er_advance();
            if (ep != 1u)
                continue;
            if (cc == CC_SUCCESS) {
                ep0_sync_dequeue_from_hw();
                return 0;
            }
            return -1;
        }
        if (typ == TRB_EVT_COMP) {
            xhci_er_advance();
            continue;
        }
        xhci_er_advance();
    }
    return -2;
}

static int xhci_control_xfer(const uint8_t setup[8], const void* out_data, uint32_t len, int is_in) {
    uint32_t trt;

    ep0_sync_dequeue_from_hw();

    if (is_in && len > 0)
        trt = SETUP_TRT_IN;
    else if (!is_in && len > 0)
        trt = SETUP_TRT_OUT;
    else
        trt = SETUP_TRT_NO;

    if (len > 0 && !is_in && out_data) {
        uint32_t i;
        for (i = 0; i < len && i < (uint32_t)PAGE_SIZE; i++)
            XH.ctrl_buf_pg[i] = ((const uint8_t*)out_data)[i];
    }

    if (trt == SETUP_TRT_NO) {
        ep0_enqueue_setup(setup, trt, 0);
        ep0_enqueue_status(1, 1);
    } else if (trt == SETUP_TRT_IN) {
        ep0_enqueue_setup(setup, trt, 1);
        ep0_enqueue_data(XH.ctrl_buf_phys, len, 1, 1);
        ep0_enqueue_status(0, 1);
    } else {
        ep0_enqueue_setup(setup, trt, 1);
        ep0_enqueue_data(XH.ctrl_buf_phys, len, 0, 1);
        ep0_enqueue_status(1, 1);
    }

    ring_ep_db(XH.slot_id, 1);
    return xhci_wait_transfer_done();
}

static int xhci_control_xfer_simple(const uint8_t setup[8], int is_in, uint32_t len) {
    return xhci_control_xfer(setup, 0, len, is_in);
}

static int get_descriptor_device(void) {
    uint8_t setup[8] = {0x80u, 6u, 0u, 1u, 0u, 0u, 18u, 0u};
    return xhci_control_xfer_simple(setup, 1, 18u);
}

static int get_descriptor_cfg(uint8_t idx, uint16_t total_len) {
    uint8_t setup[8] = {0x80u, 6u, idx, 2u, 0u, 0u, (uint8_t)total_len, (uint8_t)(total_len >> 8)};
    return xhci_control_xfer_simple(setup, 1, total_len);
}

static int set_configuration(uint8_t cfg) {
    uint8_t setup[8] = {0u, 9u, cfg, 0u, 0u, 0u, 0u, 0u};
    return xhci_control_xfer_simple(setup, 0, 0u);
}

static int set_protocol_boot(uint8_t iface) {
    uint8_t setup[8] = {0x21u, 0x0Bu, 0u, 0u, iface, 0u, 0u, 0u};
    return xhci_control_xfer_simple(setup, 0, 0u);
}

static int xhci_port_reset(unsigned port0) {
    volatile uint32_t* pr = XH.op + portsc_off(port0) / 4u;
    uint32_t v;
    unsigned i;

    v = mmio_r32(pr, 0);
    if (!(v & PORTSC_CCS))
        return -1;
    v |= PORTSC_PR;
    mmio_w32(pr, 0, v);
    for (i = 0; i < 2000000u; i++) {
        v = mmio_r32(pr, 0);
        if (v & PORTSC_PRC)
            break;
        __asm__ volatile("pause");
    }
    mmio_w32(pr, 0, v | PORTSC_PRC);
    for (i = 0; i < 2000000u; i++) {
        v = mmio_r32(pr, 0);
        if (v & PORTSC_PED)
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static int find_boot_mouse_in_cfg(const uint8_t* buf, uint32_t total, uint8_t* cfg_val_out,
                                  uint8_t* iface_out, uint8_t* ep_id_out, uint16_t* mps_out,
                                  uint8_t* interval_out) {
    uint32_t pos;
    if (total < 9u || buf[1] != 2u)
        return -1;
    *cfg_val_out = buf[5];
    pos = 0u;
    while (pos + 2u <= total) {
        uint8_t len = buf[pos];
        uint8_t type = buf[pos + 1];
        if (len < 2u || pos + (uint32_t)len > total)
            break;
        if (type == 4u && len >= 9u) {
            uint8_t cls = buf[pos + 5];
            uint8_t sub = buf[pos + 6];
            uint8_t prot = buf[pos + 7];
            if (cls == 3u && sub == 1u && prot == 2u) {
                uint8_t iface = buf[pos + 2];
                uint32_t q = pos + (uint32_t)len;
                while (q + 2u <= total) {
                    uint8_t el = buf[q];
                    uint8_t et = buf[q + 1];
                    if (el < 7u || q + (uint32_t)el > total)
                        break;
                    if (et == 5u) {
                        uint8_t epaddr = buf[q + 2];
                        uint16_t mps = (uint16_t)buf[q + 4] | ((uint16_t)buf[q + 5] << 8);
                        if ((epaddr & 0x80u) != 0) {
                            uint8_t epnum = epaddr & 0xFu;
                            if (epnum != 0u) {
                                *iface_out = iface;
                                *ep_id_out = (uint8_t)(2u * epnum + 1u);
                                *mps_out = (uint16_t)(mps & 0x7FFu);
                                *interval_out = buf[q + 6];
                                return 0;
                            }
                        }
                    }
                    q += (uint32_t)el;
                }
            }
        }
        pos += (uint32_t)len;
    }
    return -1;
}

static void xhci_run(void) {
    uint32_t v = mmio_r32(XH.op, 0);
    mmio_w32(XH.op, 0, v | USBCMD_RS);
    for (unsigned i = 0; i < 2000000u; i++) {
        if ((mmio_r32(XH.op, 0x04) & USBSTS_HCH) == 0)
            return;
        __asm__ volatile("pause");
    }
}

static int xhci_enumerate_hid_mouse(void) {
    unsigned p;
    uint8_t spd;
    uint32_t slot_id = 0;
    uint32_t k;
    uint16_t cfg_len = 9u;

    for (p = 0; p < XH.n_ports; p++) {
        volatile uint32_t* pr = XH.op + portsc_off(p) / 4u;
        uint32_t pv = mmio_r32(pr, 0);
        if (pv & PORTSC_CCS)
            break;
    }
    if (p >= XH.n_ports)
        return -1;

    if (xhci_port_reset(p) != 0)
        return -2;

    {
        volatile uint32_t* pr = XH.op + portsc_off(p) / 4u;
        uint32_t pv = mmio_r32(pr, 0);
        spd = (uint8_t)((pv >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK);
    }

    XH.root_port = (uint8_t)(p + 1u);
    XH.port_speed = spd;

    if (xhci_cmd_enable_slot(&slot_id) != 0)
        return -3;
    XH.slot_id = slot_id;

    XH.dcbaa[slot_id] = XH.dev_out_phys;

    inctx_clear();
    icc_set_add(0, 0x3u);
    fill_slot_ctx(inctx_slot_va(), spd, XH.root_port, 1u);
    XH.ep0_mps = (spd == 4u || spd == 5u) ? 512u : 64u;
    fill_ep_ctx_ctrl(inctx_ep_va(1), XH.ep0_ring_phys, XH.ep0_cyc & 1, XH.ep0_mps);

    if (xhci_cmd_address_device(slot_id, XH.inctx_phys) != 0)
        return -4;

    if (get_descriptor_device() != 0)
        return -5;

    {
        uint8_t mps0 = XH.ctrl_buf_pg[7];
        uint16_t mps;
        if (spd >= 4u)
            mps = (uint16_t)(1u << (mps0 & 0xFu));
        else {
            mps = mps0;
            if (mps < 8u)
                mps = 8u;
        }
        if (mps > 1024u)
            mps = 1024u;
        if (mps != XH.ep0_mps) {
            XH.ep0_mps = mps;
            inctx_clear();
            icc_set_add(0, 0x2u);
            fill_ep_ctx_ctrl(inctx_ep_va(1), XH.ep0_ring_phys, XH.ep0_cyc & 1, XH.ep0_mps);
            if (xhci_cmd_evaluate_context(slot_id, XH.inctx_phys) != 0)
                return -6;
        }
    }

    if (get_descriptor_cfg(0, 9u) != 0)
        return -7;

    {
        uint16_t need = (uint16_t)XH.ctrl_buf_pg[2] | ((uint16_t)XH.ctrl_buf_pg[3] << 8);
        if (need > (uint16_t)PAGE_SIZE)
            need = (uint16_t)PAGE_SIZE;
        if (need < 9u)
            need = 9u;
        cfg_len = need;
        if (get_descriptor_cfg(0, need) != 0)
            return -8;
    }

    if (find_boot_mouse_in_cfg(XH.ctrl_buf_pg, cfg_len, &XH.conf_val, &XH.hid_iface, &XH.ep_intr_id,
                                &XH.ep_intr_mps, &XH.ep_interval) != 0)
        return -9;

    if (set_configuration(XH.conf_val) != 0)
        return -10;

    (void)set_protocol_boot(XH.hid_iface);

    {
        unsigned eid = (unsigned)XH.ep_intr_id;
        if (eid >= 31u)
            return -11;
        for (k = 0; k < (uint32_t)eid * XH.ctx_bytes; k++)
            XH.inctx_pg[32 + k] = XH.dev_out_pg[k];
        icc_set_add(0, (1u << 0) | (1u << eid));
        {
            uint32_t* sc = (uint32_t*)inctx_slot_va();
            sc[0] = (sc[0] & ~(0x1Fu << 27)) | ((uint32_t)eid << 27);
        }
        fill_ep_ctx_intr_in(inctx_ep_va(eid), XH.ep_in_ring_phys, XH.epi_cyc & 1, XH.ep_intr_mps,
                            XH.ep_interval ? XH.ep_interval : 10u);
        if (xhci_cmd_configure_ep(slot_id, XH.inctx_phys) != 0)
            return -12;
    }

    {
        XhciTrb* t = &((XhciTrb*)XH.ep_in_ring_pg)[XH.epi_enq];
        uint32_t rl = (uint32_t)XH.ep_intr_mps;
        if (rl < 8u)
            rl = 8u;
        if (rl > 256u)
            rl = 256u;
        t->parameter = XH.ctrl_buf_phys;
        t->status = rl;
        t->control = (TRB_NORMAL << 10) | TRB_CTRL_IOC | TRB_CYC_BIT(XH.epi_cyc);
        if (++XH.epi_enq >= EPI_TRBS - 1u) {
            XH.epi_enq = 0;
            XH.epi_cyc ^= 1;
        }
        ring_ep_db(slot_id, XH.ep_intr_id);
    }

    return 0;
}

static void xhci_repost_intr_trb(void) {
    XhciTrb* t = &((XhciTrb*)XH.ep_in_ring_pg)[XH.epi_enq];
    uint32_t rl = (uint32_t)XH.ep_intr_mps;
    if (rl < 8u)
        rl = 8u;
    if (rl > 256u)
        rl = 256u;
    t->parameter = XH.ctrl_buf_phys;
    t->status = rl;
    t->control = (TRB_NORMAL << 10) | TRB_CTRL_IOC | TRB_CYC_BIT(XH.epi_cyc);
    if (++XH.epi_enq >= EPI_TRBS - 1u) {
        XH.epi_enq = 0;
        XH.epi_cyc ^= 1;
    }
    ring_ep_db(XH.slot_id, XH.ep_intr_id);
}

void xhci_poll(void) {
    if (!XH.ok || !xhci_usb_mouse_active)
        return;

    for (;;) {
        XhciTrb* te = &XH.er[XH.er_deq];
        if (!event_trb_valid(te))
            return;
        uint32_t typ = (te->control >> 10) & 0x3Fu;
        if (typ != TRB_EVT_TRANSFER) {
            xhci_er_advance();
            continue;
        }
        {
            uint32_t cc = (te->status >> 24) & 0xFFu;
            uint32_t ep = (te->control >> 16) & 0x1Fu;
            xhci_er_advance();
            if (cc != CC_SUCCESS || ep != (uint32_t)XH.ep_intr_id) {
                xhci_repost_intr_trb();
                continue;
            }
        }

        {
            const uint8_t* r = XH.ctrl_buf_pg;
            uint8_t bt = r[0];
            int dx = (int)(int8_t)r[1];
            int dy = (int)(int8_t)r[2];
            {
                os_event_t o;
                unsigned char b0 = (unsigned char)((bt & 7u) | 0x08u);

                o.type     = MOUSE_EVENT;
                o.mouse_x  = dx;
                o.mouse_y  = dy;
                o.key_code = (unsigned)b0;
                event_queue_push(&o);
            }
        }
        xhci_repost_intr_trb();
        return;
    }
}

void xhci_set_screen_dims(int w, int h) {
    XH.scr_w = w > 0 ? w : 1;
    XH.scr_h = h > 0 ? h : 1;
}

int xhci_init(uint32_t mb2_phys, XhciInfo* info_out) {
    PciFunction f;
    uint32_t bar_sz = 0;
    uint64_t bar;
    uint32_t max_slots, i;
    void *cr_v, *er_v, *erst_v, *dcbaa_v, *spa_v = 0;
    uint64_t cr_p, er_p, erst_p, dcbaa_p;
    uint32_t hcc;
    volatile uint32_t *cap32, *op32;

    xhci_usb_mouse_active = 0;
    XH.ok = 0;

    xhci_parse_mcfg(mb2_phys);

    if (info_out)
        info_out->present = 0;

    f = pci_find_class(PCI_CLASS_SERIAL, PCI_SUBCLASS_USB, PCI_PROGIF_XHCI);
    if (!f.valid)
        return -1;
    if (g_mcfg.valid)
        (void)xhci_ecam_read32(f.bus, f.slot, f.func, 0);

    {
        uint16_t cmd = pci_read16(f.bus, f.slot, f.func, 0x04);
        pci_write16(f.bus, f.slot, f.func, 0x04, (uint16_t)(cmd | 6u));
    }

    bar = xhci_probe_bar_mem(&f, &bar_sz);
    if (bar == 0 || bar_sz < 0x1000u)
        return -2;

    xhci_map_mmio_uncached(bar, bar_sz);

    cap32 = (volatile uint32_t*)(uintptr_t)bar;
    {
        uint8_t caplen = *(volatile uint8_t*)(uintptr_t)bar;
        if (info_out) {
            info_out->present = 1;
            info_out->bus = f.bus;
            info_out->slot = f.slot;
            info_out->func = f.func;
            info_out->bar_phys = bar;
            info_out->bar_size = bar_sz;
            info_out->cap_length = caplen;
            info_out->hci_version = *(volatile uint16_t*)((volatile uint8_t*)cap32 + 2);
            info_out->hcs_params1 = mmio_r32(cap32, 0x04);
            info_out->hcs_params2 = mmio_r32(cap32, 0x08);
            info_out->hcs_params3 = mmio_r32(cap32, 0x0C);
            info_out->hcc_params = mmio_r32(cap32, 0x10);
            info_out->db_off = mmio_r32(cap32, 0x14);
            info_out->rt_off = mmio_r32(cap32, 0x18);
        }
    }

    hcc = mmio_r32(cap32, 0x10);
    xhci_legacy_handoff(cap32, hcc);

    op32 = (volatile uint32_t*)((uint8_t*)cap32 + *(volatile uint8_t*)(uintptr_t)bar);

    if (xhci_halt_reset(op32) != 0)
        return -3;

    XH.cap = cap32;
    XH.op = op32;
    XH.caplen = *(volatile uint8_t*)(uintptr_t)bar;
    XH.hcs1 = mmio_r32(cap32, 0x04);
    XH.hcs2 = mmio_r32(cap32, 0x08);
    XH.hcc = hcc;
    XH.n_ports = (XH.hcs1 >> 24) & 0xFFu;
    max_slots = XH.hcs1 & 0xFFu;
    XH.ctx_bytes = (hcc & (1u << 2)) ? 32u : 64u;
    XH.scratch = xhci_scratch_count(XH.hcs2);

    XH.db = (volatile uint32_t*)((uint8_t*)cap32 + (mmio_r32(cap32, 0x14) & ~3u));
    {
        uint32_t rts = mmio_r32(cap32, 0x18);
        XH.rt = (volatile uint32_t*)((uint8_t*)cap32 + (rts & ~0x1Fu));
    }

    dcbaa_v = xhci_page_z();
    if (!dcbaa_v)
        return -4;
    dcbaa_p = (uint64_t)(uintptr_t)dcbaa_v;
    XH.dcbaa = (volatile uint64_t*)dcbaa_v;
    XH.dcbaa_phys = dcbaa_p;

    if (XH.scratch > 0u) {
        uint64_t* spa;
        spa_v = xhci_page_z();
        if (!spa_v)
            return -4;
        spa = (uint64_t*)spa_v;
        for (i = 0; i < XH.scratch; i++) {
            void* pg = xhci_page_z();
            if (!pg)
                return -4;
            spa[i] = (uint64_t)(uintptr_t)pg;
        }
        XH.dcbaa[0] = (uint64_t)(uintptr_t)spa;
    }

    cr_v = xhci_page_z();
    er_v = xhci_page_z();
    erst_v = xhci_page_z();
    if (!cr_v || !er_v || !erst_v)
        return -4;

    XH.cr = (XhciTrb*)cr_v;
    XH.cr_phys = (uint64_t)(uintptr_t)cr_v;
    cr_p = XH.cr_phys;
    XH.cr_enq = 0;
    XH.cr_cyc = 1;
    cr_link_init(XH.cr);

    er_p = (uint64_t)(uintptr_t)er_v;
    XH.er = (XhciTrb*)er_v;
    XH.er_phys = er_p;
    XH.er_deq = 0;
    XH.er_cyc = 1;
    for (i = 0; i < EV_TRBS; i++) {
        XH.er[i].parameter = 0;
        XH.er[i].status = 0;
        XH.er[i].control = 0;
    }

    erst_p = (uint64_t)(uintptr_t)erst_v;
    {
        volatile uint8_t* ep = (volatile uint8_t*)erst_v;
        *(volatile uint64_t*)ep = er_p;
        *(volatile uint16_t*)(ep + 8) = (uint16_t)EV_TRBS;
    }

    XH.dev_out_pg = xhci_page_z();
    XH.inctx_pg = xhci_page_z();
    XH.ep0_ring_pg = xhci_page_z();
    XH.ep_in_ring_pg = xhci_page_z();
    XH.ctrl_buf_pg = xhci_page_z();
    if (!XH.dev_out_pg || !XH.inctx_pg || !XH.ep0_ring_pg || !XH.ep_in_ring_pg || !XH.ctrl_buf_pg)
        return -4;
    XH.dev_out_phys = (uint64_t)(uintptr_t)XH.dev_out_pg;
    XH.inctx_phys = (uint64_t)(uintptr_t)XH.inctx_pg;
    XH.ep0_ring_phys = (uint64_t)(uintptr_t)XH.ep0_ring_pg;
    XH.ep_in_ring_phys = (uint64_t)(uintptr_t)XH.ep_in_ring_pg;
    XH.ctrl_buf_phys = (uint64_t)(uintptr_t)XH.ctrl_buf_pg;

    XH.ep0_enq = 0;
    XH.ep0_cyc = 1;
    XH.epi_enq = 0;
    XH.epi_cyc = 1;

    ep_ring_link_init(XH.ep0_ring_pg, XH.ep0_ring_phys);
    ep_ring_link_init(XH.ep_in_ring_pg, XH.ep_in_ring_phys);

    mmio_w32(op32, 0x38, max_slots & 0xFFu);
    mmio_w64(op32, 0x30, dcbaa_p);
    mmio_w64(op32, 0x18, cr_p & ~63ULL);

    {
        volatile uint32_t* rt0 = XH.rt + 0x20 / 4u;
        mmio_w32(rt0, 0x00, mmio_r32(rt0, 0x00) | (1u << 1));
        mmio_w32(rt0, 0x08, 1u);
        mmio_w64(rt0, 0x10, erst_p);
        mmio_w64(rt0, 0x18, er_p & ~15ULL);
    }

    xhci_run();

    if (xhci_enumerate_hid_mouse() == 0) {
        XH.ok = 1;
        xhci_usb_mouse_active = 1;
    }

    return 0;
}
