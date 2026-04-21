#include "nexus_ulib.h"

/*
 * Instalador: cada iteración pide al kernel un frame del instalador gráfico
 * (misma lógica que start_gui_installer, vía syscall).
 */
void _start(void) {
    for (;;)
        (void)nexus_wm(2, 0, 0);
}
