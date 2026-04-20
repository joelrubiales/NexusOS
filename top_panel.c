#include "top_panel.h"
#include "desktop.h"
#include "gfx.h"
#include "nexus.h"

extern volatile uint64_t ticks;

/* Logo “N” proporcional a layout_ui_scale (sin caracteres bloque). */
static void draw_panel_logo_n(int bx, int by, int lw, int lh) {
    int i, r = lh / 4;
    if (r < 3) r = 3;
    if (r > lh / 2) r = lh / 2;
    gfx_fill_rounded_rect(bx, by, lw, lh, r, RGB(42, 88, 210));
    gfx_fill_rounded_rect(bx + lw / 11, by + lh / 9, lw * 9 / 11, lh * 7 / 9, r - 1, RGB(58, 115, 240));
    gfx_fill_rect(bx + lw * 5 / 22, by + lh * 4 / 18, lw * 4 / 22, lh * 11 / 18, RGB(255, 255, 255));
    gfx_fill_rect(bx + lw * 15 / 22, by + lh * 4 / 18, lw * 4 / 22, lh * 11 / 18, RGB(255, 255, 255));
    for (i = 0; i < lh * 10 / 18; i++)
        gfx_fill_rect(bx + lw * 8 / 22 + i * lw / 22 / 2, by + lh * 4 / 18 + i, lw / 10, lh / 16, RGB(255, 255, 255));
}

void top_panel_draw(int sw, int sh) {
    int rx, cpu_p, mem_p, tw, px;
    int th = DESK_TOP_BAR_HEIGHT;
    int u = layout_ui_scale;
    int pad, logo_w, logo_h, tx, ty;
    int right_blk, box_w, box_h, box_y, inner_pad;
    char hora[12];

    (void)sh;
    /* Barra base y vidrio: desktop_draw_top_bar() (gui). Aquí solo cromo y métricas. */

    pad = u * 2 / 100;
    if (pad < 4) pad = 4;
    logo_w = (u * 7) / 100;
    logo_h = (u * 6) / 100;
    if (logo_w < 18) logo_w = 18;
    if (logo_h < 15) logo_h = 15;
    if (logo_w > th + th / 2) logo_w = th + th / 2;
    if (logo_h > th - 6) logo_h = th - 6;

    draw_panel_logo_n(pad, (th - logo_h) / 2, logo_w, logo_h);

    tx = pad + logo_w + pad;
    ty = (th - 10) / 2;
    if (ty < 2) ty = 2;
    gfx_draw_text_aa(tx, ty, "NexusOS", RGB(235, 238, 250), 1);

    obtener_hora(hora);
    tw = gfx_text_width_aa(hora, 1);
    px = sw / 2 - tw / 2;
    {
        int min_px = sw / 10 + tx + tw / 4;
        int max_px = sw - sw / 3 - tw;
        if (max_px < min_px) max_px = min_px;
        if (px < min_px) px = min_px;
        if (px > max_px) px = max_px;
    }
    gfx_draw_text_aa(px, ty, hora, RGB(248, 250, 255), 1);

    cpu_p = (int)((ticks / 19) % 100);
    mem_p = 12;
    right_blk = (u * 33) / 100;
    if (right_blk < 170) right_blk = 170;
    if (right_blk > 240) right_blk = 240;
    rx = sw - right_blk;
    if (rx < px + tw + 12) rx = px + tw + 12;
    if (rx + right_blk > sw) rx = sw - right_blk;
    if (rx < 8) rx = 8;

    box_h = th - u * 12 / 100;
    if (box_h < 14) box_h = 14;
    if (box_h > th - 6) box_h = th - 6;
    box_y = (th - box_h) / 2;
    box_w = (right_blk * 48) / 100;
    if (box_w < 88) box_w = 88;
    inner_pad = u * 15 / 1000;
    if (inner_pad < 4) inner_pad = 4;

    gfx_fill_rounded_rect(rx, box_y, box_w, box_h, box_h / 4, RGB(32, 35, 52));
    gfx_draw_text_aa(rx + inner_pad, box_y + (box_h - 8) / 2, "CPU", RGB(190, 200, 220), 1);
    gfx_fill_rounded_rect(rx + box_w * 38 / 100, box_y + box_h / 4, box_w * 54 / 100, box_h / 2, 3, RGB(18, 20, 32));
    if (cpu_p > 0) {
        int bw = cpu_p * (box_w * 52 / 100) / 100;
        if (bw < 2) bw = 2;
        gfx_fill_rounded_rect(rx + box_w * 39 / 100, box_y + box_h / 4 + 1, bw, box_h / 2 - 2, 2, RGB(55, 210, 130));
    }

    rx += box_w + u * 3 / 100;
    if (rx + box_w > sw - 4) rx = sw - box_w - 4;
    gfx_fill_rounded_rect(rx, box_y, box_w, box_h, box_h / 4, RGB(32, 35, 52));
    gfx_draw_text_aa(rx + inner_pad, box_y + (box_h - 8) / 2, "MEM", RGB(190, 200, 220), 1);
    gfx_fill_rounded_rect(rx + box_w * 42 / 100, box_y + box_h / 4, box_w * 48 / 100, box_h / 2, 3, RGB(18, 20, 32));
    gfx_fill_rounded_rect(rx + box_w * 43 / 100, box_y + box_h / 4 + 1, mem_p * (box_w * 46 / 100) / 100, box_h / 2 - 2, 2, RGB(80, 160, 240));
}
