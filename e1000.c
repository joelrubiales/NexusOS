#include "e1000.h"
#include "nexus.h"
#include "pci.h"
#include "memory.h"
#include "net.h"
#include <stddef.h>

#define E1000_VENDOR_INTEL 0x8086u

static const uint16_t e1000_device_ids[] = {
    0x100Eu, 0x100Fu, 0x1011u, 0x1026u, 0x1027u, 0x1028u,
    0x107Cu, 0x10D3u, 0x1533u,
};

/* Registros (offsets MMIO 32-bit). */
#define E1000_CTRL       0x00000u
#define E1000_STATUS     0x00008u
#define E1000_CTRL_EXT   0x00018u
#define E1000_ICR        0x000C0u
#define E1000_IMS        0x000D0u
#define E1000_IMC        0x000D8u
#define E1000_RCTL       0x00100u
#define E1000_TCTL       0x00400u
#define E1000_TIPG       0x00410u
#define E1000_EERD       0x00014u
#define E1000_RDTR       0x02820u
#define E1000_RDBAL      0x02800u
#define E1000_RDBAH      0x02804u
#define E1000_RDLEN      0x02808u
#define E1000_RDH        0x02810u
#define E1000_RDT        0x02818u
#define E1000_TDBAL      0x03800u
#define E1000_TDBAH      0x03804u
#define E1000_TDLEN      0x03808u
#define E1000_TDH        0x03810u
#define E1000_TDT        0x03818u
#define E1000_RAL0       0x05400u
#define E1000_RAH0       0x05404u

#define E1000_CTRL_RST   (1u << 26)
#define E1000_CTRL_SLU   (1u << 14)

#define E1000_RCTL_EN    (1u << 1)
#define E1000_RCTL_SBP   (1u << 2)
#define E1000_RCTL_BAM   (1u << 15)
#define E1000_RCTL_SECRC (1u << 26)

#define E1000_TCTL_EN    (1u << 1)
#define E1000_TCTL_PSP   (1u << 3)
#define E1000_TCTL_CT    (0x10u << 4)
#define E1000_TCTL_COLD  (0x40u << 12)

#define E1000_TXD_CMD_EOP  0x01u
#define E1000_TXD_CMD_IFCS 0x02u
#define E1000_TXD_CMD_RS   0x08u
#define E1000_TXD_STAT_DD  0x01u

#define E1000_RXD_STAT_DD  0x01u
#define E1000_RXD_STAT_EOP 0x02u

#define E1000_IMS_RXT0     (1u << 7)
#define E1000_IMS_RXO      (1u << 6)
#define E1000_IMS_TXDW     (1u << 0)
#define E1000_IMS_LSC      (1u << 2)

#define RX_RING_SIZE 32u
#define TX_RING_SIZE 8u
#define PACKET_MAX   2048u

typedef struct {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

static volatile uint32_t* e1000_mmio;
static uintptr_t        e1000_mmio_phys;
static uint8_t          e1000_mac[6];
static int              e1000_ok;
static int              e1000_poll_mode;
static uint8_t          e1000_irq_line = 0xFFu;

static e1000_rx_desc_t* rx_ring;
static e1000_tx_desc_t* tx_ring;
static uintptr_t        rx_ring_phys;
static uintptr_t        tx_ring_phys;
static uint8_t*         rx_bufs[RX_RING_SIZE];
static uintptr_t        rx_bufs_phys[RX_RING_SIZE];
static uint8_t          tx_buf[PACKET_MAX] __attribute__((aligned(16)));
static uintptr_t        tx_buf_phys;
static uint32_t         rx_next;
static uint32_t         tx_tail;

static uint32_t e1000_read32(uint32_t reg) {
    return e1000_mmio[reg / 4u];
}

static void e1000_write32(uint32_t reg, uint32_t val) {
    e1000_mmio[reg / 4u] = val;
    __asm__ volatile("mfence" ::: "memory");
}

static void e1000_mac_from_eeprom(void) {
    unsigned w;
    for (w = 0; w < 3u; w++) {
        uint32_t v = (uint32_t)(w << 8) | 1u;
        int t;
        e1000_write32(E1000_EERD, v);
        for (t = 0; t < 100000; t++) {
            uint32_t r = e1000_read32(E1000_EERD);
            if (r & (1u << 4))
                break;
        }
        {
            uint32_t r = e1000_read32(E1000_EERD);
            uint16_t d = (uint16_t)(r >> 16);
            e1000_mac[w * 2u]     = (uint8_t)(d & 0xFFu);
            e1000_mac[w * 2u + 1u] = (uint8_t)(d >> 8);
        }
    }
}

static void e1000_read_mac(void) {
    uint32_t ral = e1000_read32(E1000_RAL0);
    uint32_t rah = e1000_read32(E1000_RAH0);
    if (rah & (1u << 31)) {
        e1000_mac[0] = (uint8_t)(ral & 0xFFu);
        e1000_mac[1] = (uint8_t)((ral >> 8) & 0xFFu);
        e1000_mac[2] = (uint8_t)((ral >> 16) & 0xFFu);
        e1000_mac[3] = (uint8_t)((ral >> 24) & 0xFFu);
        e1000_mac[4] = (uint8_t)(rah & 0xFFu);
        e1000_mac[5] = (uint8_t)((rah >> 8) & 0xFFu);
    } else {
        e1000_mac_from_eeprom();
    }
}

static int pci_bar0_mem(const PciDevice* d, uint64_t* phys, int* is_64) {
    uint32_t lo = pci_read32(d->bus, d->slot, d->func, 0x10);
    if (lo & 1u)
        return -1;
    *is_64 = ((lo & 0x6u) == 0x4u) ? 1 : 0;
    *phys = (uint64_t)(lo & ~0xFULL);
    if (*is_64) {
        uint32_t hi = pci_read32(d->bus, d->slot, d->func, 0x14);
        *phys |= ((uint64_t)hi << 32);
    }
    return 0;
}

static int e1000_find_pci(PciDevice* out) {
    size_t i;
    for (i = 0; i < sizeof(e1000_device_ids) / sizeof(e1000_device_ids[0]); i++) {
        *out = pci_find(E1000_VENDOR_INTEL, e1000_device_ids[i]);
        if (out->valid)
            return 0;
    }
    return -1;
}

static void e1000_map_mmio(uint64_t base) {
    uint64_t a;
    e1000_mmio_phys = (uintptr_t)base;
    for (a = base & ~(PAGE_SIZE - 1); a < base + 128u * 1024u; a += PAGE_SIZE)
        vmm_map_page(a, a, VMM_PAGE_MMIO);
    e1000_mmio = (volatile uint32_t*)(uintptr_t)base;
}

static void e1000_rx_drain(void) {
    while (1) {
        e1000_rx_desc_t* d = &rx_ring[rx_next];
        if (!(d->status & E1000_RXD_STAT_DD))
            break;
        if ((d->status & E1000_RXD_STAT_EOP) && d->length > 0u)
            net_rx_frame(rx_bufs[rx_next], d->length);
        d->status = 0;
        __sync_synchronize();
        e1000_write32(E1000_RDT, rx_next);
        rx_next = (rx_next + 1u) % RX_RING_SIZE;
    }
}

void e1000_poll_rx_if_needed(void) {
    if (!e1000_ok || !e1000_poll_mode)
        return;
    e1000_rx_drain();
}

int e1000_needs_poll(void) { return e1000_ok && e1000_poll_mode; }

void e1000_irq_body(void) {
    (void)e1000_read32(E1000_ICR);
    e1000_rx_drain();
    if (e1000_irq_line < 8u)
        outb(0x20, 0x20);
    else {
        outb(0xA0, 0x20);
        outb(0x20, 0x20);
    }
}

int e1000_transmit(const void* frame, uint16_t len) {
    e1000_tx_desc_t* td;
    if (!e1000_ok || !frame || len == 0u || len > PACKET_MAX)
        return -1;
    if (len < 60u) {
        __builtin_memcpy(tx_buf, frame, len);
        __builtin_memset(tx_buf + len, 0, (size_t)(60u - len));
        len = 60u;
    } else {
        __builtin_memcpy(tx_buf, frame, len);
    }

    td = &tx_ring[tx_tail];
    if (!(td->status & E1000_TXD_STAT_DD)) {
        unsigned spin = 1u << 24;
        while (!(td->status & E1000_TXD_STAT_DD) && spin--)
            __asm__ volatile("pause" ::: "memory");
        if (!(td->status & E1000_TXD_STAT_DD))
            return -1;
    }

    td->buffer_addr = (uint64_t)tx_buf_phys;
    td->length = len;
    td->cso = 0;
    td->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    td->status = 0;
    td->css = 0;
    td->special = 0;
    __sync_synchronize();

    {
        uint32_t next = (tx_tail + 1u) % TX_RING_SIZE;
        e1000_write32(E1000_TDT, next);
        while (!(td->status & E1000_TXD_STAT_DD))
            __asm__ volatile("pause" ::: "memory");
        tx_tail = next;
    }
    return 0;
}

int e1000_present(void) { return e1000_ok; }

void e1000_get_mac(uint8_t mac[6]) {
    unsigned i;
    for (i = 0; i < 6u; i++)
        mac[i] = e1000_mac[i];
}

extern void e1000_handler(void);

int e1000_init(void) {
    PciDevice d;
    uint64_t bar_phys;
    int bar64 = 0;
    uint32_t ctrl;
    unsigned i;
    uint32_t ims_mask;

    e1000_ok = 0;
    e1000_poll_mode = 0;
    e1000_mmio = 0;
    rx_ring = 0;
    tx_ring = 0;
    rx_next = 0;
    tx_tail = 0;

    if (e1000_find_pci(&d) != 0)
        return -1;

    if (pci_bar0_mem(&d, &bar_phys, &bar64) != 0 || bar_phys == 0)
        return -1;

    {
        uint32_t cmd = pci_read32(d.bus, d.slot, d.func, 0x04);
        pci_write32(d.bus, d.slot, d.func, 0x04, cmd | 0x06u);
    }

    e1000_irq_line = pci_read8(d.bus, d.slot, d.func, 0x3Cu);
    e1000_map_mmio(bar_phys);

    ctrl = e1000_read32(E1000_CTRL);
    e1000_write32(E1000_CTRL, ctrl | E1000_CTRL_RST);
    {
        int t;
        for (t = 0; t < 100000; t++) {
            if (!(e1000_read32(E1000_CTRL) & E1000_CTRL_RST))
                break;
        }
    }

    ctrl = e1000_read32(E1000_CTRL);
    e1000_write32(E1000_CTRL, ctrl | E1000_CTRL_SLU);

    e1000_write32(E1000_IMC, 0xFFFFFFFFu);

    e1000_read_mac();

    rx_ring = (e1000_rx_desc_t*)kmalloc((uint64_t)RX_RING_SIZE * sizeof(e1000_rx_desc_t));
    tx_ring = (e1000_tx_desc_t*)kmalloc((uint64_t)TX_RING_SIZE * sizeof(e1000_tx_desc_t));
    if (!rx_ring || !tx_ring)
        return -1;
    rx_ring_phys = (uintptr_t)rx_ring;
    tx_ring_phys = (uintptr_t)tx_ring;
    tx_buf_phys = (uintptr_t)tx_buf;

    for (i = 0; i < RX_RING_SIZE; i++) {
        rx_bufs[i] = (uint8_t*)kmalloc(PACKET_MAX);
        if (!rx_bufs[i])
            return -1;
        rx_bufs_phys[i] = (uintptr_t)rx_bufs[i];
        __builtin_memset(&rx_ring[i], 0, sizeof(rx_ring[i]));
        rx_ring[i].buffer_addr = (uint64_t)rx_bufs_phys[i];
    }

    for (i = 0; i < TX_RING_SIZE; i++) {
        __builtin_memset(&tx_ring[i], 0, sizeof(tx_ring[i]));
        tx_ring[i].status = E1000_TXD_STAT_DD;
    }

    e1000_write32(E1000_RDBAL, (uint32_t)rx_ring_phys);
    e1000_write32(E1000_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    e1000_write32(E1000_RDLEN, RX_RING_SIZE * (uint32_t)sizeof(e1000_rx_desc_t));
    e1000_write32(E1000_RDH, 0);
    e1000_write32(E1000_RDT, RX_RING_SIZE - 1u);
    e1000_write32(E1000_RDTR, 0);

    e1000_write32(E1000_TDBAL, (uint32_t)tx_ring_phys);
    e1000_write32(E1000_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    e1000_write32(E1000_TDLEN, TX_RING_SIZE * (uint32_t)sizeof(e1000_tx_desc_t));
    e1000_write32(E1000_TDH, 0);
    e1000_write32(E1000_TDT, 0);
    tx_tail = 0;

    e1000_write32(E1000_TIPG, 0x0060200Au);
    e1000_write32(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD);

    e1000_write32(E1000_RCTL,
                  E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);

    ims_mask = E1000_IMS_RXT0 | E1000_IMS_RXO | E1000_IMS_LSC | E1000_IMS_TXDW;
    e1000_write32(E1000_IMS, ims_mask);

    if (e1000_irq_line != 0xFFu && e1000_irq_line < 16u) {
        idt_irq_install(e1000_irq_line, e1000_handler);
        pic_irq_unmask(e1000_irq_line);
    } else
        e1000_poll_mode = 1;

    e1000_ok = 1;
    (void)bar64;
    return 0;
}
