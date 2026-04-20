#include "nexus.h"
#include "pci.h"
#include "nic.h"

// ─── helpers ─────────────────────────────────────────────────────────────────
static char hex_nibble(unsigned char v) { return v < 10 ? (char)('0'+v) : (char)('a'+(v-10)); }

static void str_copy(char* d, const char* s) {
    int i = 0; while(s[i]) { d[i]=s[i]; i++; } d[i]='\0';
}

// ─── RTL8139 (Vendor 0x10EC / Device 0x8139) ─────────────────────────────────
static NicInfo try_rtl8139() {
    NicInfo n; n.valid = 0;
    PciDevice d = pci_find(0x10EC, 0x8139);
    if(!d.valid) return n;

    unsigned int bar0 = pci_read32(d.bus, d.slot, 0, 0x10);
    if(!(bar0 & 1)) return n;          // debe ser IO space
    unsigned short io = (unsigned short)(bar0 & 0xFFFC);

    // Habilitar IO + bus master
    unsigned int cmd = pci_read32(d.bus, d.slot, 0, 0x04);
    pci_write32(d.bus, d.slot, 0, 0x04, cmd | 0x05);

    for(int i = 0; i < 6; i++) n.mac[i] = inb((unsigned short)(io + i));
    n.valid = 1;
    str_copy(n.name, "RTL8139");
    return n;
}

// ─── Intel E1000 (varios device IDs) ─────────────────────────────────────────
static const unsigned short e1000_ids[] = {
    0x100E, 0x100F, 0x1011, 0x1026, 0x1027, 0x1028,
    0x107C, 0x10D3, 0x1533, 0x0000  // termina en 0
};

static NicInfo try_e1000() {
    NicInfo n; n.valid = 0;
    PciDevice d; d.valid = 0;

    for(int i = 0; e1000_ids[i]; i++) {
        d = pci_find(0x8086, e1000_ids[i]);
        if(d.valid) break;
    }
    if(!d.valid) return n;

    unsigned int bar0 = pci_read32(d.bus, d.slot, 0, 0x10);
    if(bar0 & 1) return n;             // esperamos MMIO (bit 0 = 0)
    unsigned int mmio_base = bar0 & 0xFFFFFFF0;
    if(mmio_base == 0) return n;

    // Habilitar MMIO + bus master
    unsigned int cmd = pci_read32(d.bus, d.slot, 0, 0x04);
    pci_write32(d.bus, d.slot, 0, 0x04, cmd | 0x06);

    // RAL0 (0x5400) y RAH0 (0x5404) contienen la MAC tras reset
    volatile unsigned int* mmio = (volatile unsigned int*)(__UINTPTR_TYPE__)mmio_base;
    unsigned int ral = mmio[0x5400 / 4];
    unsigned int rah = mmio[0x5404 / 4];

    // Si RAH no tiene el bit de dirección válida (bit 31) algo falla
    if(!(rah & (1u << 31))) {
        // Intento alternativo: EERD (EEPROM read)
        for(int word = 0; word < 3; word++) {
            mmio[0x0014 / 4] = (unsigned int)((word << 8) | 1); // START
            for(volatile int t = 0; t < 100000; t++) {
                if(mmio[0x0014 / 4] & (1u << 4)) break;         // DONE
            }
            unsigned short data = (unsigned short)(mmio[0x0014 / 4] >> 16);
            n.mac[word*2]   = (unsigned char)(data & 0xFF);
            n.mac[word*2+1] = (unsigned char)(data >> 8);
        }
    } else {
        n.mac[0] = (unsigned char)( ral        & 0xFF);
        n.mac[1] = (unsigned char)((ral >>  8) & 0xFF);
        n.mac[2] = (unsigned char)((ral >> 16) & 0xFF);
        n.mac[3] = (unsigned char)((ral >> 24) & 0xFF);
        n.mac[4] = (unsigned char)( rah        & 0xFF);
        n.mac[5] = (unsigned char)((rah >>  8) & 0xFF);
    }

    // Validar que la MAC no sea 00:00:00:00:00:00 ni FF:FF:FF:FF:FF:FF
    unsigned int sum = 0;
    for(int i = 0; i < 6; i++) sum += n.mac[i];
    if(sum == 0 || sum == 6*255) return n;

    n.valid = 1;
    str_copy(n.name, "E1000");
    return n;
}

// ─── PCnet-FAST III (0x1022 / 0x2000) ────────────────────────────────────────
static NicInfo try_pcnet() {
    NicInfo n; n.valid = 0;
    PciDevice d = pci_find(0x1022, 0x2000);
    if(!d.valid) d = pci_find(0x1022, 0x2001);
    if(!d.valid) return n;

    unsigned int bar0 = pci_read32(d.bus, d.slot, 0, 0x10);
    if(!(bar0 & 1)) return n;
    unsigned short io = (unsigned short)(bar0 & 0xFFFC);

    unsigned int cmd = pci_read32(d.bus, d.slot, 0, 0x04);
    pci_write32(d.bus, d.slot, 0, 0x04, cmd | 0x05);

    for(int i = 0; i < 6; i++) n.mac[i] = inb((unsigned short)(io + i));
    n.valid = 1;
    str_copy(n.name, "PCnet");
    return n;
}

// ─── API pública ──────────────────────────────────────────────────────────────
NicInfo nic_detect() {
    NicInfo n;
    n = try_e1000();   if(n.valid) return n;
    n = try_rtl8139(); if(n.valid) return n;
    n = try_pcnet();   if(n.valid) return n;
    n.valid = 0; n.name[0] = '\0';
    return n;
}

void nic_mac_str(const NicInfo* nic, char* out) {
    for(int i = 0; i < 6; i++) {
        out[i*3]   = hex_nibble((nic->mac[i] >> 4) & 0xF);
        out[i*3+1] = hex_nibble( nic->mac[i]       & 0xF);
        out[i*3+2] = (i < 5) ? ':' : '\0';
    }
    out[17] = '\0';
}
