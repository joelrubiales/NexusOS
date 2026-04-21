#include "mouse_gui.h"
#include "gfx.h"
#include "compositor.h"

int nwm_hit_titlebar(const NWM_Window* w, int px, int py, int title_h) {
    if (!w->visible) return 0;
    return px >= w->x && px < w->x + w->w && py >= w->y && py < w->y + title_h;
}

int nwm_hit_close_button(const NWM_Window* w, int px, int py, int title_h) {
    int pad = layout_ui_scale * 3 / 100;
    int rad = layout_ui_scale * 4 / 100;
    int cx, cy, dx, dy;
    if (pad < 8) pad = 8;
    if (rad < 8) rad = 8;
    if (rad > 14) rad = 14;
    cx = w->x + pad;
    cy = w->y + title_h / 2;
    dx = px - cx;
    dy = py - cy;
    return dx * dx + dy * dy <= rad * rad;
}

void nwm_raise_window(int* zorder, int zcount, int id) {
    int p = -1, i;
    for (i = 0; i < zcount; i++) {
        if (zorder[i] == id) {
            p = i;
            break;
        }
    }
    if (p < 0) return;
    for (i = p; i < zcount - 1; i++)
        zorder[i] = zorder[i + 1];
    zorder[zcount - 1] = id;
}

void nwm_apply_window_drag(NWM_Window* w, int mx, int my, int min_y, int max_y) {
    int sw = gfx_width();
    int ox = w->x, oy = w->y;
    w->x = mx - w->drag_ox;
    w->y = my - w->drag_oy;
    if (w->x < 0) w->x = 0;
    if (w->x + w->w > sw) w->x = sw - w->w;
    if (w->y < min_y) w->y = min_y;
    if (w->y + w->h > max_y) w->y = max_y - w->h;
    if (w->x != ox || w->y != oy)
        compositor_notify_window_moved(w, ox, oy);
}
