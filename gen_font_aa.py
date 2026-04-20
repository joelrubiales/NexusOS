#!/usr/bin/env python3
"""
gen_font_aa.py — Genera font_aa_data.c/h con glifos grayscale antialiased.

Pipeline por glifo (ASCII 0x20–0x7E):
  1. Leer el glifo 8×8 1-bit de font8x8.c  (regex sobre el fuente C).
  2. Upsample ×UPSAMPLE con nearest-neighbor → imagen float (UPSAMPLE*8)².
  3. Gaussian blur separable (sigma=SIGMA en espacio upsampled) para
     simular la "extensión de tinta" que producen las fuentes vectoriales.
  4. Area-average downsample a GLYPH_W × GLYPH_H.
  5. Stretch lineal a [0, 255] por glifo y quantizar a uint8.

Salida: font_aa_data.c  y  font_aa_data.h  en el mismo directorio.

Uso:
  python3 gen_font_aa.py
  python3 gen_font_aa.py [output_dir]

No requiere dependencias externas (solo stdlib).
"""

import re, math, os, sys

# ── Parámetros de la fuente de salida ────────────────────────────────────
GLYPH_W   = 10   # ancho del glifo en píxeles
GLYPH_H   = 18   # alto del glifo en píxeles
UPSAMPLE  = 5    # factor de supersampling intermedio (5 → imagen 40×40)
SIGMA     = 2.2  # sigma del Gaussian en espacio upsampled (~0.44 px fuente)
FIRST_CHAR = 0x20
NUM_GLYPHS = 95  # 0x20 – 0x7E inclusive

# ── I.  Parsear font8x8.c ─────────────────────────────────────────────────

def parse_font8x8(path: str) -> list:
    """
    Devuelve lista de 95 glifos, cada uno como lista de 8 ints (bytes de fila).
    La convención de bits es MSB-derecha: bit 0 = columna 0 (izquierda).
    """
    with open(path, encoding='utf-8', errors='replace') as fh:
        src = fh.read()

    # Captura grupos { 0xHH, ... } con exactamente 8 valores hex
    groups = []
    for m in re.finditer(r'\{([^{}]*?)\}', src, re.DOTALL):
        vals = re.findall(r'0[Xx]([0-9A-Fa-f]{2})', m.group(1))
        if len(vals) == 8:
            groups.append([int(v, 16) for v in vals])
        if len(groups) == NUM_GLYPHS:
            break

    if len(groups) < NUM_GLYPHS:
        # Rellena con glifos vacíos si faltan (no debería ocurrir)
        while len(groups) < NUM_GLYPHS:
            groups.append([0] * 8)

    return groups[:NUM_GLYPHS]


def get_bit(glyph: list, col: int, row: int) -> int:
    """
    Devuelve el bit (col, row) del glifo.
    col: 0 = columna izquierda (bit 0 / LSB del byte de fila).
    """
    if row < 0 or row >= 8 or col < 0 or col >= 8:
        return 0
    return (glyph[row] >> col) & 1


# ── II. Upsample nearest-neighbor ────────────────────────────────────────

def upsample_nn(glyph: list, factor: int) -> list:
    """
    Expande el glifo 8×8 a (8*factor × 8*factor) por nearest-neighbor.
    Devuelve lista de listas de float en [0.0, 1.0].
    """
    dim = 8 * factor
    img = []
    for oy in range(dim):
        src_y = oy * 8 // dim
        row = []
        for ox in range(dim):
            src_x = ox * 8 // dim
            row.append(float(get_bit(glyph, src_x, src_y)))
        img.append(row)
    return img


# ── III. Gaussian blur separable ─────────────────────────────────────────

def _gauss_kernel_1d(sigma: float, radius: int) -> list:
    """Kernel gaussiano 1-D normalizado de longitud (2*radius+1)."""
    k = [math.exp(-(i * i) / (2 * sigma * sigma))
         for i in range(-radius, radius + 1)]
    total = sum(k)
    return [v / total for v in k]


def gaussian_blur_sep(img: list, h: int, w: int,
                      sigma: float, radius: int) -> list:
    """
    Gaussian blur separable (horizontal + vertical).
    img : lista de h listas de w floats.
    Devuelve nueva imagen del mismo tamaño.
    """
    k = _gauss_kernel_1d(sigma, radius)
    half = radius

    # ── Pase horizontal ──────────────────────────────────────────────────
    tmp = [[0.0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            acc = 0.0
            for i, wt in enumerate(k):
                xi = max(0, min(w - 1, x + i - half))
                acc += img[y][xi] * wt
            tmp[y][x] = acc

    # ── Pase vertical ────────────────────────────────────────────────────
    out = [[0.0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            acc = 0.0
            for i, wt in enumerate(k):
                yi = max(0, min(h - 1, y + i - half))
                acc += tmp[yi][x] * wt
            out[y][x] = acc

    return out


# ── IV. Area-average downsample ──────────────────────────────────────────

def area_downsample(img: list, src_w: int, src_h: int,
                    dst_w: int, dst_h: int) -> list:
    """
    Reduce img de (src_h × src_w) a (dst_h × dst_w) promediando áreas.
    Devuelve lista de dst_h listas de dst_w floats.
    """
    out = []
    for oy in range(dst_h):
        y0 = oy * src_h // dst_h
        y1 = (oy + 1) * src_h // dst_h
        if y1 <= y0:
            y1 = y0 + 1
        row = []
        for ox in range(dst_w):
            x0 = ox * src_w // dst_w
            x1 = (ox + 1) * src_w // dst_w
            if x1 <= x0:
                x1 = x0 + 1
            acc = 0.0
            cnt = 0
            for sy in range(y0, min(y1, src_h)):
                for sx in range(x0, min(x1, src_w)):
                    acc += img[sy][sx]
                    cnt += 1
            row.append(acc / cnt if cnt else 0.0)
        out.append(row)
    return out


# ── V. Normalización y quantización ──────────────────────────────────────

def normalize_to_u8(img: list, dst_w: int, dst_h: int) -> list:
    """
    Stretch lineal per-glifo a [0, 255] y convierte a int.
    Si el glifo es totalmente vacío (espacio en blanco) devuelve todo ceros.
    """
    max_v = max(max(row) for row in img) if img else 0.0
    if max_v < 1e-9:
        return [[0] * dst_w for _ in range(dst_h)]

    out = []
    for row in img:
        out.append([min(255, int(round(v / max_v * 255.0))) for v in row])
    return out


# ── VI. Pipeline completo por glifo ──────────────────────────────────────

def process_glyph(glyph: list, gw: int, gh: int,
                  upsample_f: int, sigma: float) -> list:
    """
    Devuelve lista de gh listas de gw uint8 (0=transparente, 255=opaco).
    """
    up_dim = 8 * upsample_f

    img = upsample_nn(glyph, upsample_f)

    radius = max(1, int(math.ceil(sigma * 2.5)))
    img = gaussian_blur_sep(img, up_dim, up_dim, sigma, radius)

    img = area_downsample(img, up_dim, up_dim, gw, gh)

    img = normalize_to_u8(img, gw, gh)

    return img


# ── VII. Generación de los archivos C ─────────────────────────────────────

HEADER_TEMPLATE = """\
/* AUTO-GENERADO por gen_font_aa.py — NO editar manualmente. */
#ifndef FONT_AA_DATA_H
#define FONT_AA_DATA_H

#include <stdint.h>

/*
 * Fuente grayscale antialiased — pipeline Gaussian + area-average.
 * Cada glifo: {gh} filas × {gw} columnas, uint8_t (0=transparente, 255=opaco).
 * Rango ASCII: 0x{fc:02X}–0x{lc:02X}  ({ng} glifos).
 *
 * Uso del canal de opacidad con blend_colors():
 *   uint32_t argb = ((uint32_t)glifo[y][x] << 24) | fg_rgb;
 *   gfx_put_pixel(px, py, argb);  // auto-blend si alpha 1..254
 */
#define FONT_AA_GLYPH_W  {gw}
#define FONT_AA_GLYPH_H  {gh}
#define FONT_AA_FIRST    0x{fc:02X}
#define FONT_AA_NGLYPHS  {ng}

/* font_aa_glyphs[glyph_index][row][col] */
extern const uint8_t font_aa_glyphs[{ng}][{gh}][{gw}];

#endif /* FONT_AA_DATA_H */
"""

SOURCE_PREAMBLE = """\
/* AUTO-GENERADO por gen_font_aa.py — NO editar manualmente. */
#include "font_aa_data.h"

const uint8_t font_aa_glyphs[FONT_AA_NGLYPHS][FONT_AA_GLYPH_H][FONT_AA_GLYPH_W] = {
"""


def emit_files(processed: list, out_dir: str,
               gw: int, gh: int) -> None:
    hdr_path = os.path.join(out_dir, 'font_aa_data.h')
    src_path = os.path.join(out_dir, 'font_aa_data.c')

    # Header
    with open(hdr_path, 'w', encoding='utf-8') as fh:
        fh.write(HEADER_TEMPLATE.format(
            gh=gh, gw=gw,
            fc=FIRST_CHAR, lc=FIRST_CHAR + NUM_GLYPHS - 1,
            ng=NUM_GLYPHS))

    # Source
    with open(src_path, 'w', encoding='utf-8') as fh:
        fh.write(SOURCE_PREAMBLE)
        for idx, g in enumerate(processed):
            ch_code = FIRST_CHAR + idx
            label = chr(ch_code)
            # escape unsafe chars for C comment
            if label in ('/', '*', '\\'):
                label = '?'
            fh.write(f"    /* 0x{ch_code:02X} '{label}' */ {{\n")
            for row in g:
                hex_row = ', '.join(f'0x{v:02X}' for v in row)
                fh.write(f'        {{ {hex_row} }},\n')
            fh.write('    },\n')
        fh.write('};\n')

    total_bytes = NUM_GLYPHS * gh * gw
    print(f'gen_font_aa: {hdr_path}')
    print(f'gen_font_aa: {src_path}')
    print(f'gen_font_aa: {NUM_GLYPHS} glifos × {gh}×{gw} = {total_bytes} bytes')


# ── VIII. Punto de entrada ─────────────────────────────────────────────────

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir    = sys.argv[1] if len(sys.argv) > 1 else script_dir

    font8x8_path = os.path.join(script_dir, 'font8x8.c')
    if not os.path.isfile(font8x8_path):
        print(f'ERROR: no se encontró {font8x8_path}', file=sys.stderr)
        sys.exit(1)

    glyphs    = parse_font8x8(font8x8_path)
    processed = [process_glyph(g, GLYPH_W, GLYPH_H, UPSAMPLE, SIGMA)
                 for g in glyphs]

    emit_files(processed, out_dir, GLYPH_W, GLYPH_H)


if __name__ == '__main__':
    main()
