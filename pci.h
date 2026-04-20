#ifndef PCI_H
#define PCI_H

typedef struct {
    unsigned char  bus, slot, func;
    unsigned short vendor, device;
    unsigned char  valid;
} PciDevice;

unsigned int pci_read32 (unsigned char bus, unsigned char slot, unsigned char func, unsigned char off);
void         pci_write32(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off, unsigned int val);
PciDevice    pci_find   (unsigned short vendor, unsigned short device);

// Devuelve 1 si corremos dentro de VirtualBox
int pci_detect_virtualbox();

#endif
