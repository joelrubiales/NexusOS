#ifndef NEXUS_INSTALLER_UI_H
#define NEXUS_INSTALLER_UI_H

#include "window.h"
#include "bmp_loader.h"

/* Assets BMP (NULL = fallback primitivo). Asignar tras load_bmp desde initrd/VFS. */
extern Image* installer_bg_image;
extern Image* installer_window_bg;
extern Image* installer_btn_hover;

/* Fondo a pantalla completa (escalado) sobre el backbuffer GUI. */
void installer_paint_background_fullscreen(void);

/* Debe coincidir con WM_TITLE_H / WM_CLIENT_INSET en window.c */
#define INSTALLER_WIN_TITLE_H   52
#define INSTALLER_WIN_INSET     4

/*
 * Asistente multi-paso (estilo Calamares / macOS / GTK4).
 * La barra lateral refleja el mismo orden enumerado en el diseño.
 */
typedef enum InstallerState {
    WELCOME,           /* 1. Bienvenida — idioma */
    LOCALE,            /* 2. Localización — región / huso (mapa simulado) */
    USER_ACCOUNT,      /* 3. Usuario — nombre, login, contraseña, hostname */
    DISK_SETUP,        /* 4. Disco — barra tipo GParted + opciones */
    SUMMARY,           /* 5. Resumen — revisar antes de instalar */
    INSTALLING,        /* 6. Instalación — TAR + slideshow */
    FINISHED           /* 7. Fin — reinicio */
} InstallerState;

extern InstallerState current_step;

void draw_installer_content(const Window* win);

#endif
