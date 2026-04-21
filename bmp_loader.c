/*
 * Cargador BMP 32 bpp (BI_RGB) y utilidades de fuente bitmap (sprite sheet).
 */
#include "bmp_loader.h"
#include "gui.h"
#include "memory.h"

#include <stddef.h>
#include <stdint.h>

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int bmp_row_stride_bytes(int width, uint16_t bpp) {
    return (int)((((uint32_t)width * (uint32_t)bpp + 31u) / 32u) * 4u);
}

/* BMP almacena B,G,R,A → ARGB para el motor (alpha en bits 31–24). */
static uint32_t bmp_pixel_to_argb(uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

Image* load_bmp(const uint8_t* d, uint32_t file_size) {
    uint32_t       off, dib_sz, w, h_abs;
    int32_t        h_signed;
    int            top_down = 0;
    uint16_t       bpp;
    uint32_t       comp;
    int            row_stride;
    uint32_t       bf_size;
    Image*         img;
    uint32_t*      pix;
    int            y;
    size_t         pix_bytes;

    if (!d)
        return NULL;
    if (file_size != 0u && file_size < 54u)
        return NULL;
    if (d[0] != (uint8_t)'B' || d[1] != (uint8_t)'M')
        return NULL;

    bf_size = le32(d + 2);
    if (file_size == 0u)
        file_size = bf_size;
    if (file_size != 0u && bf_size > file_size)
        return NULL;

    off    = le32(d + 10);
    dib_sz = le32(d + 14);
    if (dib_sz < 40u)
        return NULL;
    if (file_size != 0u && 14u + dib_sz > file_size)
        return NULL;

    w        = le32(d + 18);
    h_signed = (int32_t)le32(d + 22);
    bpp      = le16(d + 28);
    comp     = le32(d + 30);

    if (w == 0u || w > 16384u)
        return NULL;
    if (bpp != 32u || comp != 0u) /* solo BI_RGB 32-bit */
        return NULL;

    if (h_signed < 0) {
        /* -2^31: altura inválida en BMP prácticos. */
        if ((uint32_t)h_signed == 0x80000000u)
            return NULL;
        top_down = 1;
        h_abs    = (uint32_t)(-h_signed);
    } else
        h_abs = (uint32_t)h_signed;

    if (h_abs == 0u || h_abs > 16384u)
        return NULL;

    row_stride = bmp_row_stride_bytes((int)w, bpp);
    if (row_stride <= 0)
        return NULL;

    if (off > file_size || (uint64_t)off + (uint64_t)row_stride * (uint64_t)h_abs > (uint64_t)file_size)
        return NULL;

    pix_bytes = (size_t)(unsigned)w * (size_t)h_abs * sizeof(uint32_t);
    if (pix_bytes / sizeof(uint32_t) != (size_t)(unsigned)w * (size_t)h_abs)
        return NULL;

    img = (Image*)kmalloc(sizeof(Image));
    if (!img)
        return NULL;
    pix = (uint32_t*)kmalloc(pix_bytes);
    if (!pix) {
        kfree(img);
        return NULL;
    }

    img->width  = (int)w;
    img->height = (int)h_abs;
    img->pixels = pix;

    for (y = 0; y < (int)h_abs; y++) {
        int           src_row = top_down ? y : ((int)h_abs - 1 - y);
        const uint8_t* src    = d + off + (size_t)src_row * (size_t)row_stride;
        uint32_t*      dst    = pix + (size_t)y * (size_t)w;
        int            x;
        for (x = 0; x < (int)w; x++) {
            const uint8_t* s = src + (size_t)x * 4u;
            dst[x]           = bmp_pixel_to_argb(s[0], s[1], s[2], s[3]);
        }
    }

    return img;
}

void free_image(Image* img) {
    if (!img)
        return;
    if (img->pixels)
        kfree(img->pixels);
    kfree(img);
}

void bmp_font_init(BmpFont* f, const Image* sheet, int glyph_w, int glyph_h, int columns,
                   int first_codepoint) {
    if (!f)
        return;
    f->sheet           = sheet;
    f->glyph_w         = glyph_w;
    f->glyph_h         = glyph_h;
    f->columns         = columns > 0 ? columns : 1;
    f->first_codepoint = first_codepoint;
}

void bmp_font_draw_glyph(const BmpFont* f, int x, int y, unsigned char ch) {
    unsigned u;
    int      gx, gy, sx, sy;

    if (!f || !f->sheet || !f->sheet->pixels)
        return;
    if (f->glyph_w < 1 || f->glyph_h < 1)
        return;

    u = (unsigned)ch;
    if (u < (unsigned)f->first_codepoint)
        return;
    u -= (unsigned)f->first_codepoint;

    gx = (int)(u % (unsigned)f->columns);
    gy = (int)(u / (unsigned)f->columns);
    sx = gx * f->glyph_w;
    sy = gy * f->glyph_h;

    if (sx + f->glyph_w > f->sheet->width || sy + f->glyph_h > f->sheet->height)
        return;

    gui_draw_image_rect(x, y, sx, sy, f->glyph_w, f->glyph_h, f->sheet);
}
