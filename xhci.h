#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

/*
 * xHCI: PCI 0x0C/0x03/0x30, MMIO PCD, legacy handoff, anillos, RUN,
 * reset de puerto, Address Device, descriptores, HID boot mouse (polling).
 */

typedef struct {
    int      present;
    uint8_t  bus, slot, func;
    uint64_t bar_phys;   /* BAR0 (mem), alineado */
    uint32_t bar_size;   /* tamaño máscara PCI */
    uint8_t  cap_length;
    uint16_t hci_version;
    uint32_t hcs_params1;
    uint32_t hcs_params2;
    uint32_t hcs_params3;
    uint32_t hcc_params;
    uint32_t db_off;
    uint32_t rt_off;
} XhciInfo;

/* mb2_phys: MBI Multiboot2 para MCFG (opcional); puede ser 0. */
int xhci_init(uint32_t mb2_phys, XhciInfo* info_out);

/* 1 si hay ratón HID USB listo (evitar PS/2). */
extern volatile int xhci_usb_mouse_active;

void xhci_poll(void);
void xhci_set_screen_dims(int w, int h);

#endif
