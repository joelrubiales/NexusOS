#ifndef NEXUS_COMPOSITOR_H
#define NEXUS_COMPOSITOR_H

#include "window.h"

#define NEXUS_COMPOSITOR_SHADOW_PAD 20

void compositor_init(int screen_w, int screen_h, int stride_u32);
void compositor_shutdown(void);

/* Caché de escritorio (wallpaper + iconos), sin barra ni ventanas. */
void compositor_bake_wallpaper_layer(void);

void compositor_mark_full(void);
void compositor_damage_rect(int x, int y, int w, int h);
void compositor_damage_rect_pad(int x, int y, int w, int h, int pad);

/* Tras mover una ventana: región antigua y nueva (con margen para sombra). */
void compositor_notify_window_moved(const NWM_Window* w, int old_x, int old_y);

/* Cursor: invalida bbox anterior y nueva. */
void compositor_cursor_moved(int mx, int my);

/*
 * Compone solo las regiones marcadas; redibuja capas del escritorio según intersección.
 * dock_hover_out: mismo contrato que desktop_draw_dock.
 */
void compositor_render(int sw, int sh, int mx, int my, int* dock_hover_out);

/* Puente desde event_system / drivers (documentación + trazas futuras). */
void compositor_event_bridge(const void* ev /* const Event* */);

#endif
