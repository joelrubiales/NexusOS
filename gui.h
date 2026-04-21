#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "gfx.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Widget system — tipos, estructura y API pública.
 *
 *  Cada frame el instalador llama ui_element_count = 0 y re-registra los
 *  widgets con ui_push_*().  Los campos de estado (text_buffer, is_checked)
 *  NO se borran al registrar para que persistan entre frames.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Tipos de widget ───────────────────────────────────────────────────── */
#define UI_TYPE_BUTTON       0
#define UI_TYPE_TEXT_INPUT   1
#define UI_TYPE_CHECKBOX     2
#define UI_TYPE_PROGRESS_BAR 3

typedef void (*UI_ElementCallback)(void);
typedef void (*UI_KeypressFn)(char key);

typedef struct UI_Element {
    int  id;
    int  x, y, w, h;
    int  type;                  /* UI_TYPE_* */
    UI_ElementCallback callback;
    int                is_focusable; /* 0 = omitir en Tab (p. ej. progress) */
    UI_KeypressFn      on_keypress; /* opcional; NULL = teclas por defecto */

    /* TEXT_INPUT ─────────────────────────────────────────────────────── */
    char text_buffer[256];      /* contenido actual (null-terminado) */
    int  text_len;              /* == strlen(text_buffer) */
    char placeholder[64];       /* texto gris cuando text_buffer está vacío */
    int  is_password;           /* 1 = mostrar '*' (contenido real en text_buffer) */

    /* CHECKBOX ───────────────────────────────────────────────────────── */
    int  is_checked;
    char label[64];             /* texto a la derecha del cuadrado */

    /* PROGRESS_BAR ───────────────────────────────────────────────────── */
    int  progress_value;        /* 0 – 100 */
} UI_Element;

#define UI_MAX_ELEMENTS 16

extern UI_Element ui_elements[UI_MAX_ELEMENTS];
extern int        ui_element_count;
extern int        focused_element_index;

/* ── Foco / anillo de widgets focusables (orden de Tab) ───────────────── */
void ui_focus_advance(void);
void ui_focus_chain_rebuild(void);
void ui_focus_tab_next(void);
void ui_focus_tab_prev(void);
void ui_focus_reset_step(void);
void gui_blur_widget(int focus_idx);
void gui_focus_widget(int focus_idx);
void ui_sync_focus_ring_from_mouse(void);

void ui_redraw(void);
void ui_focus_clear(void);

/* ── Hit-test / ratón ─────────────────────────────────────────────────── */
int  ui_get_element_at(int x, int y);
void ui_update_focus_from_mouse(int mx, int my);

/* ── Activación ───────────────────────────────────────────────────────── */
void ui_activate_focused(void);

/* ── Registro de widgets ──────────────────────────────────────────────── */
/* Nota: no borra state (text_buffer, is_checked) para preservarlo entre frames. */
int  ui_push_button      (int id, int x, int y, int w, int h, UI_ElementCallback cb);
int  ui_push_text_input  (int id, int x, int y, int w, int h, const char* placeholder,
                          int is_password);
int  ui_push_checkbox    (int id, int x, int y, int w, const char* label, UI_ElementCallback cb);
int  ui_push_progress_bar(int id, int x, int y, int w, int h, int value);

/* ── Renderizado de widgets ───────────────────────────────────────────── */
void ui_draw_element    (int idx);          /* dibuja un widget por índice */
void ui_draw_all_elements(void);            /* dibuja todos los no-BUTTON  */

/* ── Entrada de texto ─────────────────────────────────────────────────── */
/*  Llamar desde keyboard_irq cuando el foco está en un TEXT_INPUT.
 *  ch = carácter ASCII (imprimible) o '\b' (backspace). */
void ui_handle_char(unsigned char ch);

/* ── Bucle GUI y utilidades ───────────────────────────────────────────── */
void gui_run(void);
void start_gui(void);

int  gui_framebuffer_init_kmalloc(const VesaBootInfo* vbi);

void gui_put_pixel(int x, int y, uint32_t rgb);
void gui_draw_rect(int x, int y, int w, int h, uint32_t rgb);
void swap_buffers(void);

/*
 * Un frame de escritorio: compositor (solo regiones dañadas en RAM) +
 * menú contextual + cursor + presentación parcial al LFB.
 */
void gui_render_frame(int sw, int sh, int mx, int my, int* dock_hover, int menu_o, int menu_s,
                      int ax, int ay);

/* ── Tema GTK/macOS (ARGB 0xAARRGGBB; el motor usa RGB en framebuffer) ─── */
extern uint32_t COLOR_BG_WINDOW;
extern uint32_t COLOR_ACCENT;
extern uint32_t COLOR_TEXT_PRIMARY;
extern uint32_t COLOR_BORDER;
extern uint32_t COLOR_DESKTOP_BG;

void draw_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color_argb);

/* Alpha-over: mezcla fg sobre bg usando el canal A de fg (sin float). Orden (bg, fg). */
uint32_t gui_blend_colors(uint32_t bg, uint32_t fg);

/* Relleno redondeado; color 0xAARRGGBB (alpha 0 en alto = opaco, convención gfx). */
void draw_rounded_rect_filled(int x, int y, int w, int h, int radius, uint32_t color);

/*
 * Sombra proyectada (capas redondeadas, mayor opacidad junto al borde).
 * base_color: tinte RGB (byte alto FF o 0); se usa el RGB de 24 bits bajos.
 */
void draw_drop_shadow(int x, int y, int w, int h, int radius, int spread, uint32_t base_color);

void draw_shadow_rect(int x, int y, int w, int h);

#endif
