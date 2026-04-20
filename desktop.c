#include "desktop.h"
#include "gfx.h"
#include "window.h"
#include "installer_ui.h"
#include "dock_icons.h"
#include "font_data.h"
#include "vfs.h"

#define DOCK_BG 0x00383C52u

/* ── Ventana global del instalador (oculta hasta que is_visible = 1) ── */
Window installer_win;

#define DESKTOP_WM_MAX 8
static Window* wm_registry[DESKTOP_WM_MAX];
static int wm_registry_n;

void desktop_wm_register(Window* w) {
    int i;
    if (!w) return;
    for (i = 0; i < wm_registry_n; i++)
        if (wm_registry[i] == w) return;
    if (wm_registry_n >= DESKTOP_WM_MAX) return;
    wm_registry[wm_registry_n++] = w;
}

void desktop_wm_init(void) {
    static int once;
    if (once) return;
    once = 1;
    installer_win.x = 0;
    installer_win.y = 0;
    installer_win.width = 560;
    installer_win.height = 420;
    installer_win.title = "Instalador de NexusOS";
    /* Visible por defecto para probar el asistente (Live CD). */
    installer_win.is_visible = 1;
    installer_win.is_active = 1;
    installer_win.z_index = 100;
    desktop_wm_register(&installer_win);
}

static void desktop_wm_layout(void) {
    int sw = screen_width;
    int avail_h = layout_dock_y - DESK_TOP_BAR_HEIGHT;
    int w = installer_win.width;
    int h = installer_win.height;
    if (avail_h < 120) return;
    if (w > sw - 32) w = sw - 32;
    if (h > avail_h - 16) h = avail_h - 16;
    if (h < 200) h = 200;
    if (w < 200) w = 200;
    installer_win.width = w;
    installer_win.height = h;
    installer_win.x = (sw - w) / 2;
    installer_win.y = DESK_TOP_BAR_HEIGHT + (avail_h - h) / 2;
    if (installer_win.y < DESK_TOP_BAR_HEIGHT + 2) installer_win.y = DESK_TOP_BAR_HEIGHT + 2;
}

void desktop_paint_wm_windows(void) {
    Window* sorted[DESKTOP_WM_MAX];
    int i, j, n;

    if (wm_registry_n <= 0) return;

    desktop_wm_layout();

    n = wm_registry_n;
    if (n > DESKTOP_WM_MAX) n = DESKTOP_WM_MAX;
    for (i = 0; i < n; i++) sorted[i] = wm_registry[i];

    for (i = 1; i < n; i++) {
        Window* key = sorted[i];
        j = i - 1;
        while (j >= 0 && sorted[j]->z_index > key->z_index) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    for (i = 0; i < n; i++) {
        if (!sorted[i]->is_visible)
            continue;
        draw_window(sorted[i]);
        if (sorted[i] == &installer_win)
            draw_installer_content(&installer_win);
    }
}

/* Geometría dock: ancho DESK_DOCK_WIDTH_PCENT %, centrado X, flotante abajo. */
static void desk_dock_compute(int sw, int sh, int* dx, int* dy, int* dw, int* dh) {
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    *dw = (sw * DESK_DOCK_WIDTH_PCENT) / 100;
    if (*dw < 1) *dw = 1;
    *dh = (sh * 6) / 100;
    if (*dh < DESK_DOCK_MIN_HEIGHT) *dh = DESK_DOCK_MIN_HEIGHT;
    if (*dh > sh / 4) *dh = sh / 4;
    *dx = (sw - *dw) / 2;
    *dy = sh - *dh - DESK_DOCK_BOTTOM_PAD;
    if (*dy < DESK_TOP_BAR_HEIGHT) *dy = DESK_TOP_BAR_HEIGHT;
}

/*
 * Live CD: tarjeta central “Instalar NexusOS” con icono geométrico (medio + flecha).
 * Debe dibujarse después del fondo y antes del dock (el bucle en gui.c ya respeta el orden).
 */
static void desktop_draw_livecd_install_tile(void) {
    int sw = screen_width;
    int top = DESK_TOP_BAR_HEIGHT;
    int bot = layout_dock_y;
    int avail = bot - top;
    int u = layout_ui_scale;
    int card_w, card_h, cx, cy, r;
    int ix, iy, iw, ih;
    int tx0, ty0;
    const char* subtitle = "NexusOS Live";
    const char* action = "Instalar NexusOS";

    if (avail < 100 || sw < 240)
        return;

    card_w = (u * 38) / 100;
    if (card_w < 280) card_w = 280;
    if (card_w > 500) card_w = 500;
    if (card_w > sw - 40) card_w = sw - 40;

    card_h = (u * 15) / 100;
    if (card_h < 118) card_h = 118;
    if (card_h > 172) card_h = 172;
    if (card_h > avail - 16) card_h = avail - 16;

    cx = (sw - card_w) / 2;
    cy = top + (avail - card_h) / 2;
    r = 22;
    if (r > card_h / 3) r = card_h / 3;

    /* Sombra suave */
    gfx_fill_rounded_rect(cx + 5, cy + 6, card_w, card_h, r, RGB(0, 0, 0));
    gfx_blend_rect(cx + 5, cy + 6, card_w, card_h, RGB(0, 0, 0), 90);

    /* Cuerpo vidrio */
    gfx_fill_rounded_rect(cx, cy, card_w, card_h, r, RGB(26, 30, 52));
    gfx_blend_rect(cx + 2, cy + 2, card_w - 4, (card_h * 38) / 100, RGB(70, 110, 230), 40);
    gfx_blend_rect(cx + 2, cy + card_h / 2, card_w - 4, card_h / 2 - 4, RGB(40, 20, 70), 25);
    gfx_rounded_rect_stroke_aa(cx, cy, card_w, card_h, r, RGB(140, 155, 220));

    /* Icono: soporte + núcleo + flecha (geometría simple, sin bitmap externo) */
    ix = cx + 22;
    iy = cy + (card_h - 72) / 2;
    iw = 72;
    ih = 72;
    gfx_fill_rounded_rect(ix, iy, iw, ih, 16, RGB(52, 115, 245));
    gfx_fill_rounded_rect(ix + 9, iy + 9, iw - 18, ih - 18, 12, RGB(16, 28, 62));
    gfx_fill_circle(ix + iw / 2, iy + ih / 2 - 4, 11, RGB(10, 16, 36));
    gfx_circle_outline_aa(ix + iw / 2, iy + ih / 2 - 4, 11, RGB(120, 195, 255));
    /* Flecha hacia abajo (instalar) */
    gfx_fill_rect(ix + iw / 2 - 7, iy + ih - 28, 14, 14, RGB(230, 240, 255));
    gfx_fill_rect(ix + iw / 2 - 15, iy + ih - 18, 30, 8, RGB(230, 240, 255));

    tx0 = ix + iw + 18;
    ty0 = cy + card_h / 2 - FONT_HQ_CELL_H - 6;
    if (tx0 + gfx_text_width_hq(action) < cx + card_w - 12) {
        gfx_draw_text_hq(tx0, ty0, subtitle, RGB(155, 170, 205));
        gfx_draw_text_hq(tx0, ty0 + FONT_HQ_CELL_H + 8, action, RGB(250, 252, 255));
    } else {
        /* Modo estrecho: texto centrado bajo el icono */
        {
            int tws = gfx_text_width_hq(subtitle);
            int twa = gfx_text_width_hq(action);
            int mx = cx + card_w / 2;
            gfx_draw_text_hq(mx - tws / 2, cy + 16, subtitle, RGB(155, 170, 205));
            gfx_draw_text_hq(mx - twa / 2, cy + 16 + FONT_HQ_CELL_H + 6, action, RGB(250, 252, 255));
        }
    }
}

void desktop_paint_wallpaper_layer(void) {
    int sw = screen_width;
    int sh = screen_height;
    if (sw < 1 || sh < 1) return;

    /* Intentar wallpaper desde el VFS (initrd:/background.bmp). */
    if (vfs_ready()) {
        int bw = 0, bh = 0;
        const uint32_t* bmp = vfs_get_wallpaper(&bw, &bh);
        if (bmp && bw > 0 && bh > 0) {
            gfx_blit_scaled(0, 0, sw, sh, bmp, bw, bh);
            /* Velos suaves para integrar el dock y la barra de título. */
            gfx_blend_rect(0, 0, sw, sh / 14, RGB(0, 8, 30), 55);
            gfx_blend_rect(0, sh - sh / 10, sw, sh / 10, RGB(10, 0, 20), 50);
            return;
        }
    }

    /* Fallback: degradado vectorial (sin initrd o BMP no encontrado). */
    gfx_gradient_v(0, 0, sw, sh, RGB(8, 18, 52), RGB(72, 34, 96));
    gfx_blend_rect(0, 0, sw, sh / 12, RGB(0, 10, 40), 45);
    gfx_blend_rect(0, sh - sh / 10, sw, sh / 10, RGB(20, 0, 30), 40);
}

void desktop_paint_desktop_icons(void) {
    desktop_draw_livecd_install_tile();
}

void draw_desktop(void) {
    desktop_paint_wallpaper_layer();
    desktop_paint_desktop_icons();
}

void desktop_draw_top_bar(void) {
    int sw = screen_width;
    int th = DESK_TOP_BAR_HEIGHT;
    if (sw < 1) return;
    /* Panel 30px: capas oscuras translúcidas tipo “mica”. */
    gfx_blend_rect(0, 0, sw, th, RGB(12, 14, 22), 220);
    gfx_blend_rect(0, 0, sw, th / 2 + 6, RGB(55, 60, 85), 48);
    gfx_hline(0, th - 1, sw, RGB(95, 102, 130));
}

void desktop_draw_wallpaper(int x, int y, int w, int h) {
    if (w < 1 || h < 1) return;
    gfx_gradient_v(x, y, w, h, RGB(8, 20, 58), RGB(68, 32, 92));
}

void desktop_get_dock_geometry(int sw, int sh, int* dx, int* dy, int* dw, int* dh) {
    desk_dock_compute(sw, sh, dx, dy, dw, dh);
}

void desktop_get_launcher_rect(int sw, int sh, int* lx, int* ly, int* lw, int* lh) {
    int dx, dy, dw, dh;
    int pad, tw, btn_h;
    (void)sw;
    (void)sh;
    desk_dock_compute(screen_width, screen_height, &dx, &dy, &dw, &dh);
    pad = layout_ui_scale * 25 / 1000;
    if (pad < 8) pad = 8;
    if (pad > 24) pad = 24;
    *lx = dx + pad;
    tw = gfx_text_width_hq("Apps");
    *lw = tw + pad * 2;
    if (*lw < layout_ui_scale / 10) *lw = layout_ui_scale / 10;
    btn_h = dh - layout_ui_scale * 6 / 100;
    if (btn_h < 22) btn_h = 22;
    if (btn_h > dh - 8) btn_h = dh - 8;
    *lh = btn_h;
    *ly = dy + (dh - *lh) / 2;
}

int desktop_dock_hit(int sw, int sh, int px, int py) {
    int dx, dy, dw, dh, lx, ly, lw, lh, slot0, k, ix, iy;
    int icon_s = layout_icon_size;
    int slot_sp = layout_slot_sp;
    int r = layout_dock_r;

    (void)sw;
    (void)sh;
    desk_dock_compute(screen_width, screen_height, &dx, &dy, &dw, &dh);
    if (!nwm_point_in_rounded_rect(px, py, dx, dy, dw, dh, r))
        return -1;

    desktop_get_launcher_rect(screen_width, screen_height, &lx, &ly, &lw, &lh);
    if (px >= lx - 4 && px < lx + lw + 4 && py >= ly - 4 && py < ly + lh + 4)
        return -2;

    slot0 = lx + lw + layout_ui_scale * 4 / 100;
    if (slot0 < lx + lw + 12) slot0 = lx + lw + 12;
    iy = dy + (dh - icon_s) / 2;
    for (k = 0; k < DESK_DOCK_ITEMS; k++) {
        ix = slot0 + k * slot_sp;
        if (px >= ix - 4 && px < ix + icon_s + 8 && py >= iy - 4 && py < iy + icon_s + 8)
            return k;
    }
    return -1;
}

static void menu_geometry(int anchor_x, int anchor_y, int* mx, int* my, int* mw, int* mh) {
    int u = layout_ui_scale;
    *mw = (u * 22) / 100;
    if (*mw < 180) *mw = 180;
    if (*mw > screen_width - 24) *mw = screen_width - 24;
    *mh = 6 * (FONT_HQ_CELL_H + 8) + 30;
    if (*mh > screen_height / 2) *mh = screen_height / 2;
    *mx = anchor_x - *mw / 2;
    *my = anchor_y - *mh - layout_ui_scale * 15 / 1000;
    if (*mx < 8) *mx = 8;
    if (*my < DESK_TOP_BAR_HEIGHT + 4) *my = DESK_TOP_BAR_HEIGHT + 4;
}

int desktop_menu_contains(int anchor_x, int anchor_y, int px, int py) {
    int mx, my, mw, mh;
    int rr = layout_ui_scale * 14 / 1000;
    if (rr < 10) rr = 10;
    menu_geometry(anchor_x, anchor_y, &mx, &my, &mw, &mh);
    return nwm_point_in_rounded_rect(px, py, mx, my, mw, mh, rr);
}

int desktop_menu_hit(int anchor_x, int anchor_y, int px, int py) {
    int mx, my, mw, mh, row, row_h;
    int rr = layout_ui_scale * 14 / 1000;
    if (rr < 10) rr = 10;

    menu_geometry(anchor_x, anchor_y, &mx, &my, &mw, &mh);
    if (!nwm_point_in_rounded_rect(px, py, mx, my, mw, mh, rr))
        return -1;
    if (py < my + 28)
        return -1;
    row_h = FONT_HQ_CELL_H + 6;
    row = (py - my - 28) / row_h;
    if (row < 0 || row >= 6)
        return -1;
    return row;
}

void desktop_draw_dock(int sw, int sh, int mx, int my, int* out_hover) {
    int dx, dy, dw, dh, r;
    int slot0, k, ix, iy, lx, ly, lw, lh, tw, label_y;
    int icon_s = layout_icon_size;
    int slot_sp = layout_slot_sp;
    int hi_r = layout_ui_scale * 14 / 1000;
    if (hi_r < 10) hi_r = 10;

    (void)sw;
    (void)sh;
    desk_dock_compute(screen_width, screen_height, &dx, &dy, &dw, &dh);
    r = layout_dock_r;
    *out_hover = desktop_dock_hit(screen_width, screen_height, mx, my);

    gfx_fill_rounded_rect(dx, dy, dw, dh, r, DOCK_BG);
    gfx_blend_rect(dx + 2, dy + 2, dw - 4, (dh * 35) / 100, RGB(70, 75, 95), 28);
    gfx_rounded_rect_stroke_aa(dx, dy, dw, dh, r, RGB(100, 108, 132));

    desktop_get_launcher_rect(screen_width, screen_height, &lx, &ly, &lw, &lh);
    gfx_fill_rounded_rect(lx - 3, ly - 3, lw + 6, lh + 6, hi_r, RGB(48, 52, 72));
    tw = gfx_text_width_hq("Apps");
    label_y = ly + (lh - FONT_HQ_CELL_H) / 2;
    gfx_draw_text_hq(lx + (lw - tw) / 2, label_y, "Apps", RGB(245, 246, 252));

    slot0 = lx + lw + layout_ui_scale * 4 / 100;
    if (slot0 < lx + lw + 12) slot0 = lx + lw + 12;
    iy = dy + (dh - icon_s) / 2;
    for (k = 0; k < DESK_DOCK_ITEMS; k++) {
        ix = slot0 + k * slot_sp;
        if (*out_hover == k)
            gfx_fill_rounded_rect(ix - 5, iy - 5, icon_s + 10, icon_s + 10, hi_r, RGB(255, 255, 255));
        if (*out_hover == k)
            gfx_blend_rect(ix - 4, iy - 4, icon_s + 8, icon_s + 8, RGB(55, 58, 78), 90);
        dock_icon_draw(k, ix, iy, icon_s);
    }
}

void desktop_draw_app_menu(int open, int sel, int anchor_x, int anchor_y) {
    int mw, mh = 6 * (FONT_HQ_CELL_H + 8) + 30, mx, my, i, iy;
    int u = layout_ui_scale;
    int rr = u * 14 / 1000;
    if (rr < 10) rr = 10;
    const char* items[] = {
        "Terminal", "Files", "Nexus Firefox", "System", "About", "Quit"
    };
    if (!open) return;
    mw = (u * 22) / 100;
    if (mw < 180) mw = 180;
    if (mw > screen_width - 24) mw = screen_width - 24;
    mx = anchor_x - mw / 2;
    my = anchor_y - mh - u * 15 / 1000;
    if (mx < 8) mx = 8;
    if (my < DESK_TOP_BAR_HEIGHT + 4) my = DESK_TOP_BAR_HEIGHT + 4;

    for (i = 0; i < mw; i++) {
        int j;
        for (j = 0; j < mh; j++) {
            int ppx = mx + i, ppy = my + j;
            if (!nwm_point_in_rounded_rect(ppx, ppy, mx, my, mw, mh, rr)) continue;
            gfx_blend_pixel(ppx, ppy, RGB(26, 28, 44), 230);
        }
    }
    gfx_draw_rect(mx, my, mw, mh, RGB(100, 105, 135));
    gfx_draw_text_aa(mx + 16, my + 10, "Applications", COL_ACCENT, 2);

    for (i = 0; i < 6; i++) {
        int row_h = FONT_HQ_CELL_H + 6;
        iy = my + 28 + i * row_h;
        if (i == sel) {
            gfx_fill_rounded_rect(mx + 8, iy, mw - 16, row_h - 2, 8, RGB(55, 75, 150));
            gfx_draw_text_aa(mx + 20, iy + 4, items[i], COL_WHITE, 2);
        } else
            gfx_draw_text_aa(mx + 20, iy + 4, items[i], COL_LGRAY, 2);
        if (i == 5)
            gfx_hline(mx + 12, iy - 2, mw - 24, RGB(65, 70, 90));
    }
}
