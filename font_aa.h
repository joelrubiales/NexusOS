#ifndef FONT_AA_H
#define FONT_AA_H

/*
 * font_aa — Motor de texto antialiased para NexusOS (bare-metal).
 *
 * Cada glifo es un array de bytes de opacidad (0 = transparente, 255 = opaco).
 * El renderizado usa blend_colors() a través de gfx_put_pixel() con ARGB:
 *   alpha 0        → escribe opaco (compatibilidad RGB() existente).
 *   alpha 1..254   → auto-blend sobre el fondo actual del backbuffer.
 *   alpha 255      → escribe opaco (fast-path).
 *
 * Funciones públicas:
 *   gfx_aa_char (x, y, ch,  fg_rgb, scale)
 *   gfx_aa_text (x, y, str, fg_rgb, scale)
 *   gfx_aa_text_w(str, scale)   → ancho total en píxeles
 *   gfx_aa_line_h(scale)        → alto de línea (+ interlineado)
 *
 * scale = 1 → cuerpo (10×18 px)
 * scale = 2 → subtítulo (20×36 px)
 * scale = 3 → título grande (30×54 px)
 */

#include <stdint.h>
#include "font_aa_data.h"

/*
 * Renderiza el carácter 'ch' en (x, y) con el color fg_rgb (0x00RRGGBB).
 * scale: 1–4. La opacidad de cada glifo se mezcla sobre el fondo actual.
 */
void gfx_aa_char(int x, int y, unsigned char ch, uint32_t fg_rgb, int scale);

/*
 * Renderiza la cadena 's'. Soporta '\n' (retorno al inicio de línea + avance).
 */
void gfx_aa_text(int x, int y, const char *s, uint32_t fg_rgb, int scale);

/*
 * Ancho en píxeles de la cadena 's' a la escala indicada.
 * Solo cuenta caracteres no-newline.
 */
int gfx_aa_text_w(const char *s, int scale);

/*
 * Alto de línea (glifo + interlineado mínimo) para la escala indicada.
 */
static inline int gfx_aa_line_h(int scale)
{
    if (scale < 1) scale = 1;
    /* 1 px de interlineado por nivel de escala */
    return FONT_AA_GLYPH_H * scale + scale;
}

#endif /* FONT_AA_H */
