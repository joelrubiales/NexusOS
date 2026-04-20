#ifndef NEXUS_DESKTOP_H
#define NEXUS_DESKTOP_H

#include "window.h"

/* Política resolution-independent: valores de GRUB (screen_width / screen_height). */
#define DESK_TOP_BAR_HEIGHT     30
#define DESK_DOCK_WIDTH_PCENT   70
#define DESK_DOCK_BOTTOM_PAD    12
#define DESK_DOCK_MIN_HEIGHT    44

/* Capas del escritorio Live: fondo → iconos (draw_desktop = ambas). */
void desktop_paint_wallpaper_layer(void);
void desktop_paint_desktop_icons(void);
void draw_desktop(void);

/* Registro del WM compuesto (orden por z_index, pintado tras ventanas NWM en gui.c). */
extern Window installer_win;
void desktop_wm_register(Window* w);
void desktop_wm_init(void);
void desktop_paint_wm_windows(void);

/* Barra superior “mica” (oscuro translúcido) sobre el gradiente. */
void desktop_draw_top_bar(void);

void desktop_draw_wallpaper(int x, int y, int w, int h);

#define DESK_DOCK_ITEMS        6
#define DESK_GLASS_ALPHA       135

void desktop_get_dock_geometry(int sw, int sh, int* dx, int* dy, int* dw, int* dh);
void desktop_get_launcher_rect(int sw, int sh, int* lx, int* ly, int* lw, int* lh);
void desktop_draw_dock(int sw, int sh, int mx, int my, int* out_hover);
void desktop_draw_app_menu(int open, int sel, int anchor_x, int anchor_y);

/* -1 = fuera, -2 = launcher, 0..DESK_DOCK_ITEMS-1 = icono */
int desktop_dock_hit(int sw, int sh, int px, int py);
int desktop_menu_hit(int anchor_x, int anchor_y, int px, int py);
int desktop_menu_contains(int anchor_x, int anchor_y, int px, int py);

#endif
