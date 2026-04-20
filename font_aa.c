/*
 * font_aa.c — Motor de tipografía antialiased para NexusOS.
 *
 * Cada byte de los datos del glifo representa la opacidad del píxel
 * (0 = totalmente transparente, 255 = totalmente opaco).  El valor se
 * usa como canal alpha en la codificación ARGB 0xAARRGGBB y se pasa a
 * gfx_put_pixel(), que lo mezcla automáticamente con el fondo.
 *
 * Al escalar (scale > 1), cada píxel del glifo se repite en un bloque
 * scale×scale en pantalla (Nearest Neighbor).  La suavidad visual viene
 * de los valores de opacidad del propio glifo, no del escalado.
 */

#include "font_aa.h"
#include "gfx.h"
#include "font_aa_data.h"
#include <stdint.h>

/* ── helpers locales ─────────────────────────────────────────────────── */

static inline int aa_clamp_scale(int s)
{
    if (s < 1) return 1;
    if (s > 4) return 4;
    return s;
}

/* ── gfx_aa_char ─────────────────────────────────────────────────────── */

void gfx_aa_char(int x, int y, unsigned char ch, uint32_t fg_rgb, int scale)
{
    const uint8_t (*glyph)[FONT_AA_GLYPH_W];
    int idx, gx, gy, sx, sy;
    uint32_t fg_base;

    if (ch < FONT_AA_FIRST ||
        ch >= (unsigned char)(FONT_AA_FIRST + FONT_AA_NGLYPHS))
        return;

    scale   = aa_clamp_scale(scale);
    fg_base = fg_rgb & 0x00FFFFFFu;

    idx   = (int)(unsigned char)ch - (int)FONT_AA_FIRST;
    glyph = font_aa_glyphs[idx];

    for (gy = 0; gy < FONT_AA_GLYPH_H; gy++) {
        int py = y + gy * scale;
        for (gx = 0; gx < FONT_AA_GLYPH_W; gx++) {
            uint8_t  cov = glyph[gy][gx];
            uint32_t argb;
            int      px;

            if (cov == 0) continue;  /* completamente transparente */

            px = x + gx * scale;

            /*
             * Empaquetar la opacidad del glifo en el canal alpha.
             * gfx_put_pixel interpretará:
             *   alpha 0        → opaco (colores RGB() legados — no aplica aquí)
             *   alpha 1..254   → blend automático sobre el fondo del backbuffer
             *   alpha 255      → opaco directo (fast path)
             */
            argb = ((uint32_t)cov << 24) | fg_base;

            /* Repetir el píxel en el bloque scale×scale. */
            for (sy = 0; sy < scale; sy++) {
                for (sx = 0; sx < scale; sx++) {
                    gfx_put_pixel(px + sx, py + sy, argb);
                }
            }
        }
    }
}

/* ── gfx_aa_text ─────────────────────────────────────────────────────── */

void gfx_aa_text(int x, int y, const char *s, uint32_t fg_rgb, int scale)
{
    int ox, step_x, step_y;

    if (!s) return;
    scale  = aa_clamp_scale(scale);
    ox     = x;
    step_x = FONT_AA_GLYPH_W * scale;
    step_y = gfx_aa_line_h(scale);

    for (; *s; s++) {
        if (*s == '\n') {
            x = ox;
            y += step_y;
            continue;
        }
        gfx_aa_char(x, y, (unsigned char)*s, fg_rgb, scale);
        x += step_x;
    }
}

/* ── gfx_aa_text_w ───────────────────────────────────────────────────── */

int gfx_aa_text_w(const char *s, int scale)
{
    int n = 0;
    if (!s) return 0;
    scale = aa_clamp_scale(scale);
    for (; *s; s++) {
        if (*s != '\n') n++;
    }
    return n * FONT_AA_GLYPH_W * scale;
}
