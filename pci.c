#include "nexus.h"
#include "pci.h"

#define PCI_ADDR 0x0CF8
#define PCI_DATA 0x0CFC

unsigned int pci_read32(unsigned char bus, unsigned char slot,
                        unsigned char func, unsigned char off) {
    unsigned int addr = (1u << 31)
                      | ((unsigned int)bus  << 16)
                      | ((unsigned int)(slot & 0x1F) << 11)
                      | ((unsigned int)(func & 0x07) <<  8)
                      | (off & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

void pci_write32(unsigned char bus, unsigned char slot,
                 unsigned char func, unsigned char off, unsigned int val) {
    unsigned int addr = (1u << 31)
                      | ((unsigned int)bus  << 16)
                      | ((unsigned int)(slot & 0x1F) << 11)
                      | ((unsigned int)(func & 0x07) <<  8)
                      | (off & 0xFC);
    outl(PCI_ADDR, addr);
    outl(PCI_DATA, val);
}

uint16_t pci_read16(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off) {
    uint32_t w = pci_read32(bus, slot, func, off & 0xFCu);
    if ((off & 2u) != 0)
        return (uint16_t)(w >> 16);
    return (uint16_t)w;
}

uint8_t pci_read8(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off) {
    uint32_t w = pci_read32(bus, slot, func, off & 0xFCu);
    return (uint8_t)(w >> ((off & 3u) * 8u));
}

void pci_write16(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off,
                 uint16_t val) {
    uint32_t w = pci_read32(bus, slot, func, off & 0xFCu);
    if ((off & 2u) != 0)
        w = (w & 0xFFFFu) | ((uint32_t)val << 16);
    else
        w = (w & 0xFFFF0000u) | val;
    pci_write32(bus, slot, func, off & 0xFCu, w);
}

void pci_write8(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off,
                uint8_t val) {
    uint32_t w = pci_read32(bus, slot, func, off & 0xFCu);
    unsigned sh = (unsigned)(off & 3u) * 8u;
    w &= ~(0xFFu << sh);
    w |= (uint32_t)val << sh;
    pci_write32(bus, slot, func, off & 0xFCu, w);
}

static void pci_fill_function(PciFunction* o, uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t id = pci_read32(bus, slot, func, 0x00);
    uint32_t class_rev = pci_read32(bus, slot, func, 0x08);
    uint8_t ht = pci_read8(bus, slot, func, 0x0E);

    o->valid = 1;
    o->bus = bus;
    o->slot = slot;
    o->func = func;
    o->vendor_id = (uint16_t)(id & 0xFFFFu);
    o->device_id = (uint16_t)((id >> 16) & 0xFFFFu);
    o->class_code = (uint8_t)((class_rev >> 24) & 0xFFu);
    o->subclass = (uint8_t)((class_rev >> 16) & 0xFFu);
    o->prog_if = (uint8_t)((class_rev >> 8) & 0xFFu);
    o->header_type = (uint8_t)(ht & 0x7Fu);
    (void)ht;
}

PciFunction pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    PciFunction r;
    unsigned    b, s, f;

    r.valid = 0;
    for (b = 0; b < PCI_MAX_BUS; b++) {
        for (s = 0; s < 32u; s++) {
            uint32_t id0 = pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00);
            if (id0 == 0xFFFFFFFFu || (id0 & 0xFFFFu) == 0xFFFFu)
                continue;

            for (f = 0; f < 8u; f++) {
                uint32_t id = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00);
                if (id == 0xFFFFFFFFu || (id & 0xFFFFu) == 0xFFFFu)
                    continue;

                pci_fill_function(&r, (uint8_t)b, (uint8_t)s, (uint8_t)f);
                if (r.class_code == class_code && r.subclass == subclass && r.prog_if == prog_if)
                    return r;

                if (f == 0) {
                    uint8_t ht = pci_read8((uint8_t)b, (uint8_t)s, 0, 0x0E);
                    if ((ht & 0x80u) == 0)
                        break;
                }
            }
        }
    }
    return r;
}

void pci_scan_bus(pci_visit_fn visitor, void* user) {
    unsigned b, s, f;
    if (!visitor)
        return;
    for (b = 0; b < PCI_MAX_BUS; b++) {
        for (s = 0; s < 32u; s++) {
            uint32_t id0 = pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00);
            if (id0 == 0xFFFFFFFFu || (id0 & 0xFFFFu) == 0xFFFFu)
                continue;

            for (f = 0; f < 8u; f++) {
                uint32_t id = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00);
                if (id == 0xFFFFFFFFu || (id & 0xFFFFu) == 0xFFFFu)
                    continue;

                {
                    PciFunction pf;
                    pci_fill_function(&pf, (uint8_t)b, (uint8_t)s, (uint8_t)f);
                    if (visitor(&pf, user))
                        return;
                }

                if (f == 0) {
                    uint8_t ht = pci_read8((uint8_t)b, (uint8_t)s, 0, 0x0E);
                    if ((ht & 0x80u) == 0)
                        break;
                }
            }
        }
    }
}

PciDevice pci_find(unsigned short vendor, unsigned short device) {
    PciDevice r; r.valid = 0;
    for(unsigned char bus = 0; bus < 16; bus++) {
        for(unsigned char slot = 0; slot < 32; slot++) {
            unsigned int id = pci_read32(bus, slot, 0, 0x00);
            if((id & 0xFFFF) == vendor && ((id >> 16) & 0xFFFF) == device) {
                r.bus = bus; r.slot = slot; r.func = 0;
                r.vendor = vendor; r.device = device;
                r.valid = 1;
                return r;
            }
        }
    }
    return r;
}

int pci_detect_virtualbox() {
    // Comprueba si el hypervisor bit está activo (CPUID EAX=1, ECX bit 31)
    unsigned int ecx;
    __asm__ volatile("cpuid" : "=c"(ecx) : "a"(1) : "ebx","edx");
    if(!(ecx & (1u << 31))) return 0;

    // Leaf 0x40000000 → hypervisor brand
    unsigned int ebx, ecx2, edx;
    __asm__ volatile("cpuid"
        : "=b"(ebx), "=c"(ecx2), "=d"(edx)
        : "a"(0x40000000u) : );
    // VirtualBox: "VBoxVBoxVBox" → 0x786f4256 en EBX, ECX, EDX
    if(ebx == 0x786f4256u && ecx2 == 0x786f4256u && edx == 0x786f4256u)
        return 1;
    return 0;
}
