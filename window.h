#ifndef NEXUS_WINDOW_H
#define NEXUS_WINDOW_H

#include "gfx.h"

#define NWM_COL_FRAME_ACT     RGB(48, 50, 72)
#define NWM_COL_FRAME_INACT   RGB(36, 38, 56)
#define NWM_COL_TITLE_ACT     RGB(42, 44, 62)
#define NWM_COL_TITLE_INACT   RGB(30, 32, 48)
#define NWM_COL_BODY          RGB(30, 32, 46)
#define NWM_COL_BORDER_ACT    RGB(100, 110, 160)
#define NWM_COL_BORDER_INACT  RGB(60, 62, 82)

/* ── Window Manager (instalador / apps compuestas) ─────────────────── */
typedef struct Window {
    int x, y;
    int width, height;
    const char* title;
    int is_visible;
    int is_active;
    int z_index;
} Window;

void draw_window(const Window* w);

/* Altura de título y radio: usar layout_chrome_title_h / layout_chrome_corner_r (gfx.h). */
typedef struct NWM_Window {
    int x, y, w, h;
    int visible;
    int focused;
    int dragging;
    int drag_ox, drag_oy;
    const char* title;
    int app_type;
    unsigned int* own_buffer;
} NWM_Window;

void nwm_draw_window_frame(const NWM_Window* win, int title_h, int corner_r);

void nwm_paint_painter_order(const NWM_Window* wins, int nwin, const int* zorder, int zcount,
    void (*draw_client)(void* user, int win_idx), void* user);

int nwm_point_in_rounded_rect(int px, int py, int x, int y, int w, int h, int r);

#endif
