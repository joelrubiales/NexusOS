#ifndef E1000_H
#define E1000_H

#include <stdint.h>

/* Intel 8254x / e1000 (QEMU default: 82540EM 8086:100E). */

int  e1000_init(void);
void e1000_irq_body(void);

/* Envía un frame Ethernet crudo (longitud mínima 60 rellenada en el driver). */
int e1000_transmit(const void* frame, uint16_t len);

int  e1000_present(void);
void e1000_get_mac(uint8_t mac[6]);

/* Si no hay línea PIC válida (0xFF), el driver usa sondeo desde el scheduler. */
void e1000_poll_rx_if_needed(void);
int  e1000_needs_poll(void);

#endif
