#ifndef NEXUS_KEYBOARD_H
#define NEXUS_KEYBOARD_H

#include <stdint.h>

#include "event.h"
#include "event_queue.h"

void keyboard_init(void);

void keyboard_event_flush_state(void);

int keyboard_os_event_to_gui(const os_event_t* o, Event* out);

/* Llamar desde el ISR de IRQ1 tras leer 0x60 (y conocer si el byte es extendido). */
int keyboard_irq(uint8_t scancode, int extended_prefix);

/* Bloquea hasta recibir un carácter imprimible vía la misma cola que la GUI. */
char keyboard_getchar(void);

#endif
