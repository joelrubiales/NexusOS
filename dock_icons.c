#include "dock_icons.h"
#include "gfx.h"
#include "icons.h"
#include "vfs.h"

static void icon_system_detailed(int x, int y, int s) {
    int ox = x + s * 18 / 100, oy = y + s * 15 / 100, bw = s * 64 / 100, bh = s * 55 / 100;
    gfx_fill_rounded_rect(ox, oy, bw, bh, 4, RGB(48, 52, 62));
    gfx_draw_rect(ox, oy, bw, bh, RGB(90, 95, 115));
    gfx_fill_rect(ox + bw * 15 / 100, oy + bh * 12 / 100, bw * 70 / 100, bh * 12 / 100, RGB(55, 200, 95));
    gfx_fill_rect(ox + bw * 15 / 100, oy + bh * 38 / 100, bw * 45 / 100, bh * 10 / 100, RGB(70, 150, 240));
    gfx_fill_rect(ox + bw * 15 / 100, oy + bh * 58 / 100, bw * 30 / 100, bh * 8 / 100, RGB(230, 190, 60));
}

static void icon_info_detailed(int x, int y, int s) {
    int cx = x + s / 2, cy = y + s / 2, r = s * 38 / 100;
    gfx_fill_circle(cx, cy, r + 1, RGB(120, 70, 195));
    gfx_fill_circle(cx, cy, r - 2, RGB(150, 95, 220));
    gfx_draw_text_hq(cx - 8, cy - 10, "i", RGB(255, 255, 255));
}

static void icon_power_detailed(int x, int y, int s) {
    int cx = x + s / 2, cy = y + s / 2 + s / 12, r = s * 32 / 100;
    gfx_draw_rect(cx - r - 1, cy - r - 1, r * 2 + 2, r * 2 + 2, RGB(220, 70, 70));
    gfx_fill_circle(cx, cy, r, RGB(200, 55, 55));
    gfx_fill_circle(cx, cy, r - 3, RGB(160, 40, 40));
    gfx_vline(cx - 1, y + s * 12 / 100, s * 28 / 100, RGB(255, 255, 255));
    gfx_vline(cx, y + s * 12 / 100, s * 28 / 100, RGB(255, 255, 255));
}

/*
 * Intenta usar el icono del VFS (initrd:/icons/<id>.bmp) a tamaño arbitrario
 * escalando con gfx_blit_scaled. Si no existe o el VFS no está listo, cae al
 * renderer geométrico nativo.
 */
void dock_icon_draw(int id, int x, int y, int size) {
    int ox, oy;
    if (size < 16) size = 16;

    /* Intentar icono VFS (carga en cache la primera vez, después zero-cost). */
    if (vfs_ready()) {
        int iw = 0, ih = 0;
        const uint32_t* bmp = vfs_get_icon(id, &iw, &ih);
        if (bmp && iw > 0 && ih > 0) {
            gfx_blit_scaled(x, y, size, size, bmp, iw, ih);
            return;
        }
    }

    /* Fallback: renderers geométricos nativos (siempre disponibles). */
    ox = (size - ICON_RGBA_W) / 2;
    oy = (size - ICON_RGBA_H) / 2;
    if (id == 0) {
        gfx_draw_image_rgba(x + ox, y + oy, ICON_RGBA_W, ICON_RGBA_H, icon_rgba_folder);
        return;
    }
    if (id == 1) {
        gfx_draw_image_rgba(x + ox, y + oy, ICON_RGBA_W, ICON_RGBA_H, icon_rgba_terminal);
        return;
    }
    if (id == 2) {
        gfx_draw_image_rgba(x + ox, y + oy, ICON_RGBA_W, ICON_RGBA_H, icon_rgba_globe);
        return;
    }
    if (id == 3) {
        icon_system_detailed(x, y, size);
        return;
    }
    if (id == 4) {
        icon_info_detailed(x, y, size);
        return;
    }
    icon_power_detailed(x, y, size);
}
