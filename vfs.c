#include "vfs.h"
#include "tar.h"
#include "memory.h"
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  USTAR TAR parser + BMP 24/32-bit loader para NexusOS initrd.
 *
 *  El GRUB carga el módulo (initrd.tar) en memoria física y nos pasa la
 *  dirección en el tag MB2 de tipo MODULE.  Aquí solo guardamos el puntero
 *  y caminamos el TAR en caliente, sin copiar nada (zero-copy para find).
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ─── Estado global ─────────────────────────────────────────────────────── */
static const uint8_t* initrd_base = NULL;
static uint32_t       initrd_size = 0;

/* ─── Caché de wallpaper e iconos ───────────────────────────────────────── */
static uint32_t* wp_px   = NULL;
static int       wp_w    = 0;
static int       wp_h    = 0;
static int       wp_done = 0;       /* 1 = ya intentado (sea éxito o no) */

static uint32_t* icon_px  [VFS_ICON_MAX];
static int       icon_w   [VFS_ICON_MAX];
static int       icon_h   [VFS_ICON_MAX];
static int       icon_done[VFS_ICON_MAX];

/* ─── Helpers de cadenas (sin libc) ─────────────────────────────────────── */
static int vfs_strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* Descarta '/' y './' del inicio de una ruta para comparar de forma
 * canonicalizada: "/icons/x.bmp" == "./icons/x.bmp" == "icons/x.bmp". */
static const char* canon(const char* s) {
    while (*s == '/') s++;
    if (s[0] == '.' && s[1] == '/') s += 2;
    while (*s == '/') s++;
    return s;
}


/* ─── Inicialización ─────────────────────────────────────────────────────── */
void vfs_init(uint32_t mod_start, uint32_t mod_end) {
    int i;
    initrd_base = NULL;
    initrd_size = 0;
    wp_px   = NULL; wp_w = 0; wp_h = 0; wp_done = 0;
    for (i = 0; i < VFS_ICON_MAX; i++) {
        icon_px[i] = NULL; icon_w[i] = 0; icon_h[i] = 0; icon_done[i] = 0;
    }
    if (mod_start == 0 || mod_end <= mod_start) return;
    initrd_base = (const uint8_t*)(uintptr_t)mod_start;
    initrd_size = mod_end - mod_start;
    /* Inicializar el módulo TAR con la misma región de memoria. */
    tar_init(initrd_base, initrd_size);
}

int vfs_ready(void) { return initrd_base != NULL; }

/* ═══════════════════════════════════════════════════════════════════════════
 *  USTAR TAR walker
 *
 *  Formato:
 *    Bloque de 512 bytes: cabecera POSIX ustar
 *      [0..99]   nombre del archivo
 *      [124..135] tamaño en bytes (octal ASCII, 11 dígitos + nul)
 *      [156]     tipo: '0'/'\0' = fichero regular, '5' = directorio, etc.
 *      [257..262] magia "ustar"
 *    Tras la cabecera: CEIL(size / 512) bloques de 512 bytes con los datos.
 *    El TAR termina con dos bloques consecutivos de ceros.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TAR_BLOCK 512u

static uint32_t tar_octal(const uint8_t* s, int n) {
    uint32_t v = 0;
    while (n-- > 0 && *s >= '0' && *s <= '7')
        v = v * 8u + (uint32_t)(*s++ - '0');
    return v;
}

const uint8_t* vfs_find(const char* path, uint32_t* out_size) {
    uint32_t off = 0;
    const char* want = canon(path);

    if (!initrd_base || initrd_size < TAR_BLOCK) return NULL;
    if (out_size) *out_size = 0;

    while (off + TAR_BLOCK <= initrd_size) {
        const uint8_t* hdr = initrd_base + off;

        /* Dos bloques de ceros → fin del TAR. */
        if (hdr[0] == 0) return NULL;

        /* Tamaño del archivo (offset 124, 12 bytes de octal ASCII). */
        uint32_t fsize = tar_octal(hdr + 124, 12);

        /* Tipo (offset 156): '0' o '\0' → fichero regular. */
        uint8_t ftype = hdr[156];
        int is_file = (ftype == '0' || ftype == '\0');

        if (is_file) {
            /* Nombre (offset 0, 100 bytes) + prefijo ustar (offset 345, 155 bytes).
             * Para rutas largas, el nombre completo es prefix + '/' + name.
             * En la mayoría de los casos el prefix está vacío. */
            char entry[256];
            int  ei = 0;
            /* Prefijo (campo offset 345, 155 bytes). */
            const uint8_t* pfx = hdr + 345;
            if (pfx[0]) {
                int pi;
                for (pi = 0; pi < 155 && pfx[pi]; pi++)
                    if (ei < 254) entry[ei++] = (char)pfx[pi];
                if (ei < 254) entry[ei++] = '/';
            }
            /* Nombre base. */
            int ni;
            for (ni = 0; ni < 100 && hdr[ni]; ni++)
                if (ei < 254) entry[ei++] = (char)hdr[ni];
            entry[ei] = '\0';

            const char* have = canon(entry);
            if (vfs_strcmp(have, want) == 0) {
                const uint8_t* data = hdr + TAR_BLOCK;
                /* Verificar que el payload cabe en el initrd. */
                if (off + TAR_BLOCK + fsize > initrd_size) return NULL;
                if (out_size) *out_size = fsize;
                return data;
            }
        }

        /* Avanzar al siguiente bloque: cabecera + CEIL(fsize/512) bloques. */
        uint32_t data_blocks = (fsize + TAR_BLOCK - 1u) / TAR_BLOCK;
        off += TAR_BLOCK + data_blocks * TAR_BLOCK;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  BMP loader (24-bit y 32-bit sin compresión)
 *
 *  Formato BMP 32-bit en memoria: bytes [B, G, R, A] por píxel.
 *  Como uint32_t LE: A<<24 | R<<16 | G<<8 | B = 0xAARRGGBB.
 *  Eso es exactamente el formato de gfx_draw_image_rgba. ← Sin conversión.
 *
 *  Cabecera:
 *    0..1   "BM"
 *    2..5   tamaño del fichero
 *    10..13 offset al inicio de los píxeles
 *    14..53 BITMAPINFOHEADER (40 bytes)
 *      14..17 tamaño header (40)
 *      18..21 width (int32, positivo)
 *      22..25 height (int32; negativo = top-down, positivo = bottom-up)
 *      26..27 planes (1)
 *      28..29 bpp (24 o 32)
 *      30..33 compression (0 = ninguna)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static int32_t le32s(const uint8_t* p) {
    uint32_t u = le32(p);
    int32_t  s;
    /* Reinterpretación segura sin UB. */
    __builtin_memcpy(&s, &u, 4);
    return s;
}

uint32_t* vfs_load_bmp(const char* path, int* out_w, int* out_h) {
    uint32_t   file_size;
    const uint8_t* d;
    uint32_t   pix_off;
    int32_t    bmp_width, bmp_height_raw;
    int        height, width, bottom_up;
    uint16_t   bpp;
    uint32_t   compression;
    uint32_t   row_stride;
    uint32_t   bytes_per_px;
    uint32_t*  buf;
    int        y;

    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;

    d = vfs_find(path, &file_size);
    if (!d || file_size < 54u) return NULL;

    /* Verificar magia "BM". */
    if (d[0] != 'B' || d[1] != 'M') return NULL;

    pix_off     = le32(d + 10);
    bmp_width   = le32s(d + 18);
    bmp_height_raw = le32s(d + 22);
    bpp         = le16(d + 28);
    compression = le32(d + 30);

    if (bmp_width  <= 0 || bmp_width  > 4096) return NULL;
    if (bmp_height_raw == 0 || bmp_height_raw > 4096 || bmp_height_raw < -4096)
        return NULL;
    if (bpp != 24 && bpp != 32) return NULL;
    if (compression != 0u) return NULL;

    width     = bmp_width;
    bottom_up = (bmp_height_raw > 0);
    height    = bottom_up ? (int)bmp_height_raw : (int)(-bmp_height_raw);

    bytes_per_px = (uint32_t)bpp / 8u;
    /* Cada fila está alineada a 4 bytes en el archivo. */
    row_stride   = ((uint32_t)width * bytes_per_px + 3u) & ~3u;

    if (pix_off + (uint32_t)height * row_stride > file_size) return NULL;

    buf = (uint32_t*)kmalloc((uint64_t)width * (uint64_t)height * 4u);
    if (!buf) return NULL;

    {
        const uint8_t* pixels = d + pix_off;
        int x;
        for (y = 0; y < height; y++) {
            /* BMP bottom-up: la fila 0 del bitmap es la inferior de la imagen. */
            int src_row = bottom_up ? (height - 1 - y) : y;
            const uint8_t* row = pixels + (uint32_t)src_row * row_stride;
            uint32_t* dst = buf + (uint32_t)y * (uint32_t)width;

            for (x = 0; x < width; x++) {
                const uint8_t* px = row + (uint32_t)x * bytes_per_px;
                uint8_t b = px[0];
                uint8_t g = px[1];
                uint8_t r = px[2];
                uint8_t a = (bpp == 32u) ? px[3] : 0xFFu;
                /* 0xAARRGGBB: compatible con gfx_draw_image_rgba. */
                dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16)
                        | ((uint32_t)g << 8)  |  (uint32_t)b;
            }
        }
    }

    if (out_w) *out_w = width;
    if (out_h) *out_h = height;
    return buf;
}

/* ─── Caché de assets ────────────────────────────────────────────────────── */

const uint32_t* vfs_get_wallpaper(int* out_w, int* out_h) {
    if (!wp_done) {
        wp_done = 1;
        wp_px = vfs_load_bmp("/background.bmp", &wp_w, &wp_h);
    }
    if (out_w) *out_w = wp_w;
    if (out_h) *out_h = wp_h;
    return wp_px;
}

const uint32_t* vfs_get_icon(int id, int* out_w, int* out_h) {
    if (id < 0 || id >= VFS_ICON_MAX) return NULL;

    if (!icon_done[id]) {
        icon_done[id] = 1;
        /* Ruta: /icons/0.bmp … /icons/7.bmp */
        char path[] = "/icons/0.bmp";
        /* Sobrescribir el dígito en la posición 7. */
        path[7] = (char)('0' + id);
        icon_px[id] = vfs_load_bmp(path, &icon_w[id], &icon_h[id]);
    }

    if (out_w) *out_w = icon_w[id];
    if (out_h) *out_h = icon_h[id];
    return icon_px[id];
}
