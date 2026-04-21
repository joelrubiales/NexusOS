/*
 * Intel HDA (Azalia) — inicialización PCI/MMIO, CORB/RIRB, sondeo de códec
 * y reproducción PCM mínima (BDL + stream de salida).
 *
 * Referencias: Intel HD Audio 1.0 spec, OSDev Intel HDA wiki.
 */
#include "hda.h"
#include "pci.h"
#include "memory.h"
#include <stddef.h>
#include <stdint.h>

#define PCI_CMD_REG     0x04u
#define PCI_CMD_BME     0x04u
#define PCI_CMD_MSE     0x02u
#define PCI_BAR0        0x10u

/* MMIO registers (BAR0) */
#define HDA_REG_GCAP         0x00u
#define HDA_REG_VMIN         0x02u
#define HDA_REG_VMAJ         0x03u
#define HDA_REG_OUTPAY       0x04u
#define HDA_REG_INPAY        0x06u
#define HDA_REG_GCTL         0x08u
#define HDA_REG_WAKEEN       0x0cu
#define HDA_REG_STATESTS     0x0eu
#define HDA_REG_GSTS         0x10u
#define HDA_REG_INTCTL       0x20u
#define HDA_REG_INTSTS       0x24u
#define HDA_REG_WALCLK       0x30u
#define HDA_REG_SSYNC        0x38u
#define HDA_REG_CORBLBASE    0x40u
#define HDA_REG_CORBUBASE    0x44u
#define HDA_REG_CORBWP       0x48u
#define HDA_REG_CORBRP       0x4au
#define HDA_REG_CORBCTL      0x4cu
#define HDA_REG_CORBSTS      0x4du
#define HDA_REG_CORBSIZE     0x4eu
#define HDA_REG_RIRBLBASE    0x50u
#define HDA_REG_RIRBUBASE    0x54u
#define HDA_REG_RIRBWP       0x58u
#define HDA_REG_RINTCNT      0x5au
#define HDA_REG_RIRBCTL      0x5cu
#define HDA_REG_RIRBSTS      0x5du
#define HDA_REG_RIRBSIZE     0x5eu
#define HDA_REG_IC           0x60u
#define HDA_REG_IR           0x64u
#define HDA_REG_IRS          0x68u
#define HDA_REG_DPLBASE      0x70u
#define HDA_REG_DPUBASE      0x74u

#define HDA_GCTL_CRST        (1u << 0)
#define HDA_CORBCTL_RUN      (1u << 0)
#define HDA_CORBCTL_CORBRST  (1u << 1)
#define HDA_RIRBCTL_RUN      (1u << 0)

#define HDA_CORBSIZE_256     0x02u
#define HDA_RIRBSIZE_256     0x02u

#define HDA_STREAM_BASE      0x80u
#define HDA_STREAM_STRIDE    0x20u

#define HDA_SD_CTL_SRST      (1u << 0)
#define HDA_SD_CTL_RUN       (1u << 1)

#define HDA_SD_OFFSET_CTL    0x00u
#define HDA_SD_OFFSET_STS    0x03u
#define HDA_SD_OFFSET_LPIB   0x04u
#define HDA_SD_OFFSET_CBL    0x08u
#define HDA_SD_OFFSET_LVI    0x0cu
#define HDA_SD_OFFSET_FMT    0x12u
#define HDA_SD_OFFSET_BDPL   0x18u
#define HDA_SD_OFFSET_BDPU   0x1cu

/* Verbs */
#define HDA_VERB_GET_PARAM   0xf00u
#define AC_PAR_VENDOR_ID     0x00u
#define AC_PAR_SUBNODE_COUNT 0x04u
#define AC_PAR_NODE_COUNT    0xffu
#define AC_PAR_WIDGET_CAP    0x0fu

#define AC_WCAP_TYPE         (0x0fu << 20)
#define AC_WCAP_TYPE_SHIFT   20
#define AC_WID_AUD_OUT       0x02u
#define AC_WID_PIN_COMPLEX   0x04u

#define HDA_CORB_ENTRIES     256u
#define HDA_RIRB_ENTRIES     256u
#define HDA_CORB_SZ          (HDA_CORB_ENTRIES * 4u)
#define HDA_RIRB_SZ          (HDA_RIRB_ENTRIES * 8u)

#define HDA_PLAY_RATE        44100u
#define HDA_MMIO_VIRT_BASE   0x00000000F0C00000ULL

typedef struct __attribute__((packed)) {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t length;
    uint32_t reserved;
} HdaBdlEntry;

static struct {
    int                 ok;
    PciFunction         pci;
    volatile uint8_t*   mmio;
    uint64_t            bar_phys;
    size_t              bar_size;
    uint32_t*           corb; /* kernel virt, identity phys */
    volatile uint64_t*  rirb;
    uintptr_t           corb_phys;
    uintptr_t           rirb_phys;
    uint16_t            corb_wp;
    uint16_t            rirb_rp;
    uint8_t             codec_mask;
    uint8_t             codec_addr;
    uint8_t             dac_nid;
    uint8_t             pin_nid;
    uint16_t            gcap;
    uint8_t             iss;
    uint8_t             oss;
    unsigned            sd_out_base;
    uint32_t*           bdl;
    uintptr_t           bdl_phys;
} g_hda;

static PciFunction g_hda_find_result;

static int hda_pci_visit(const PciFunction* f, void* user) {
    (void)user;
    if (f->class_code == PCI_CLASS_MULTIMEDIA && f->subclass == PCI_SUBCLASS_HDA) {
        g_hda_find_result = *f;
        return 1;
    }
    return 0;
}

static uint32_t mmio_r32(volatile uint8_t* b, unsigned off) {
    return *(volatile uint32_t*)(b + off);
}

static void mmio_w32(volatile uint8_t* b, unsigned off, uint32_t v) {
    *(volatile uint32_t*)(b + off) = v;
}

static uint16_t mmio_r16(volatile uint8_t* b, unsigned off) {
    return *(volatile uint16_t*)(b + off);
}

static void mmio_w16(volatile uint8_t* b, unsigned off, uint16_t v) {
    *(volatile uint16_t*)(b + off) = v;
}

static uint8_t mmio_r8(volatile uint8_t* b, unsigned off) {
    return *(volatile uint8_t*)(b + off);
}

static void mmio_w8(volatile uint8_t* b, unsigned off, uint8_t v) {
    *(volatile uint8_t*)(b + off) = v;
}

static void delay_spin(unsigned n) {
    volatile unsigned i;
    for (i = 0; i < n; i++)
        __asm__ volatile("pause" ::: "memory");
}

static uint64_t pci_bar_size_raw(uint8_t bus, uint8_t slot, uint8_t fn, unsigned bar_off) {
    uint32_t lo, mask;
    lo = pci_read32(bus, slot, fn, bar_off);
    pci_write32(bus, slot, fn, bar_off, 0xFFFFFFFFu);
    mask = pci_read32(bus, slot, fn, bar_off);
    pci_write32(bus, slot, fn, bar_off, lo);
    if ((mask & 0x6u) == 0x4u)
        return 0; /* 64-bit: simplificado; reprobar con BAR alto */
    if ((mask & 1u) != 0)
        return 0;
    mask &= ~0xFu;
    if (mask == 0)
        return 0;
    return (uint64_t)(~mask + 1u);
}

static int pci_read_bar0_mem(uint8_t bus, uint8_t slot, uint8_t fn, uint64_t* phys, uint64_t* size) {
    uint32_t lo = pci_read32(bus, slot, fn, PCI_BAR0);
    if (lo & 1u)
        return -1;
    if ((lo & 0x6u) == 0x4u) {
        uint32_t hi = pci_read32(bus, slot, fn, PCI_BAR0 + 4u);
        *phys = ((uint64_t)(hi) << 32) | ((uint64_t)lo & ~0xFULL);
        pci_write32(bus, slot, fn, PCI_BAR0, 0xFFFFFFFFu);
        pci_write32(bus, slot, fn, PCI_BAR0 + 4u, 0xFFFFFFFFu);
        {
            uint32_t ml = pci_read32(bus, slot, fn, PCI_BAR0);
            uint32_t mh = pci_read32(bus, slot, fn, PCI_BAR0 + 4u);
            pci_write32(bus, slot, fn, PCI_BAR0, lo);
            pci_write32(bus, slot, fn, PCI_BAR0 + 4u, hi);
            {
                uint64_t p64 = (((uint64_t)mh << 32) | ml) & ~0xFULL;
                *size = (~p64 + 1ull) & 0xFFFFFFFFFFFFFULL;
            }
        }
        return 0;
    }
    *phys = (uint64_t)(lo & ~0xFu);
    *size = pci_bar_size_raw(bus, slot, fn, PCI_BAR0);
    if (*size == 0)
        return -1;
    return 0;
}

static void hda_map_mmio(uint64_t phys, size_t len) {
    uint64_t pa = phys & ~(PAGE_SIZE - 1ULL);
    uint64_t end = phys + len;
    uint64_t i = 0;
    if (end < phys)
        end = (uint64_t)-1;
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    while (pa < end) {
        vmm_map_page(HDA_MMIO_VIRT_BASE + (uint64_t)i * PAGE_SIZE, pa, VMM_PAGE_MMIO);
        pa += PAGE_SIZE;
        i++;
    }
}

static volatile uint8_t* hda_mmio_va(uint64_t bar_phys) {
    uintptr_t off = (uintptr_t)(bar_phys & (PAGE_SIZE - 1ULL));
    return (volatile uint8_t*)(uintptr_t)(HDA_MMIO_VIRT_BASE + off);
}

static void hda_controller_reset(volatile uint8_t* m) {
    uint32_t g;
    mmio_w32(m, HDA_REG_GCTL, 0);
    delay_spin(10000);
    g = mmio_r32(m, HDA_REG_GCTL);
    mmio_w32(m, HDA_REG_GCTL, g | HDA_GCTL_CRST);
    delay_spin(10000);
    {
        int t = 0;
        while (t++ < 500000) {
            if (mmio_r32(m, HDA_REG_GCTL) & HDA_GCTL_CRST)
                return;
            delay_spin(50);
        }
    }
}

static void hda_corb_rirb_setup(volatile uint8_t* m) {
    uint8_t* blob;
    uintptr_t p;

    mmio_w8(m, HDA_REG_CORBCTL, 0);
    mmio_w8(m, HDA_REG_RIRBCTL, 0);
    delay_spin(5000);

    mmio_w16(m, HDA_REG_CORBRP, 0x8000u);
    delay_spin(1000);
    mmio_w16(m, HDA_REG_CORBRP, 0);

    mmio_w16(m, HDA_REG_CORBWP, 0);
    mmio_w8(m, HDA_REG_CORBSIZE, HDA_CORBSIZE_256);
    mmio_w8(m, HDA_REG_RIRBSIZE, HDA_RIRBSIZE_256);

    blob = (uint8_t*)kmalloc(HDA_CORB_SZ + HDA_RIRB_SZ + 128u);
    if (!blob)
        return;
    p = (uintptr_t)blob;
    p = (p + 127u) & ~127u;
    g_hda.corb = (uint32_t*)p;
    g_hda.corb_phys = p;
    g_hda.rirb = (volatile uint64_t*)(p + HDA_CORB_SZ);
    g_hda.rirb_phys = p + HDA_CORB_SZ;

    mmio_w32(m, HDA_REG_CORBLBASE, (uint32_t)g_hda.corb_phys);
    mmio_w32(m, HDA_REG_CORBUBASE, 0);
    mmio_w32(m, HDA_REG_RIRBLBASE, (uint32_t)g_hda.rirb_phys);
    mmio_w32(m, HDA_REG_RIRBUBASE, 0);

    g_hda.corb_wp = 0;
    g_hda.rirb_rp = 0;

    mmio_w16(m, HDA_REG_RIRBWP, 0);
    mmio_w16(m, HDA_REG_RINTCNT, 1);

    mmio_w8(m, HDA_REG_RIRBCTL, HDA_RIRBCTL_RUN);
    delay_spin(1000);
    mmio_w8(m, HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    delay_spin(1000);
}

static uint32_t hda_make_verb(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t payload) {
    return ((uint32_t)cad << 28) | ((uint32_t)nid << 20) | ((uint32_t)verb << 8) | (uint32_t)payload;
}

static int hda_verb_sync(volatile uint8_t* m, uint8_t cad, uint8_t nid, uint16_t verb, uint8_t payload,
                         uint32_t* resp) {
    uint32_t v = hda_make_verb(cad, nid, verb, payload);
    uint16_t cwp;
    uint16_t rp;
    uint8_t rirb_wp_before;
    volatile uint64_t* rirb = g_hda.rirb;

    if (!g_hda.corb || !rirb)
        return -1;

    cwp = g_hda.corb_wp;
    rp = mmio_r16(m, HDA_REG_CORBRP) & 0xFFu;
    if (((cwp + 1u) & 0xFFu) == rp)
        return -1;

    rirb_wp_before = (uint8_t)(mmio_r16(m, HDA_REG_RIRBWP) & 0xFFu);

    g_hda.corb[cwp & 0xFFu] = v;
    g_hda.corb_wp = (uint16_t)((cwp + 1u) & 0xFFu);
    mmio_w16(m, HDA_REG_CORBWP, g_hda.corb_wp);
    __asm__ volatile("mfence" ::: "memory");

    {
        uint8_t expect = (uint8_t)((rirb_wp_before + 1u) & 0xFFu);
        int t = 0;
        while (t++ < 200000) {
            if ((uint8_t)(mmio_r16(m, HDA_REG_RIRBWP) & 0xFFu) == expect) {
                uint64_t ent = rirb[rirb_wp_before];
                if (resp)
                    *resp = (uint32_t)ent;
                return 0;
            }
            delay_spin(20);
        }
    }
    return -1;
}

static uint32_t hda_get_param(volatile uint8_t* m, uint8_t cad, uint8_t nid, uint8_t param) {
    uint32_t r = 0;
    if (hda_verb_sync(m, cad, nid, HDA_VERB_GET_PARAM, param, &r) != 0)
        return 0;
    return r;
}

static void hda_scan_widgets(volatile uint8_t* m) {
    uint32_t sub, i;
    uint8_t cad = g_hda.codec_addr;
    uint8_t first, count;

    g_hda.dac_nid = 0;
    g_hda.pin_nid = 0;

    sub = hda_get_param(m, cad, 0, AC_PAR_SUBNODE_COUNT);
    first = (uint8_t)((sub >> 16) & 0xFFu);
    count = (uint8_t)(sub & 0xFFu);
    if (count == 0)
        return;

    for (i = 0; i < count && i < 64u; i++) {
        uint8_t nid = (uint8_t)(first + i);
        uint32_t wc = hda_get_param(m, cad, nid, AC_PAR_WIDGET_CAP);
        uint32_t typ = (wc & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
        if (typ == AC_WID_AUD_OUT && g_hda.dac_nid == 0)
            g_hda.dac_nid = nid;
        if (typ == AC_WID_PIN_COMPLEX && g_hda.pin_nid == 0)
            g_hda.pin_nid = nid;
    }
}

static unsigned hda_sd_out_offset(void) {
    return HDA_STREAM_BASE + (unsigned)g_hda.iss * HDA_STREAM_STRIDE;
}

static void sd_w32(volatile uint8_t* m, unsigned sd, unsigned reg, uint32_t v) {
    mmio_w32(m, sd + reg, v);
}

static uint32_t sd_r32(volatile uint8_t* m, unsigned sd, unsigned reg) {
    return mmio_r32(m, sd + reg);
}

static uint8_t sd_r8(volatile uint8_t* m, unsigned sd, unsigned reg) {
    return mmio_r8(m, sd + reg);
}

void hda_play_pcm(uint8_t* buffer, size_t size) {
    volatile uint8_t* m;
    unsigned sd;
    HdaBdlEntry* bdl;
    uintptr_t buf_phys;
    size_t play_len;
    uint8_t* play_buf = buffer;

    if (!g_hda.ok || !buffer || size < 16u)
        return;

    play_len = size & ~(size_t)127u;
    if (play_len < 128u)
        play_len = 128u;
    if (play_len > size)
        play_len = size;

    m = g_hda.mmio;
    sd = g_hda.sd_out_base;

    if (!g_hda.bdl) {
        g_hda.bdl = (uint32_t*)kmalloc(sizeof(HdaBdlEntry) * 4u + 64u);
        if (!g_hda.bdl)
            return;
        {
            uintptr_t p = ((uintptr_t)g_hda.bdl + 15u) & ~15u;
            g_hda.bdl = (uint32_t*)p;
            g_hda.bdl_phys = p;
        }
    }

    bdl = (HdaBdlEntry*)g_hda.bdl;
    buf_phys = (uintptr_t)play_buf;

    bdl[0].addr_lo = (uint32_t)buf_phys;
    bdl[0].addr_hi = (uint32_t)(buf_phys >> 32);
    bdl[0].length = (uint32_t)play_len | (1u << 31);
    bdl[0].reserved = 0;

    sd_w32(m, sd, HDA_SD_OFFSET_CTL, sd_r32(m, sd, HDA_SD_OFFSET_CTL) | HDA_SD_CTL_SRST);
    {
        int t = 0;
        while (t++ < 50000 && (sd_r32(m, sd, HDA_SD_OFFSET_CTL) & HDA_SD_CTL_SRST))
            delay_spin(10);
    }

    /* 44.1 kHz, 16-bit, estéreo — formato estándar Azalia (bits según Linux/snd_hda_intel) */
    mmio_w16(m, sd + HDA_SD_OFFSET_FMT, 0x4011u);

    sd_w32(m, sd, HDA_SD_OFFSET_BDPL, (uint32_t)g_hda.bdl_phys);
    sd_w32(m, sd, HDA_SD_OFFSET_BDPU, (uint32_t)(g_hda.bdl_phys >> 32));
    sd_w32(m, sd, HDA_SD_OFFSET_CBL, (uint32_t)play_len);
    mmio_w8(m, sd + HDA_SD_OFFSET_LVI, 0);

    sd_w32(m, sd, HDA_SD_OFFSET_CTL, sd_r32(m, sd, HDA_SD_OFFSET_CTL) | HDA_SD_CTL_RUN);
    delay_spin(5000);

    {
        int t = 0;
        while (t++ < 5000000) {
            if (sd_r8(m, sd, HDA_SD_OFFSET_STS) & (1u << 2))
                break;
            delay_spin(50);
        }
    }

    mmio_w8(m, sd + HDA_SD_OFFSET_STS, (uint8_t)(1u << 2));
    sd_w32(m, sd, HDA_SD_OFFSET_CTL, sd_r32(m, sd, HDA_SD_OFFSET_CTL) & ~HDA_SD_CTL_RUN);
    delay_spin(2000);
}

int hda_init(void) {
    volatile uint8_t* m;
    uint16_t cmd;
    uint64_t phys, sz;
    uint32_t st;

    g_hda.ok = 0;
    g_hda.mmio = NULL;
    g_hda.corb = NULL;
    g_hda.rirb = NULL;
    g_hda.bdl = NULL;
    g_hda.codec_mask = 0;
    g_hda.codec_addr = 0;
    g_hda.dac_nid = 0;
    g_hda.pin_nid = 0;

    g_hda_find_result.valid = 0;
    pci_scan_bus(hda_pci_visit, NULL);
    if (!g_hda_find_result.valid)
        return -1;

    g_hda.pci = g_hda_find_result;

    cmd = pci_read16(g_hda.pci.bus, g_hda.pci.slot, g_hda.pci.func, PCI_CMD_REG);
    pci_write16(g_hda.pci.bus, g_hda.pci.slot, g_hda.pci.func, PCI_CMD_REG,
                (uint16_t)(cmd | PCI_CMD_BME | PCI_CMD_MSE));

    if (pci_read_bar0_mem(g_hda.pci.bus, g_hda.pci.slot, g_hda.pci.func, &phys, &sz) != 0)
        return -2;
    if (sz < 0x400u)
        return -2;

    g_hda.bar_phys = phys;
    g_hda.bar_size = (size_t)sz;

    hda_map_mmio(phys, (size_t)sz);
    g_hda.mmio = hda_mmio_va(phys);

    m = g_hda.mmio;

    hda_controller_reset(m);

    g_hda.gcap = mmio_r16(m, HDA_REG_GCAP);
    g_hda.iss = (uint8_t)((g_hda.gcap >> 8) & 0x0fu);
    g_hda.oss = (uint8_t)((g_hda.gcap >> 12) & 0x0fu);
    if (g_hda.oss == 0)
        return -3;

    g_hda.sd_out_base = hda_sd_out_offset();

    hda_corb_rirb_setup(m);
    if (!g_hda.corb)
        return -4;

    st = mmio_r16(m, HDA_REG_STATESTS);
    mmio_w16(m, HDA_REG_STATESTS, st);
    delay_spin(1000);

    if ((st & 0x0fu) == 0) {
        g_hda.ok = 1;
        return 0;
    }

    if (st & 1u)
        g_hda.codec_addr = 0;
    else if (st & 2u)
        g_hda.codec_addr = 1;
    else if (st & 4u)
        g_hda.codec_addr = 2;
    else if (st & 8u)
        g_hda.codec_addr = 3;
    g_hda.codec_mask = (uint8_t)(st & 0x0fu);

    hda_scan_widgets(m);

    g_hda.ok = 1;
    return 0;
}
