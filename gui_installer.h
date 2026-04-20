#ifndef NEXUS_GUI_INSTALLER_H
#define NEXUS_GUI_INSTALLER_H

/* Arranque gráfico (GRUB mode=gui): framebuffer, ratón, layout. */
void init_desktop(void);

/* Bucle del instalador (no retorna): pantalla de bienvenida + botón “Siguiente” decorativo. */
void start_gui_installer(void);

#endif
