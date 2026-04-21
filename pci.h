#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    unsigned char  bus, slot, func;
    unsigned short vendor, device;
    unsigned char  valid;
} PciDevice;

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;    /* Base class */
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint8_t  valid;
} PciFunction;

unsigned int pci_read32(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off);
void         pci_write32(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off,
                         unsigned int val);
uint16_t     pci_read16(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off);
uint8_t      pci_read8(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off);
void         pci_write16(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off,
                         uint16_t val);
void         pci_write8(unsigned char bus, unsigned char slot, unsigned char func, unsigned char off,
                        uint8_t val);

PciDevice pci_find(unsigned short vendor, unsigned short device);

/* Multimedia / HD Audio (Prog IF suele ser 0x00). */
#define PCI_CLASS_MULTIMEDIA 0x04u
#define PCI_SUBCLASS_HDA     0x03u

/* Serial Bus / USB / xHCI (Prog IF 0x30). */
#define PCI_CLASS_SERIAL     0x0Cu
#define PCI_SUBCLASS_USB     0x03u
#define PCI_PROGIF_XHCI      0x30u

/* Devuelve la primera función que coincide; valid=0 si no hay. */
PciFunction pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

/* Iteración explícita (buses 0..PCI_MAX_BUS-1). */
#define PCI_MAX_BUS 256u
typedef int (*pci_visit_fn)(const PciFunction* f, void* user);
void pci_scan_bus(pci_visit_fn visitor, void* user);

/* Devuelve 1 si corremos dentro de VirtualBox */
int pci_detect_virtualbox(void);

#endif
