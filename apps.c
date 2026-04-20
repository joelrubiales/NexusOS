#include "apps.h"
#include "gfx.h"
#include "nexus.h"

void apps_draw_nexus_firefox(const NWM_Window* w) {
    int th = layout_chrome_title_h;
    int u = layout_ui_scale;
    int ax = w->x + w->w * 1 / 100;
    int ay = w->y + th + u * 2 / 1000;
    int aw = w->w - w->w * 2 / 100;
    int ah = w->h - th - u * 3 / 1000;
    int bar1, bar2, pad, ny, py, ph, hh, r_url;

    if (aw < 40 || ah < 50) return;

    bar1 = (ah * 18) / 100;
    if (bar1 < 28) bar1 = 28;
    if (bar1 > ah / 3) bar1 = ah / 3;
    bar2 = (ah * 14) / 100;
    if (bar2 < 24) bar2 = 24;
    if (bar2 > ah / 4) bar2 = ah / 4;
    pad = (u * 25) / 1000;
    if (pad < 6) pad = 6;
    r_url = (u * 12) / 1000;
    if (r_url < 6) r_url = 6;

    gfx_fill_rect(ax, ay, aw, bar1, RGB(218, 220, 228));
    gfx_hline(ax, ay, aw, RGB(250, 250, 252));
    gfx_hline(ax, ay + bar1 - 1, aw, RGB(180, 184, 198));

    gfx_fill_rounded_rect(ax + pad, ay + pad / 2, aw - 2 * pad, bar1 - pad, r_url, RGB(255, 255, 255));
    gfx_draw_rect(ax + pad, ay + pad / 2, aw - 2 * pad, bar1 - pad, RGB(200, 205, 220));
    gfx_draw_text_aa(ax + pad * 3, ay + (bar1 - 10) / 2, "Search or enter address", RGB(120, 125, 150), 1);

    ny = ay + bar1;
    gfx_fill_rect(ax, ny, aw, bar2, RGB(228, 230, 236));
    gfx_draw_text_aa(ax + pad, ny + (bar2 - 10) / 2, "<  >  r", RGB(85, 90, 115), 1);

    gfx_fill_rounded_rect(ax + aw * 18 / 100, ny + bar2 / 6, aw - aw * 28 / 100, bar2 * 2 / 3, r_url, RGB(255, 255, 255));
    gfx_draw_rect(ax + aw * 18 / 100, ny + bar2 / 6, aw - aw * 28 / 100, bar2 * 2 / 3, RGB(190, 195, 210));
    {
        int cr = (u * 25) / 1000;
        if (cr < 4) cr = 4;
        gfx_fill_circle(ax + aw * 22 / 100, ny + bar2 / 2, cr, RGB(60, 190, 90));
    }
    gfx_draw_text_aa(ax + aw * 26 / 100, ny + (bar2 - 10) / 2, "https://nexusos.local/welcome", RGB(30, 85, 135), 1);

    py = ny + bar2;
    ph = ah - bar1 - bar2;
    gfx_fill_rect(ax, py, aw, ph, RGB(236, 238, 244));

    hh = ph / 3;
    if (hh > (u * 20) / 100) hh = (u * 20) / 100;
    if (hh < (u * 8) / 100) hh = (u * 8) / 100;
    if (hh < 36) hh = 36;
    gfx_gradient_v(ax + aw * 5 / 100, py + u * 2 / 100, aw - aw * 10 / 100, hh, RGB(64, 78, 140), RGB(24, 110, 160));
    gfx_draw_text_aa(ax + aw / 2 - gfx_text_width_aa("Nexus Firefox", 2) / 2, py + u * 2 / 100 + hh / 2 - 12, "Nexus Firefox", COL_WHITE, 2);
    gfx_draw_text_aa(ax + aw / 2 - gfx_text_width_aa("Private. Fast. Bare-metal.", 1) / 2, py + u * 2 / 100 + hh / 2 + 8, "Private. Fast. Bare-metal.", RGB(200, 220, 255), 1);

    gfx_draw_text_aa(ax + aw * 6 / 100, py + hh + u * 3 / 100, "No network stack yet - visual shell only.", RGB(80, 85, 105), 1);
    gfx_hline(ax + aw * 5 / 100, py + ph - u * 15 / 1000, aw - aw * 10 / 100, RGB(210, 214, 224));
    gfx_draw_text_aa(ax + aw * 5 / 100, py + ph - u * 12 / 1000, "Nexus Firefox (mock UI)", RGB(130, 135, 155), 1);
}
