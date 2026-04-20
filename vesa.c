#include "nexus.h"

/* Compat: el doble búfer se reserva en gui.c con kmalloc; solo refresco consola VESA si aplica. */

void vesa_double_buffer_init(void) {
    if (vesa_console_active)
        vesa_force_refresh();
}
