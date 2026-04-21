#ifndef BMP_LOADER_H
#define BMP_LOADER_H

#include <stdint.h>

/*
 * Imagen descomprimida en memoria (ARGB 0xAARRGGBB, coherente con gui_blend_colors).
 * `pixels` es fila mayor arriba, ancho continuo (sin pitch extra).
 */
typedef struct Image {
    int        width;
    int        height;
    uint32_t*  pixels;
} Image;

typedef struct BmpFont {
    const Image* sheet;
    int          glyph_w;
    int          glyph_h;
    int          columns;
    int          first_codepoint; /* p. ej. 32 para espacio */
} BmpFont;

/*
 * Decodifica BMP 32 bpp BI_RGB (cabecera 'BM'), filas → top-down en `pixels`.
 * `file_size`: tamaño del buffer; use 0 para confiar en bfSize de la cabecera.
 * Devuelve imagen en heap (kmalloc) o NULL.
 */
Image* load_bmp(const uint8_t* file_data, uint32_t file_size);

void free_image(Image* img);

void bmp_font_init(BmpFont* f, const Image* sheet, int glyph_w, int glyph_h, int columns,
                   int first_codepoint);

/* Dibuja un glifo vía gui_draw_image_rect (requiere backbuffer GUI activo). */
void bmp_font_draw_glyph(const BmpFont* f, int x, int y, unsigned char ch);

#endif
