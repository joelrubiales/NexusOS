/* AUTO-GENERADO por gen_font_aa.py — NO editar manualmente. */
#ifndef FONT_AA_DATA_H
#define FONT_AA_DATA_H

#include <stdint.h>

/*
 * Fuente grayscale antialiased — pipeline Gaussian + area-average.
 * Cada glifo: 18 filas × 10 columnas, uint8_t (0=transparente, 255=opaco).
 * Rango ASCII: 0x20–0x7E  (95 glifos).
 *
 * Uso del canal de opacidad con blend_colors():
 *   uint32_t argb = ((uint32_t)glifo[y][x] << 24) | fg_rgb;
 *   gfx_put_pixel(px, py, argb);  // auto-blend si alpha 1..254
 */
#define FONT_AA_GLYPH_W  10
#define FONT_AA_GLYPH_H  18
#define FONT_AA_FIRST    0x20
#define FONT_AA_NGLYPHS  95

/* font_aa_glyphs[glyph_index][row][col] */
extern const uint8_t font_aa_glyphs[95][18][10];

#endif /* FONT_AA_DATA_H */
