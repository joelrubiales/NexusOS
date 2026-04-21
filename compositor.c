#include "compositor.h"
#include "gfx.h"
#include "desktop.h"
#include "top_panel.h"
#include "memory.h"
#include "event.h"
#include <stddef.h>
#include <stdint.h>

#define COMP_MAX_DAMAGE 40
#define COMP_CURSOR_W   28
#define COMP_CURSOR_H   32

static int              comp_sw, comp_sh, comp_stride;
static uint32_t*        comp_wall;
static int              comp_inited;
static int              comp_full;

static struct {
    int x, y, w, h;
} comp_damage[COMP_MAX_DAMAGE];
static int comp_ndamage;

static int comp_cx, comp_cy, comp_cinit;

static int rect_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh,
                          int* ox, int* oy, int* ow, int* oh) {
    int rx0 = ax > bx ? ax : bx;
    int ry0 = ay > by ? ay : by;
    int rx1 = (ax + aw) < (bx + bw) ? (ax + aw) : (bx + bw);
    int ry1 = (ay + ah) < (by + bh) ? (ay + ah) : (by + bh);
    int rw = rx1 - rx0;
    int rh = ry1 - ry0;
    if (rw <= 0 || rh <= 0)
        return 0;
    *ox = rx0;
    *oy = ry0;
    *ow = rw;
    *oh = rh;
    return 1;
}

static void damage_push_clipped(int x, int y, int w, int h) {
    int i;
    if (w <= 0 || h <= 0 || !comp_inited)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > comp_sw) w = comp_sw - x;
    if (y + h > comp_sh) h = comp_sh - y;
    if (w <= 0 || h <= 0)
        return;
    if (comp_ndamage >= COMP_MAX_DAMAGE) {
        comp_full = 1;
        comp_ndamage = 0;
        return;
    }
    for (i = 0; i < comp_ndamage; i++) {
        if (comp_damage[i].x == x && comp_damage[i].y == y && comp_damage[i].w == w &&
            comp_damage[i].h == h)
            return;
    }
    comp_damage[comp_ndamage].x = x;
    comp_damage[comp_ndamage].y = y;
    comp_damage[comp_ndamage].w = w;
    comp_damage[comp_ndamage].h = h;
    comp_ndamage++;
}

void compositor_init(int screen_w, int screen_h, int stride_u32) {
    uint64_t nbytes;
    comp_sw = screen_w;
    comp_sh = screen_h;
    comp_stride = stride_u32;
    comp_ndamage = 0;
    comp_full = 1;
    comp_cinit = 0;
    if (comp_wall) {
        kfree(comp_wall);
        comp_wall = 0;
    }
    if (screen_w <= 0 || screen_h <= 0 || stride_u32 < screen_w)
        return;
    nbytes = (uint64_t)(unsigned)stride_u32 * (uint64_t)(unsigned)screen_h * 4ull;
    comp_wall = (uint32_t*)kmalloc(nbytes);
    comp_inited = comp_wall ? 1 : 0;
}

void compositor_shutdown(void) {
    if (comp_wall) {
        kfree(comp_wall);
        comp_wall = 0;
    }
    comp_inited = 0;
}

void compositor_bake_wallpaper_layer(void) {
    if (!comp_inited || !comp_wall)
        return;
    gfx_push_canvas(comp_wall, comp_sw, comp_sh, comp_stride);
    draw_desktop();
    gfx_pop_canvas();
}

void compositor_mark_full(void) {
    comp_full = 1;
    comp_ndamage = 0;
}

void compositor_damage_rect(int x, int y, int w, int h) {
    if (comp_full)
        return;
    damage_push_clipped(x, y, w, h);
}

void compositor_damage_rect_pad(int x, int y, int w, int h, int pad) {
    compositor_damage_rect(x - pad, y - pad, w + 2 * pad, h + 2 * pad);
}

void compositor_notify_window_moved(const NWM_Window* w, int old_x, int old_y) {
    if (!w)
        return;
    compositor_damage_rect_pad(old_x, old_y, w->w, w->h, NEXUS_COMPOSITOR_SHADOW_PAD);
    compositor_damage_rect_pad(w->x, w->y, w->w, w->h, NEXUS_COMPOSITOR_SHADOW_PAD);
}

void compositor_cursor_moved(int mx, int my) {
    if (!comp_inited)
        return;
    if (!comp_cinit) {
        comp_cx = mx;
        comp_cy = my;
        comp_cinit = 1;
        return;
    }
    if (comp_full)
        return;
    compositor_damage_rect(comp_cx - 2, comp_cy - 2, COMP_CURSOR_W, COMP_CURSOR_H);
    compositor_damage_rect(mx - 2, my - 2, COMP_CURSOR_W, COMP_CURSOR_H);
    comp_cx = mx;
    comp_cy = my;
}

static void blit_wall_region(uint32_t* dst, int rx, int ry, int rw, int rh) {
    int j;
    if (!dst || !comp_wall || rw <= 0 || rh <= 0)
        return;
    for (j = 0; j < rh; j++) {
        uint32_t* drow = dst + (size_t)(unsigned)(ry + j) * (size_t)(unsigned)comp_stride +
                         (size_t)(unsigned)rx;
        const uint32_t* srow =
            comp_wall + (size_t)(unsigned)(ry + j) * (size_t)(unsigned)comp_stride +
            (size_t)(unsigned)rx;
        memcpy_fast(drow, srow, (size_t)(unsigned)rw * 4u);
    }
}

static void composite_windows_region(uint32_t* dst, int rx, int ry, int rw, int rh) {
    NWM_Window* u;
    int ix, iy, iw, ih, j;
    for (u = nwm_z_stack_bottom(); u; u = u->z_next) {
        if (!u->visible || !u->backing)
            continue;
        if (!rect_intersect(rx, ry, rw, rh, u->x, u->y, u->w, u->h, &ix, &iy, &iw, &ih))
            continue;
        for (j = 0; j < ih; j++) {
            uint32_t* drow = dst + (size_t)(unsigned)(iy + j) * (size_t)(unsigned)comp_stride +
                             (size_t)(unsigned)ix;
            const uint32_t* srow = u->backing +
                                   (size_t)(unsigned)(iy - u->y + j) *
                                       (size_t)(unsigned)u->backing_stride_u32 +
                                   (size_t)(unsigned)(ix - u->x);
            memcpy_fast(drow, srow, (size_t)(unsigned)iw * 4u);
        }
    }
}

static int region_hits_top_band(int ry, int rh, int top_h) {
    return ry < top_h && ry + rh > 0;
}

static int region_hits_installer(int rx, int ry, int rw, int rh) {
    int ox, oy, ow, oh;
    if (!installer_win.is_visible)
        return 0;
    return rect_intersect(rx, ry, rw, rh, installer_win.x, installer_win.y, installer_win.width,
                          installer_win.height, &ox, &oy, &ow, &oh);
}

static void render_damage_rect(int rx, int ry, int rw, int rh, int mx, int my, int* dock_hover) {
    uint32_t* dst = gfx_backbuffer_u32();
    int top_h = layout_top_h;
    int dx, dy, dw, dh;

    if (!dst)
        return;

    blit_wall_region(dst, rx, ry, rw, rh);

    if (region_hits_top_band(ry, rh, top_h)) {
        desktop_draw_top_bar();
        top_panel_draw(comp_sw, comp_sh);
    }

    composite_windows_region(dst, rx, ry, rw, rh);

    if (region_hits_installer(rx, ry, rw, rh))
        desktop_paint_wm_windows();

    if (desktop_get_dock_geometry(comp_sw, comp_sh, &dx, &dy, &dw, &dh),
        rect_intersect(rx, ry, rw, rh, dx, dy, dw, dh, &rx, &ry, &rw, &rh)) {
        desktop_draw_dock(comp_sw, comp_sh, mx, my, dock_hover);
    }
}

void compositor_render(int sw, int sh, int mx, int my, int* dock_hover_out) {
    int i;
    uint32_t* dst = gfx_backbuffer_u32();

    if (!comp_inited || !dst || !comp_wall)
        return;
    gfx_layout_refresh();

    if (comp_full) {
        blit_wall_region(dst, 0, 0, comp_sw, comp_sh);
        desktop_draw_top_bar();
        top_panel_draw(sw, sh);
        composite_windows_region(dst, 0, 0, comp_sw, comp_sh);
        desktop_paint_wm_windows();
        desktop_draw_dock(sw, sh, mx, my, dock_hover_out);
        gfx_mark_present_full();
        comp_full = 0;
        comp_ndamage = 0;
        return;
    }

    {
        int n = comp_ndamage;
        if (n <= 0) {
            gfx_mark_present_noop();
            comp_ndamage = 0;
            return;
        }
        for (i = 0; i < n; i++)
            gfx_mark_present_rect(comp_damage[i].x, comp_damage[i].y, comp_damage[i].w, comp_damage[i].h);
        for (i = 0; i < n; i++) {
            int rx = comp_damage[i].x;
            int ry = comp_damage[i].y;
            int rw = comp_damage[i].w;
            int rh = comp_damage[i].h;
            render_damage_rect(rx, ry, rw, rh, mx, my, dock_hover_out);
        }
        comp_ndamage = 0;
    }
}

void compositor_event_bridge(const void* evp) {
    const Event* e = (const Event*)evp;
    if (!e)
        return;
    (void)e;
    /* El arrastre marca daño en nwm_apply_window_drag → compositor_notify_window_moved. */
}
