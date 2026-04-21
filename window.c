#include "window.h"
#include "font_data.h"
#include "memory.h"
#include "compositor.h"

#define WM_TITLE_H        34

static int nwm_skip_shadow;
static NWM_Window* nwm_z_bottom;
static NWM_Window* nwm_z_top;
#define WM_OUTER_R        11
#define WM_CLIENT_INSET   4

static int slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

int nwm_point_in_rounded_rect(int px, int py, int x, int y, int w, int h, int r) {
    int rr = r;
    if (rr > h / 2) rr = h / 2;
    if (rr > w / 2) rr = w / 2;
    if (px < x || py < y || px >= x + w || py >= y + h) return 0;
    if (px >= x + rr && px < x + w - rr) return 1;
    if (py >= y + rr && py < y + h - rr) return 1;
    if (px < x + rr && py < y + rr) {
        int dx = px - (x + rr), dy = py - (y + rr);
        return dx * dx + dy * dy <= rr * rr;
    }
    if (px >= x + w - rr && py < y + rr) {
        int dx = px - (x + w - rr), dy = py - (y + rr);
        return dx * dx + dy * dy <= rr * rr;
    }
    if (px < x + rr && py >= y + h - rr) {
        int dx = px - (x + rr), dy = py - (y + h - rr);
        return dx * dx + dy * dy <= rr * rr;
    }
    if (px >= x + w - rr && py >= y + h - rr) {
        int dx = px - (x + w - rr), dy = py - (y + h - rr);
        return dx * dx + dy * dy <= rr * rr;
    }
    return 1;
}

/*
 * Ventana estilo instalador: sombra, marco gris muy claro, barra de título oscura,
 * minimizar (_) y cerrar (×) geométricos a la derecha, título centrado.
 */
void draw_window(const Window* w) {
    int x, y, ww, hh;
    unsigned int border_c, title_bg, title_fg;
    int tw, tx, min_tx, max_tx;
    int btn, gap, xr, ym;
    int title_y_text;

    if (!w || !w->is_visible || w->width < 120 || w->height < 80)
        return;

    x = w->x;
    y = w->y;
    ww = w->width;
    hh = w->height;

    border_c = w->is_active ? RGB(78, 82, 98) : RGB(175, 178, 188);
    title_bg = w->is_active ? RGB(26, 28, 34) : RGB(38, 40, 46);
    title_fg = RGB(236, 238, 245);

    gfx_drop_shadow_soft(x, y, ww, hh, WM_OUTER_R, 14);

    /* Cuerpo: blanco roto */
    gfx_fill_rounded_rect(x, y, ww, hh, WM_OUTER_R, RGB(245, 246, 250));
    gfx_rounded_rect_stroke_aa(x, y, ww, hh, WM_OUTER_R, border_c);

    /* Barra de título */
    gfx_fill_rect(x + 2, y + 2, ww - 4, WM_TITLE_H - 2, title_bg);
    gfx_hline(x + 2, y + WM_TITLE_H, ww - 4, RGB(52, 54, 62));

    /* Botones derecha: cerrar (×), minimizar (_) — orden tipo Windows */
    btn = 12;
    gap = 8;
    xr = x + ww - 10 - btn;
    ym = y + (WM_TITLE_H - btn) / 2;
    /* Cerrar */
    gfx_wu_line(xr + 2, ym + 2, xr + btn - 2, ym + btn - 2, RGB(235, 235, 240));
    gfx_wu_line(xr + btn - 2, ym + 2, xr + 2, ym + btn - 2, RGB(235, 235, 240));
    xr -= btn + gap;
    /* Minimizar */
    gfx_fill_rect(xr + 2, ym + btn / 2 - 1, btn - 4, 2, RGB(235, 235, 240));

    /* Título centrado (reserva ~64 px a la derecha para botones) */
    tw = gfx_text_width_hq(w->title ? w->title : "");
    tx = x + (ww - tw) / 2;
    min_tx = x + 16;
    max_tx = x + ww - 72 - tw;
    if (tx < min_tx) tx = min_tx;
    if (tx > max_tx) tx = max_tx;
    title_y_text = y + (WM_TITLE_H - FONT_HQ_CELL_H) / 2;
    if (title_y_text < y + 2) title_y_text = y + 2;
    gfx_draw_text_hq(tx, title_y_text, w->title ? w->title : "", title_fg);

    /* Área cliente: tono ligeramente más claro */
    gfx_fill_rect(x + WM_CLIENT_INSET, y + WM_TITLE_H + 2, ww - 2 * WM_CLIENT_INSET,
                  hh - WM_TITLE_H - 6, RGB(252, 253, 255));
}

static void fill_title_corner_tl(int x, int y, int r, unsigned int c) {
    int dy, dx;
    for (dy = 0; dy < r; dy++) {
        for (dx = 0; dx < r; dx++) {
            int dist2 = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
            if (dist2 <= (r - 1) * (r - 1))
                gfx_put_pixel(x + dx, y + dy, c);
        }
    }
}

static void fill_title_corner_tr(int x, int y, int r, unsigned int c) {
    int dy, dx;
    for (dy = 0; dy < r; dy++) {
        for (dx = 0; dx < r; dx++) {
            int dist2 = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
            if (dist2 <= (r - 1) * (r - 1))
                gfx_put_pixel(x - 1 - dx, y + dy, c);
        }
    }
}

static void draw_traffic_lights_left(int title_y_center, int left_x, int focused) {
    int cy = title_y_center;
    int u = layout_ui_scale;
    int step = (u * 12) / 100;
    int rdot = (u * 7) / 100;
    unsigned int dim = focused ? 255u : 120u;
    unsigned int c1 = RGB(200 * dim / 255, 60 * dim / 255, 60 * dim / 255);
    unsigned int c2 = RGB(220 * dim / 255, 180 * dim / 255, 50 * dim / 255);
    unsigned int c3 = RGB(80 * dim / 255, 200 * dim / 255, 90 * dim / 255);
    if (step < 18) step = 18;
    if (rdot < 5) rdot = 5;
    if (rdot > 10) rdot = 10;
    gfx_fill_circle(left_x + step, cy, rdot, c1);
    gfx_fill_circle(left_x + step * 2, cy, rdot, c2);
    gfx_fill_circle(left_x + step * 3, cy, rdot, c3);
    gfx_circle_outline_aa(left_x + step, cy, rdot, RGB(40, 20, 20));
    gfx_circle_outline_aa(left_x + step * 2, cy, rdot, RGB(60, 50, 20));
    gfx_circle_outline_aa(left_x + step * 3, cy, rdot, RGB(25, 55, 30));
}

void nwm_draw_window_frame(const NWM_Window* w, int title_h, int corner_r) {
    int r = corner_r;
    unsigned int frame_c, title_c, border_c;
    int tl, maxc, ttx, body_y;

    if (!w->visible) return;
    if (r > w->h / 2) r = w->h / 2;
    if (r > w->w / 2) r = w->w / 2;

    if (!nwm_skip_shadow)
        gfx_drop_shadow_soft(w->x, w->y, w->w, w->h, r, 10);

    frame_c = w->focused ? NWM_COL_FRAME_ACT : NWM_COL_FRAME_INACT;
    title_c = w->focused ? NWM_COL_TITLE_ACT : NWM_COL_TITLE_INACT;
    border_c = w->focused ? NWM_COL_BORDER_ACT : NWM_COL_BORDER_INACT;

    gfx_fill_rounded_rect(w->x, w->y, w->w, w->h, r, frame_c);

    fill_title_corner_tl(w->x, w->y, r, title_c);
    fill_title_corner_tr(w->x + w->w, w->y, r, title_c);
    gfx_fill_rect(w->x + r, w->y, w->w - 2 * r, title_h, title_c);
    gfx_hline(w->x + r, w->y + title_h, w->w - 2 * r, RGB(22, 24, 38));

    body_y = w->y + title_h;
    gfx_fill_rounded_rect(w->x + 2, body_y, w->w - 4, w->h - title_h - 2, r > 3 ? r - 2 : 1, NWM_COL_BODY);

    gfx_rounded_rect_stroke_aa(w->x, w->y, w->w, w->h, r, border_c);

    draw_traffic_lights_left(w->y + title_h / 2, w->x, w->focused);

    tl = slen(w->title);
    maxc = (w->w - (layout_ui_scale * 22) / 100) / FONT_HQ_CELL_W;
    if (maxc < 0) maxc = 0;
    if (tl > maxc) tl = maxc;
    ttx = w->x + (w->w - tl * FONT_HQ_CELL_W) / 2;
    {
        char tbuf[72];
        int ti = 0;
        while (ti < tl && ti < (int)sizeof(tbuf) - 1) {
            tbuf[ti] = w->title[ti];
            ti++;
        }
        tbuf[ti] = 0;
        gfx_draw_text_hq(ttx, w->y + (title_h - FONT_HQ_CELL_H) / 2, tbuf,
                         w->focused ? COL_WHITE : COL_DIM);
    }
}

void nwm_paint_painter_order(const NWM_Window* wins, int nwin, const int* zorder, int zcount,
    void (*draw_client)(void* user, int win_idx), void* user) {
    int zi, id;
    for (zi = 0; zi < zcount; zi++) {
        id = zorder[zi];
        if (id < 0 || id >= nwin) continue;
        if (!wins[id].visible) continue;
        nwm_draw_window_frame(&wins[id], layout_chrome_title_h, layout_chrome_corner_r);
        if (draw_client) draw_client(user, id);
    }
}

void nwm_dll_init_ring(NWM_Window* wins, int n) {
    int i;
    if (!wins || n <= 0) {
        nwm_z_bottom = 0;
        nwm_z_top = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        wins[i].z_prev = (i > 0) ? &wins[i - 1] : 0;
        wins[i].z_next = (i + 1 < n) ? &wins[i + 1] : 0;
    }
    nwm_z_bottom = &wins[0];
    nwm_z_top = &wins[n - 1];
}

void nwm_dll_raise(NWM_Window* w) {
    if (!w || !nwm_z_top || w == nwm_z_top)
        return;
    if (w->z_prev)
        w->z_prev->z_next = w->z_next;
    else
        nwm_z_bottom = w->z_next;
    if (w->z_next)
        w->z_next->z_prev = w->z_prev;
    else
        nwm_z_top = w->z_prev;
    w->z_prev = nwm_z_top;
    w->z_next = 0;
    nwm_z_top->z_next = w;
    nwm_z_top = w;
}

NWM_Window* nwm_z_stack_bottom(void) { return nwm_z_bottom; }

NWM_Window* nwm_z_stack_top(void) { return nwm_z_top; }

void nwm_window_mark_dirty(NWM_Window* w) {
    if (w)
        w->backing_dirty = 1;
}

void nwm_window_free_backing(NWM_Window* w) {
    if (!w)
        return;
    if (w->backing) {
        kfree(w->backing);
        w->backing = 0;
    }
    w->backing_alloc_w = w->backing_alloc_h = 0;
    w->backing_stride_u32 = 0;
}

void nwm_window_render_to_backing(NWM_Window* w, void (*draw_client)(NWM_Window*)) {
    NWM_Window local;
    uint64_t need;
    if (!w || w->w <= 0 || w->h <= 0)
        return;

    if (w->backing &&
        (w->backing_alloc_w != w->w || w->backing_alloc_h != w->h)) {
        kfree(w->backing);
        w->backing = 0;
    }
    need = (uint64_t)(unsigned)w->w * (uint64_t)(unsigned)w->h * 4ull;
    if (!w->backing) {
        w->backing = (uint32_t*)kmalloc(need);
        if (!w->backing)
            return;
        w->backing_alloc_w = w->w;
        w->backing_alloc_h = w->h;
    }
    w->backing_stride_u32 = (unsigned)w->w;

    local = *w;
    local.x = 0;
    local.y = 0;

    gfx_push_canvas(w->backing, w->w, w->h, (int)w->backing_stride_u32);
    gfx_clear(0u);
    nwm_skip_shadow = 1;
    nwm_draw_window_frame(&local, layout_chrome_title_h, layout_chrome_corner_r);
    nwm_skip_shadow = 0;
    if (draw_client)
        draw_client(&local);
    gfx_pop_canvas();
    w->backing_dirty = 0;
}

void nwm_window_ensure_backing(NWM_Window* w, void (*draw_client)(NWM_Window*)) {
    if (!w || !w->visible)
        return;
    if (!w->backing_dirty && w->backing && w->backing_alloc_w == w->w &&
        w->backing_alloc_h == w->h)
        return;
    nwm_window_render_to_backing(w, draw_client);
    compositor_damage_rect_pad(w->x, w->y, w->w, w->h, NEXUS_COMPOSITOR_SHADOW_PAD);
}
