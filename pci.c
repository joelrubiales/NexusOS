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
