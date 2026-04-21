#ifndef NEXUS_UI_MANAGER_H
#define NEXUS_UI_MANAGER_H

#include <stdint.h>

typedef void (*ui_on_click_fn)(void);
typedef void (*ui_on_key_fn)(char key);

/*
 * Nodo visual ligado al frame actual de ui_elements[] (mismo orden / índices).
 */
typedef struct ui_node {
    int             x, y;
    int             width, height;
    int             is_focusable;
    int             is_hovered;
    int             has_focus;
    int             element_index;
    ui_on_click_fn  on_click;
    ui_on_key_fn    on_key;
} ui_node_t;

extern int ui_manager_focused_node_index;

void ui_manager_clear(void);

/* Copia geometría y callbacks desde ui_elements[] (tras ui_push_*). */
void ui_manager_sync_from_elements(void);

/* has_focus según focused_element_index (tras ui_focus_chain_rebuild / Tab). */
void ui_manager_sync_focus_flags(void);

/* Hit-test: solo is_hovered (no mueve el foco de teclado). */
void ui_manager_update_hover(int mouse_x, int mouse_y);

/* Índice en ui_elements[] bajo el cursor, o -1. */
int ui_manager_hover_element_index(void);
int ui_manager_element_is_hovered(int element_index);

/*
 * Clic primario: foco si focusable, luego ui_activate_focused() (checkbox, botón…).
 * Devuelve 1 si el clic cayó sobre un nodo (consumido).
 */
int ui_manager_handle_primary_click(int mouse_x, int mouse_y);

/* Anillo de foco ~3 px, azul translúcido (después de pintar widgets). */
void ui_manager_draw_focus_rings(void);

#endif
