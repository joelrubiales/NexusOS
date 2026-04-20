#ifndef NEXUS_ICONS_H
#define NEXUS_ICONS_H

/* Iconos RGBA pre-renderizados (formato 0xAARRGGBB por píxel, MSB = alpha) */
#define ICON_RGBA_W   32
#define ICON_RGBA_H   32
#define ICON_RGBA_N   (ICON_RGBA_W * ICON_RGBA_H)

extern const unsigned int icon_rgba_globe[ICON_RGBA_N];
extern const unsigned int icon_rgba_folder[ICON_RGBA_N];
extern const unsigned int icon_rgba_terminal[ICON_RGBA_N];

#endif
