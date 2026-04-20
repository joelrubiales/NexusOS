#ifndef FONT_DATA_H
#define FONT_DATA_H

/* Tipografía HQ: celdas 16×24 lógicas (glifo 16×16 antialias + márgenes).
 * Los datos bitmap compactos se derivan de la fuente 8×8 con supersampling
 * en tiempo de dibujado (ver gfx_draw_text_hq en gfx.c). */
#define FONT_HQ_CELL_W 16
#define FONT_HQ_CELL_H 24
#define FONT_HQ_GLYPH_PX 16

#endif
