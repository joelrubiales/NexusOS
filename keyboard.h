#ifndef NEXUS_KEYBOARD_H
#define NEXUS_KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);

/* Llamar desde el ISR de IRQ1 tras leer 0x60 (y conocer si el byte es extendido).
 * Devuelve !=0 si la tecla no debe propagarse a tecla_nueva (p. ej. TAB en instalador). */
int keyboard_irq(uint8_t scancode, int extended_prefix);

/* Bloquea hasta recibir un carácter imprimible, \\n, \\r, \\b, \\t o ESC (0x1B). */
char keyboard_getchar(void);

#endif
