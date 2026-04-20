#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "gfx.h"

typedef void (*UI_ElementCallback)(void);

typedef struct UI_Element {
    int id;
    int x, y, w, h;
    UI_ElementCallback callback;
} UI_Element;

#define UI_MAX_ELEMENTS 16

extern UI_Element ui_elements[UI_MAX_ELEMENTS];
extern int ui_element_count;
extern int focused_element_index;

void ui_focus_advance(void);
void ui_redraw(void);
void ui_focus_clear(void);

/* Hit-test (índices más altos primero si hay solape). Sincroniza foco con el ratón cada frame. */
int  ui_get_element_at(int x, int y);
void ui_update_focus_from_mouse(int mx, int my);
void ui_activate_focused(void);

void gui_run(void);
void start_gui(void);

/* Doble búfer: reserva pitch×height con kmalloc, enlaza al motor gfx y vuelca con swap_buffers. */
int  gui_framebuffer_init_kmalloc(const VesaBootInfo* vbi);

/* Primitivas sobre el backbuffer (mismo layout que el LFB: stride = pitch/4 uint32). */
void gui_put_pixel(int x, int y, uint32_t rgb);
void gui_draw_rect(int x, int y, int w, int h, uint32_t rgb);

/* Copia el backbuffer completo al framebuffer físico (fin de frame). */
void swap_buffers(void);

#endif
