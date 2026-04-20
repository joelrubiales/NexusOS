#include "gfx.h"
#include "desktop.h"
#include "font_data.h"
#include "font8x8.h"
#include "nexus.h"
#include "memory.h"
#include <stdint.h>

_Static_assert(sizeof(VesaBootInfo) == 28, "VesaBootInfo must match boot2 / boot_info.h");

/* ── Framebuffer state ────────────────────────────────────────────── */
static volatile unsigned char* lfb = 0;
static unsigned int* backbuf = 0;
int screen_width = 0;
int screen_height = 0;

int layout_ui_scale = 1;
int layout_top_h = 25;
int layout_dock_w = 100;
int layout_dock_h = 68;
int layout_dock_x = 0;
int layout_dock_y = 600;
int layout_dock_margin_bottom = 10;
int layout_dock_r = 24;
int layout_icon_size = 28;
int layout_slot_sp = 50;
int layout_chrome_title_h = 32;
int layout_chrome_corner_r = 14;

static int scr_w, scr_h, scr_pitch, scr_bpp;
static int fb_size;
/* uint32s per scanline (LFB pitch/4 when drawing direct; else scr_w). */
static int fb_stride = 0;
static int gfx_direct32 = 0;

/* RAM backbuffer at 4MB — solo si no dibujamos directo al LFB (p. ej. 24 bpp). */
#define BACKBUF_ADDR 0x400000
static unsigned int* static_backbuf = (unsigned int*)BACKBUF_ADDR;

/* ── Video handoff @ BOOT_INFO_ADDR (boot2 / SVGA) ─ */
int gfx_vesa_detect(VesaBootInfo* out) {
    volatile BootInfo* bi = (volatile BootInfo*)(uintptr_t)NEXUS_BOOT_INFO_PHYS;
    if (bi->magic != (uint32_t)BOOT_INFO_MAGIC)
        return 0;
    out->magic    = bi->magic;
    out->width    = bi->width;
    out->height   = bi->height;
    out->pitch    = bi->pitch;
    out->bpp      = bi->bpp;
    out->lfb_ptr  = bi->lfb_ptr;
    return (out->lfb_ptr != 0ull && out->width > 0u && out->height > 0u && out->pitch >= 4u);
}

void gfx_layout_refresh(void) {
    int sw = screen_width;
    int sh = screen_height;
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;

    layout_ui_scale = (sw < sh) ? sw : sh;

    /* Barra superior — misma política que desktop.h (resolución del buzón). */
    layout_top_h = DESK_TOP_BAR_HEIGHT;

    /* Dock: DESK_DOCK_WIDTH_PCENT % ancho, centrado; margen inferior DESK_DOCK_BOTTOM_PAD. */
    layout_dock_w = (sw * DESK_DOCK_WIDTH_PCENT) / 100;
    if (layout_dock_w < 1) layout_dock_w = 1;
    layout_dock_h = (sh * 6) / 100;
    if (layout_dock_h < DESK_DOCK_MIN_HEIGHT) layout_dock_h = DESK_DOCK_MIN_HEIGHT;
    if (layout_dock_h > sh / 4) layout_dock_h = sh / 4;
    layout_dock_margin_bottom = DESK_DOCK_BOTTOM_PAD;
    layout_dock_x = (sw - layout_dock_w) / 2;
    layout_dock_y = sh - layout_dock_h - layout_dock_margin_bottom;
    if (layout_dock_y < layout_top_h) layout_dock_y = layout_top_h;

    layout_dock_r = (layout_ui_scale * 6) / 100;
    if (layout_dock_r < 14) layout_dock_r = 14;
    if (layout_dock_r > 48) layout_dock_r = 48;

    layout_icon_size = (layout_ui_scale * 5) / 100;
    if (layout_icon_size < 22) layout_icon_size = 22;
    if (layout_icon_size > 48) layout_icon_size = 48;

    layout_slot_sp = (layout_icon_size * 195) / 100;
    if (layout_slot_sp < layout_icon_size + 10) layout_slot_sp = layout_icon_size + 10;

    layout_chrome_title_h = (layout_ui_scale * 52) / 1000;
    if (layout_chrome_title_h < 28) layout_chrome_title_h = 28;
    if (layout_chrome_title_h > 42) layout_chrome_title_h = 42;
    if (layout_chrome_title_h > sh / 5) layout_chrome_title_h = sh / 5;

    layout_chrome_corner_r = (layout_ui_scale * 28) / 1000;
    if (layout_chrome_corner_r < 10) layout_chrome_corner_r = 10;
    if (layout_chrome_corner_r > 22) layout_chrome_corner_r = 22;
}

void gfx_init_vesa(uint64_t lfb_phys, int w, int h, int pitch, int bpp) {
    int min_pitch = w * (bpp / 8);
    if (pitch < min_pitch) pitch = min_pitch;
    lfb = (volatile unsigned char*)(uintptr_t)lfb_phys;
    scr_w = w;
    scr_h = h;
    screen_width = w;
    screen_height = h;
    scr_pitch = pitch;
    scr_bpp = bpp;
    fb_size = w * h;
    gfx_direct32 = 0;

    /* 32 bpp: dibujar directo en el LFB con stride real (pitch/4). Evita copia
       y desajuste backbuffer compacto vs filas con padding — causa típica de franjas. */
    if (bpp == 32) {
        gfx_direct32 = 1;
        fb_stride = pitch / 4;
        backbuf = (unsigned int*)(uintptr_t)lfb_phys;
    } else {
        fb_stride = scr_w;
        backbuf = static_backbuf;
    }

    gfx_layout_refresh();
}

/* Fallback old VGA (unused if VESA works, but kept for safety) */
void vga_set_mode_320x200x256_linear(void);
void vga_set_text_80x25(void);

void gfx_init_vga(void) {
    vga_set_mode_320x200x256_linear();
    scr_w = 320; scr_h = 200;
    screen_width = 320;
    screen_height = 200;
    scr_bpp = 8; scr_pitch = 320;
    lfb = (volatile unsigned char*)0xA0000;
    fb_size = 320*200;
    backbuf = 0;
    fb_stride = 0;
    gfx_direct32 = 0;
    gfx_layout_refresh();
}

int gfx_width(void)  { return scr_w; }
int gfx_height(void) { return scr_h; }

void gfx_fill_screen_solid(unsigned int rgb) {
    int y, x;
    if (!backbuf || scr_w <= 0 || scr_h <= 0) return;
    if (scr_bpp == 32) {
        int pitch_u32 = scr_pitch / 4;
        uint32_t* vram = (uint32_t*)backbuf;
        uint32_t c = (uint32_t)rgb;
        for (y = 0; y < scr_h; y++) {
            uint32_t* row = vram + (unsigned)y * (unsigned)pitch_u32;
            for (x = 0; x < scr_w; x++)
                row[x] = c;
        }
    } else {
        for (y = 0; y < scr_h; y++) {
            unsigned int* row = backbuf + y * fb_stride;
            for (x = 0; x < scr_w; x++)
                row[x] = rgb;
        }
    }
}

void gfx_shutdown_to_text(void) {
    vga_set_text_80x25();
}

void gfx_attach_double_buffer(uint32_t* buf) {
    int j, i;
    int row_u32;
    if (!buf || scr_bpp != 32 || scr_w <= 0 || scr_h <= 0 || scr_pitch <= 0) return;
    gfx_direct32 = 0;
    backbuf = buf;
    /* Una fila del double_buffer = scr_pitch bytes (= scr_pitch/4 uint32_t). */
    fb_stride = scr_pitch / 4;
    row_u32 = scr_pitch / 4;
    for (j = 0; j < scr_h; j++) {
        uint32_t* row = (uint32_t*)backbuf + (size_t)(unsigned)j * (size_t)(unsigned)row_u32;
        for (i = 0; i < row_u32; i++) row[i] = 0;
    }
}

void gfx_enable_double_buffer_kmalloc(void) {
    uint64_t sz;
    uint32_t* p;
    if (scr_bpp != 32 || scr_w <= 0 || scr_h <= 0 || scr_pitch <= 0) return;
    sz = (uint64_t)(unsigned)scr_pitch * (uint64_t)(unsigned)scr_h;
    p = (uint32_t*)kmalloc(sz);
    if (!p) kheap_panic_nomem("gfx_enable_double_buffer_kmalloc");
    gfx_attach_double_buffer(p);
}

/* Copia de fila al LFB: rep movsq (cuadr palabras) + cola byte a byte. */
static void gfx_copy_row_movsq(volatile unsigned char* d, const unsigned char* s, size_t n) {
    size_t q = n / 8;
    size_t r = n % 8;
    if (q) {
        unsigned long nq = (unsigned long)q;
        void* rdi = (void*)d;
        const void* rsi = (const void*)s;
        __asm__ volatile(
            "cld \n\t"
            "rep movsq"
            : "+c"(nq), "+D"(rdi), "+S"(rsi)
            :
            : "memory", "cc"
        );
    }
    if (r) {
        volatile unsigned char* pd = d + q * 8;
        const unsigned char* ps = s + q * 8;
        size_t i;
        for (i = 0; i < r; i++)
            pd[i] = ps[i];
    }
}

void gfx_swap_buffers(void) {
    if (!backbuf || !lfb) return;

    if (gfx_direct32 && scr_bpp == 32) {
        __asm__ volatile("mfence" ::: "memory");
        vesa_force_refresh();
        return;
    }

    if (scr_bpp == 32) {
        size_t y;
        size_t row_b = (size_t)(unsigned)scr_pitch;
        const unsigned char* src0 = (const unsigned char*)backbuf;
        volatile unsigned char* dst0 = (volatile unsigned char*)lfb;
        for (y = 0; y < (size_t)(unsigned)scr_h; y++)
            gfx_copy_row_movsq(dst0 + y * row_b, src0 + y * row_b, row_b);
    } else if (scr_bpp == 24) {
        int y;
        for (y = 0; y < scr_h; y++) {
            volatile unsigned char* row = lfb + y * scr_pitch;
            unsigned int* src = backbuf + y * fb_stride;
            int x;
            for (x = 0; x < scr_w; x++) {
                unsigned int c = src[x];
                row[x * 3 + 0] = (unsigned char)(c);
                row[x * 3 + 1] = (unsigned char)(c >> 8);
                row[x * 3 + 2] = (unsigned char)(c >> 16);
            }
        }
    }
    __asm__ volatile("mfence" ::: "memory");
    vesa_force_refresh();
}

void gfx_present(void) { gfx_swap_buffers(); }

/* ── Core primitives ──────────────────────────────────────────────── */
void gfx_clear(unsigned int color) {
    int j;
    if (!backbuf || scr_w <= 0 || scr_h <= 0) return;
    if (scr_bpp == 32) {
        int pitch_u32 = scr_pitch / 4;
        uint32_t* base = (uint32_t*)backbuf;
        uint32_t c = (uint32_t)color;
        for (j = 0; j < scr_h; j++) {
            uint32_t* row = base + (size_t)(unsigned)j * (size_t)(unsigned)pitch_u32;
            int i;
            for (i = 0; i < scr_w; i++) row[i] = c;
        }
    } else {
        for (j = 0; j < scr_h; j++) {
            unsigned int* row = backbuf + j * fb_stride;
            int i;
            for (i = 0; i < scr_w; i++) row[i] = color;
        }
    }
}

void gfx_put_pixel(int x, int y, unsigned int color) {
    if ((unsigned)x >= (unsigned)scr_w || (unsigned)y >= (unsigned)scr_h || !backbuf)
        return;
    /* Double buffer: double_buffer[y * (pitch/4) + x] = color; */
    if (scr_bpp == 32) {
        uint32_t* db = (uint32_t*)backbuf;
        size_t stride_u32 = (size_t)(unsigned)(scr_pitch / 4);
        db[(size_t)(unsigned)y * stride_u32 + (size_t)(unsigned)x] = (uint32_t)color;
    } else
        backbuf[y * fb_stride + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, unsigned int color) {
    int x0, y0, x1, y1;
    if (!backbuf || w <= 0 || h <= 0) return;
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w > scr_w ? scr_w : x + w;
    y1 = y + h > scr_h ? scr_h : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    if (scr_bpp == 32) {
        size_t sp = (size_t)(unsigned)(scr_pitch / 4);
        uint32_t* base = (uint32_t*)backbuf;
        uint32_t c = (uint32_t)color;
        for (int j = y0; j < y1; j++) {
            uint32_t* row = base + (size_t)(unsigned)j * sp;
            for (int i = x0; i < x1; i++) row[i] = c;
        }
    } else {
        for (int j = y0; j < y1; j++) {
            unsigned int* row = backbuf + j * fb_stride;
            for (int i = x0; i < x1; i++) row[i] = color;
        }
    }
}

void gfx_draw_rect(int x, int y, int w, int h, unsigned int color) {
    if (!backbuf || w <= 0 || h <= 0) return;
    if (h == 1) {
        gfx_hline(x, y, w, color);
        return;
    }
    if (w == 1) {
        gfx_vline(x, y, h, color);
        return;
    }
    gfx_hline(x, y, w, color);
    gfx_hline(x, y+h-1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x+w-1, y, h, color);
}

void gfx_hline(int x, int y, int w, unsigned int color) {
    if (!backbuf || w <= 0) return;
    if(y < 0 || y >= scr_h) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = x+w > scr_w ? scr_w : x+w;
    if (scr_bpp == 32) {
        uint32_t* row = (uint32_t*)backbuf + (size_t)(unsigned)y * (size_t)(unsigned)(scr_pitch / 4);
        for (int i = x0; i < x1; i++) row[i] = (uint32_t)color;
    } else {
        unsigned int* row = backbuf + y * fb_stride;
        for (int i = x0; i < x1; i++) row[i] = color;
    }
}

void gfx_vline(int x, int y, int h, unsigned int color) {
    if (!backbuf || h <= 0) return;
    if(x < 0 || x >= scr_w) return;
    int y0 = y < 0 ? 0 : y;
    int y1 = y+h > scr_h ? scr_h : y+h;
    if (scr_bpp == 32) {
        size_t sp = (size_t)(unsigned)(scr_pitch / 4);
        uint32_t* base = (uint32_t*)backbuf;
        uint32_t c = (uint32_t)color;
        for (int j = y0; j < y1; j++) base[(size_t)(unsigned)j * sp + (size_t)(unsigned)x] = c;
    } else {
        for (int j = y0; j < y1; j++) backbuf[j * fb_stride + x] = color;
    }
}

/* ── Rounded rect (filled, radius r) ─────────────────────────────── */
void gfx_fill_rounded_rect(int x, int y, int w, int h, int r, unsigned int color) {
    if (!backbuf || w <= 0 || h <= 0) return;
    if(r > h/2) r = h/2;
    if(r > w/2) r = w/2;
    gfx_fill_rect(x+r, y, w-2*r, h, color);
    gfx_fill_rect(x, y+r, r, h-2*r, color);
    gfx_fill_rect(x+w-r, y+r, r, h-2*r, color);
    for(int dy = 0; dy < r; dy++) {
        for(int dx = 0; dx < r; dx++) {
            int dist2 = (r-1-dx)*(r-1-dx) + (r-1-dy)*(r-1-dy);
            if(dist2 <= (r-1)*(r-1)) {
                gfx_put_pixel(x+dx, y+dy, color);
                gfx_put_pixel(x+w-1-dx, y+dy, color);
                gfx_put_pixel(x+dx, y+h-1-dy, color);
                gfx_put_pixel(x+w-1-dx, y+h-1-dy, color);
            }
        }
    }
}

/* ── Gradient (vertical, from top_col to bot_col) ────────────────── */
uint32_t gfx_lerp_rgb(uint32_t a, uint32_t b, int t, int max) {
    int ar = (int)((a >> 16) & 0xFFu), ag = (int)((a >> 8) & 0xFFu), ab = (int)(a & 0xFFu);
    int br = (int)((b >> 16) & 0xFFu), bg = (int)((b >> 8) & 0xFFu), bb = (int)(b & 0xFFu);
    int r, g, bl;
    if (max < 1) max = 1;
    if (t < 0) t = 0;
    if (t > max) t = max;
    r = ar + ((br - ar) * t) / max;
    g = ag + ((bg - ag) * t) / max;
    bl = ab + ((bb - ab) * t) / max;
    return ((uint32_t)(unsigned)r << 16) | ((uint32_t)(unsigned)g << 8) | (uint32_t)(unsigned)bl;
}

void gfx_gradient_v(int x, int y, int w, int h, unsigned int top, unsigned int bot) {
    int j;
    if (!backbuf || w <= 0 || h <= 0) return;
    for (j = 0; j < h; j++) {
        uint32_t c = gfx_lerp_rgb((uint32_t)top, (uint32_t)bot, j, h > 1 ? h - 1 : 1);
        gfx_hline(x, y + j, w, (unsigned int)c);
    }
}

void gfx_gradient_diagonal(int x, int y, int w, int h, unsigned int c_tl, unsigned int c_br) {
    int j, i, t_num, t_den;
    if (!backbuf || w <= 0 || h <= 0) return;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            t_num = i + j;
            t_den = w + h - 2;
            if (t_den < 1) t_den = 1;
            gfx_put_pixel(x + i, y + j, (unsigned int)gfx_lerp_rgb((uint32_t)c_tl, (uint32_t)c_br, t_num, t_den));
        }
    }
}

unsigned int gfx_get_pixel(int x, int y) {
    if ((unsigned)x >= (unsigned)scr_w || (unsigned)y >= (unsigned)scr_h || !backbuf)
        return 0;
    if (scr_bpp == 32) {
        uint32_t* db = (uint32_t*)backbuf;
        size_t stride_u32 = (size_t)(unsigned)(scr_pitch / 4);
        return (unsigned int)db[(size_t)(unsigned)y * stride_u32 + (size_t)(unsigned)x];
    }
    return backbuf[y * fb_stride + x];
}

void gfx_blend_pixel(int x, int y, unsigned int fg, unsigned int alpha) {
    unsigned int dst, r, g, b, frr, fgg, fbb, dr, dg, db;
    if ((unsigned)x >= (unsigned)scr_w || (unsigned)y >= (unsigned)scr_h || !backbuf || alpha == 0)
        return;
    if (alpha >= 255) {
        gfx_put_pixel(x, y, fg);
        return;
    }
    dst = gfx_get_pixel(x, y);
    frr = (fg >> 16) & 0xFF;
    fgg = (fg >> 8) & 0xFF;
    fbb = fg & 0xFF;
    dr = (dst >> 16) & 0xFF;
    dg = (dst >> 8) & 0xFF;
    db = dst & 0xFF;
    r = (frr * alpha + dr * (255 - alpha)) / 255;
    g = (fgg * alpha + dg * (255 - alpha)) / 255;
    b = (fbb * alpha + db * (255 - alpha)) / 255;
    gfx_put_pixel(x, y, ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b);
}

void gfx_fill_circle(int cx, int cy, int r, unsigned int color) {
    int dy, dx, lo, hi;
    if (r <= 0) return;
    lo = -r;
    hi = r;
    for (dy = lo; dy <= hi; dy++) {
        for (dx = lo; dx <= hi; dx++) {
            if (dx * dx + dy * dy <= r * r + r)
                gfx_put_pixel(cx + dx, cy + dy, color);
        }
    }
}

/* ── Text rendering (8x8 font) ───────────────────────────────────── */
void gfx_draw_char(int x, int y, unsigned char ch, unsigned int fg, unsigned int bg) {
    if(x+8 > scr_w || y+8 > scr_h || x < 0 || y < 0) return;
    const unsigned char* gl = font8x8_get(ch);
    for(int r = 0; r < 8; r++) {
        unsigned char bits = gl[r];
        if (scr_bpp == 32) {
            uint32_t* row = (uint32_t*)backbuf + (size_t)(unsigned)(y + r) * (size_t)(unsigned)(scr_pitch / 4) + (size_t)(unsigned)x;
            for(int c = 0; c < 8; c++)
                row[c] = (bits >> c) & 1 ? fg : bg;
        } else {
            unsigned int* row = backbuf + (y+r) * fb_stride + x;
            for(int c = 0; c < 8; c++)
                row[c] = (bits >> c) & 1 ? fg : bg;
        }
    }
}

void gfx_draw_text(int x, int y, const char* s, unsigned int fg, unsigned int bg) {
    int ox = x;
    for(int i = 0; s[i]; i++) {
        if(s[i] == '\n') { x = ox; y += 10; continue; }
        if(x+8 > scr_w || y+8 > scr_h) return;
        gfx_draw_char(x, y, (unsigned char)s[i], fg, bg);
        x += 8;
    }
}

void gfx_draw_text_transparent(int x, int y, const char* s, unsigned int fg) {
    int ox = x;
    for(int i = 0; s[i]; i++) {
        if(s[i] == '\n') { x = ox; y += 10; continue; }
        if(x+8 > scr_w || y+8 > scr_h) return;
        const unsigned char* gl = font8x8_get((unsigned char)s[i]);
        for(int r = 0; r < 8; r++) {
            unsigned char bits = gl[r];
            for(int c = 0; c < 8; c++)
                if((bits >> c) & 1) gfx_put_pixel(x+c, y+r, fg);
        }
        x += 8;
    }
}

int gfx_text_width(const char* s) {
    int w = 0;
    for(int i = 0; s[i]; i++) { if(s[i] != '\n') w += 8; }
    return w;
}

static int font_bit(const unsigned char* gl, int col, int row) {
    if (row < 0 || row >= 8 || col < 0 || col >= 8) return 0;
    return (gl[row] >> col) & 1;
}

void gfx_blend_rect(int x, int y, int w, int h, unsigned int fg, unsigned int alpha) {
    int j, i;
    if (alpha == 0) return;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++)
            gfx_blend_pixel(x + i, y + j, fg, alpha);
    }
}

/* Escala 2 => rejilla 16x16 por carácter; 4 muestras por píxel de salida sobre el glifo 8x8 */
void gfx_draw_char_aa(int x, int y, unsigned char ch, unsigned int fg, int scale) {
    const unsigned char* gl;
    int dim, sy, sx, acc, u, v, gx, gy;
    if (!backbuf || scale < 1) return;
    if (scale > 2) scale = 2;
    dim = 8 * scale;
    gl = font8x8_get(ch);
    for (sy = 0; sy < dim; sy++) {
        for (sx = 0; sx < dim; sx++) {
            acc = 0;
            for (u = 0; u < 2; u++) {
                for (v = 0; v < 2; v++) {
                    gx = (sx * 8 + u * 4 + 2) / (dim);
                    gy = (sy * 8 + v * 4 + 2) / (dim);
                    if (gx > 7) gx = 7;
                    if (gy > 7) gy = 7;
                    acc += font_bit(gl, gx, gy);
                }
            }
            if (acc == 0) continue;
            if ((unsigned)(x + sx) >= (unsigned)scr_w || (unsigned)(y + sy) >= (unsigned)scr_h) continue;
            if (acc == 4)
                gfx_put_pixel(x + sx, y + sy, fg);
            else
                gfx_blend_pixel(x + sx, y + sy, fg, (unsigned int)((acc * 255) / 4));
        }
    }
}

void gfx_draw_text_aa(int x, int y, const char* s, unsigned int fg, int scale) {
    int ox = x;
    int dim;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;
    dim = 8 * scale;
    for (; *s; s++) {
        if (*s == '\n') {
            x = ox;
            y += dim + 4;
            continue;
        }
        if (x + dim > scr_w || y + dim > scr_h) return;
        gfx_draw_char_aa(x, y, (unsigned char)*s, fg, scale);
        x += dim;
    }
}

int gfx_text_width_aa(const char* s, int scale) {
    int n = 0;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;
    for (; *s; s++) {
        if (*s != '\n') n += 8 * scale;
    }
    return n;
}

/* ── Math helpers (enteros; sin soft-float en el enlazado del kernel) ─ */
static int gfx_iabs(int v) { return v < 0 ? -v : v; }

static unsigned int gfx_isqrt_u32(unsigned int n) {
    unsigned int x = n, c = 0, d = 1u << 30;
    if (n == 0) return 0;
    while (d > n) d >>= 2;
    while (d) {
        if (x >= c + d) {
            x -= c + d;
            c = (c >> 1) + d;
        } else
            c >>= 1;
        d >>= 2;
    }
    return c;
}

/* ── Xiaolin Wu (fijo 16.16 sobre Y; sin float) ───────────────────── */
static void plot_wu_i(int x, int y, int br255, unsigned int rgb, int steep) {
    int px, py;
    if (br255 <= 0) return;
    if (br255 > 255) br255 = 255;
    px = steep ? y : x;
    py = steep ? x : y;
    gfx_blend_pixel(px, py, rgb, (unsigned int)br255);
}

void gfx_wu_line(int x0, int y0, int x1, int y1, unsigned int rgb) {
    int steep = gfx_iabs(y1 - y0) > gfx_iabs(x1 - x0);
    int x, xa, xb, dx, dy;
    long long y_fp, m_fp;
    if (steep) {
        int t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        int t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    xa = x0;
    xb = x1;
    dx = xb - xa;
    if (dx == 0) {
        int ya = y0 < y1 ? y0 : y1;
        int yb2 = y0 < y1 ? y1 : y0;
        for (x = ya; x <= yb2; x++) {
            int px = steep ? x : xa;
            int py = steep ? xa : x;
            gfx_blend_pixel(px, py, rgb, 255);
        }
        return;
    }
    dy = y1 - y0;
    y_fp = (long long)y0 * 65536LL;
    m_fp = (dy * 65536LL) / (long long)dx;
    for (x = xa; x <= xb; x++) {
        int yb = (int)(y_fp >> 16);
        int frac = (int)((y_fp >> 8) & 255);
        plot_wu_i(x, yb, 255 - frac, rgb, steep);
        plot_wu_i(x, yb + 1, frac, rgb, steep);
        y_fp += m_fp;
    }
}

void gfx_circle_outline_aa(int cx, int cy, int r, unsigned int rgb) {
    int x, y, x0, y0, dist, a, alpha;
    if (r <= 0) return;
    for (y = -r - 2; y <= r + 2; y++) {
        for (x = -r - 2; x <= r + 2; x++) {
            dist = (int)gfx_isqrt_u32((unsigned int)(x * x + y * y));
            a = gfx_iabs(dist - r);
            if (a <= 2) {
                alpha = 255 - a * 100;
                if (alpha < 0) alpha = 0;
                if (alpha > 255) alpha = 255;
                x0 = cx + x;
                y0 = cy + y;
                gfx_blend_pixel(x0, y0, rgb, (unsigned int)alpha);
            }
        }
    }
}

/* Esquina: 0=TL, 1=TR, 2=BR, 3=BL — contorno AA coherente con centros estándar del redondeo. */
static void gfx_arc_quarter_outline_aa(int cx, int cy, int r, unsigned int rgb, int corner) {
    int lo_x, hi_x, lo_y, hi_y, px, py, dist, a, alpha;
    if (r <= 0) return;
    lo_x = cx - r - 2;
    hi_x = cx + r + 2;
    lo_y = cy - r - 2;
    hi_y = cy + r + 2;
    for (py = lo_y; py <= hi_y; py++) {
        for (px = lo_x; px <= hi_x; px++) {
            if (corner == 0) {
                if (px > cx || py > cy) continue;
            } else if (corner == 1) {
                if (px < cx || py > cy) continue;
            } else if (corner == 2) {
                if (px < cx || py < cy) continue;
            } else {
                if (px > cx || py < cy) continue;
            }
            dist = (int)gfx_isqrt_u32((unsigned int)((px - cx) * (px - cx) + (py - cy) * (py - cy)));
            a = gfx_iabs(dist - r);
            if (a <= 2) {
                alpha = 255 - a * 100;
                if (alpha < 0) alpha = 0;
                if (alpha > 255) alpha = 255;
                gfx_blend_pixel(px, py, rgb, (unsigned int)alpha);
            }
        }
    }
}

void gfx_rounded_rect_stroke_aa(int x, int y, int w, int h, int corner_r, unsigned int rgb) {
    int r = corner_r;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (w < 2 || h < 2) return;
    gfx_wu_line(x + r, y, x + w - 1 - r, y, rgb);
    gfx_wu_line(x + r, y + h - 1, x + w - 1 - r, y + h - 1, rgb);
    gfx_wu_line(x, y + r, x, y + h - 1 - r, rgb);
    gfx_wu_line(x + w - 1, y + r, x + w - 1, y + h - 1 - r, rgb);
    if (r > 0) {
        gfx_arc_quarter_outline_aa(x + r, y + r, r, rgb, 0);
        gfx_arc_quarter_outline_aa(x + w - r, y + r, r, rgb, 1);
        gfx_arc_quarter_outline_aa(x + w - r, y + h - r, r, rgb, 2);
        gfx_arc_quarter_outline_aa(x + r, y + h - r, r, rgb, 3);
    }
}

void gfx_draw_image_rgba(int x, int y, int w, int h, const unsigned int* argb) {
    int ix, iy;
    unsigned int px;
    unsigned char a, rr, gg, bb;
    unsigned int dst, dr, dg, db, nr, ng, nb;
    if (!backbuf || w <= 0 || h <= 0) return;
    for (iy = 0; iy < h; iy++) {
        for (ix = 0; ix < w; ix++) {
            px = argb[iy * w + ix];
            a = (unsigned char)(px >> 24);
            if (a == 0) continue;
            rr = (unsigned char)(px >> 16);
            gg = (unsigned char)(px >> 8);
            bb = (unsigned char)px;
            if ((unsigned)(x + ix) >= (unsigned)scr_w || (unsigned)(y + iy) >= (unsigned)scr_h) continue;
            if (a >= 255) {
                gfx_put_pixel(x + ix, y + iy, (unsigned int)rr << 16 | (unsigned int)gg << 8 | bb);
                continue;
            }
            dst = gfx_get_pixel(x + ix, y + iy);
            dr = (dst >> 16) & 0xFF;
            dg = (dst >> 8) & 0xFF;
            db = dst & 0xFF;
            nr = (rr * a + dr * (255 - a)) / 255;
            ng = (gg * a + dg * (255 - a)) / 255;
            nb = (bb * a + db * (255 - a)) / 255;
            gfx_put_pixel(x + ix, y + iy, ((unsigned)nr << 16) | ((unsigned)ng << 8) | (unsigned)nb);
        }
    }
}

static void gfx_draw_char_hq_cell(int x, int y, unsigned char ch, unsigned int fg) {
    const unsigned char* gl = font8x8_get(ch);
    int sy, sx, acc, u, v, gx, gy, dim;
    dim = FONT_HQ_GLYPH_PX;
    if (!backbuf) return;
    for (sy = 0; sy < dim; sy++) {
        for (sx = 0; sx < dim; sx++) {
            acc = 0;
            for (u = 0; u < 2; u++) {
                for (v = 0; v < 2; v++) {
                    gx = (sx * 8 + u * 4 + 2) / dim;
                    gy = (sy * 8 + v * 4 + 2) / dim;
                    if (gx > 7) gx = 7;
                    if (gy > 7) gy = 7;
                    acc += ((gl[gy] >> gx) & 1);
                }
            }
            if (acc == 0) continue;
            if ((unsigned)(x + sx) >= (unsigned)scr_w || (unsigned)(y + sy) >= (unsigned)scr_h) continue;
            if (acc == 4)
                gfx_put_pixel(x + sx, y + sy, fg);
            else
                gfx_blend_pixel(x + sx, y + sy, fg, (unsigned int)((acc * 255) / 4));
        }
    }
}

void gfx_draw_text_hq(int x, int y, const char* s, unsigned int fg) {
    int ox = x, yy = y;
    int pad = (FONT_HQ_CELL_H - FONT_HQ_GLYPH_PX) / 2;
    for (; *s; s++) {
        if (*s == '\n') {
            x = ox;
            yy += FONT_HQ_CELL_H + 4;
            continue;
        }
        gfx_draw_char_hq_cell(x, yy + pad, (unsigned char)*s, fg);
        x += FONT_HQ_CELL_W;
    }
}

int gfx_text_width_hq(const char* s) {
    int n = 0;
    for (; *s; s++) {
        if (*s != '\n') n += FONT_HQ_CELL_W;
    }
    return n;
}

void gfx_drop_shadow_soft(int x, int y, int w, int h, int corner_r, int spread) {
    int s, a;
    (void)corner_r;
    if (spread < 4) spread = 4;
    if (spread > 14) spread = 14;
    for (s = spread; s >= 1; s--) {
        a = 18 + s * 5;
        if (a > 200) a = 200;
        gfx_blend_rect(x + s, y + s, w, h, RGB(0, 0, 0), (unsigned int)a);
    }
}

static int gfx_point_in_rr(int px, int py, int rx, int ry, int rw, int rh, int rad) {
    int rr = rad;
    if (rr > rh / 2) rr = rh / 2;
    if (rr > rw / 2) rr = rw / 2;
    if (px < rx || py < ry || px >= rx + rw || py >= ry + rh) return 0;
    if (px >= rx + rr && px < rx + rw - rr) return 1;
    if (py >= ry + rr && py < ry + rh - rr) return 1;
    if (px < rx + rr && py < ry + rr) {
        int dx = px - (rx + rr), dy = py - (ry + rr);
        return dx * dx + dy * dy <= rr * rr;
    }
    if (px >= rx + rw - rr && py < ry + rr) {
        int dx = px - (rx + rw - rr), dy = py - (ry + rr);
        return dx * dx + dy * dy <= rr * rr;
    }
    if (px < rx + rr && py >= ry + rh - rr) {
        int dx = px - (rx + rr), dy = py - (ry + rh - rr);
        return dx * dx + dy * dy <= rr * rr;
    }
    if (px >= rx + rw - rr && py >= ry + rh - rr) {
        int dx = px - (rx + rw - rr), dy = py - (ry + rh - rr);
        return dx * dx + dy * dy <= rr * rr;
    }
    return 1;
}

void gfx_rect_mica(int x, int y, int w, int h, int corner_r, unsigned int tint, unsigned int glass_alpha) {
    int j, i, px, py, rr, gg, bb, tr, tg, tb;
    unsigned int c0, c1, c2, c3, cr, cg, cb;
    tr = (tint >> 16) & 0xFF;
    tg = (tint >> 8) & 0xFF;
    tb = tint & 0xFF;
    if (glass_alpha > 220) glass_alpha = 220;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            px = x + i;
            py = y + j;
            if (!gfx_point_in_rr(px, py, x, y, w, h, corner_r)) continue;
            c0 = gfx_get_pixel(px, py);
            c1 = px + 1 < scr_w ? gfx_get_pixel(px + 1, py) : c0;
            c2 = py + 1 < scr_h ? gfx_get_pixel(px, py + 1) : c0;
            c3 = (px + 1 < scr_w && py + 1 < scr_h) ? gfx_get_pixel(px + 1, py + 1) : c0;
            cr = ((c0 >> 16) & 0xFF) + ((c1 >> 16) & 0xFF) + ((c2 >> 16) & 0xFF) + ((c3 >> 16) & 0xFF);
            cg = ((c0 >> 8) & 0xFF) + ((c1 >> 8) & 0xFF) + ((c2 >> 8) & 0xFF) + ((c3 >> 8) & 0xFF);
            cb = (c0 & 0xFF) + (c1 & 0xFF) + (c2 & 0xFF) + (c3 & 0xFF);
            cr /= 4;
            cg /= 4;
            cb /= 4;
            cr = (cr * (255 - (int)glass_alpha) + tr * (int)glass_alpha) / 255;
            cg = (cg * (255 - (int)glass_alpha) + tg * (int)glass_alpha) / 255;
            cb = (cb * (255 - (int)glass_alpha) + tb * (int)glass_alpha) / 255;
            rr = cr;
            gg = cg;
            bb = cb;
            gfx_put_pixel(px, py, ((unsigned)rr << 16) | ((unsigned)gg << 8) | (unsigned)bb);
        }
    }
}

/* ── Blit escalado (nearest-neighbor) ───────────────────────────────────── */
/*
 * Copia la imagen src (sw×sh píxeles, formato 0xAARRGGBB) al backbuffer
 * escalando al rectángulo de destino (dx,dy,dw,dh) con interpolación de
 * vecino más cercano.  Útil para pintar wallpapers y sprites de cualquier
 * tamaño sin reservas adicionales de memoria.
 */
void gfx_blit_scaled(int dx, int dy, int dw, int dh,
                     const uint32_t* src, int sw, int sh) {
    int y, x;
    if (!backbuf || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    for (y = 0; y < dh; y++) {
        int abs_y = dy + y;
        int sy    = y * sh / dh;
        if (abs_y < 0 || abs_y >= scr_h) continue;

        const uint32_t* srow = src + (size_t)(unsigned)sy * (size_t)(unsigned)sw;
        uint32_t* drow = backbuf + (size_t)(unsigned)abs_y * (size_t)(unsigned)fb_stride;

        for (x = 0; x < dw; x++) {
            int abs_x = dx + x;
            int sx    = x * sw / dw;
            if (abs_x < 0 || abs_x >= scr_w) continue;
            drow[abs_x] = srow[sx];
        }
    }
}

/* ── Mouse cursor (12x16 arrow with shadow) ──────────────────────── */
void gfx_draw_cursor(int cx, int cy) {
    static const unsigned short shape[16] = {
        0x8000, 0xC000, 0xE000, 0xF000,
        0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xF800, 0xF800, 0xD800, 0x8C00,
        0x0C00, 0x0600, 0x0600, 0x0000
    };
    static const unsigned short inner[16] = {
        0x0000, 0x4000, 0x6000, 0x7000,
        0x7800, 0x7C00, 0x7E00, 0x7F00,
        0x7800, 0x7000, 0x5800, 0x0400,
        0x0400, 0x0200, 0x0000, 0x0000
    };
    /* Shadow pass */
    for(int r = 0; r < 16; r++)
        for(int c = 0; c < 12; c++)
            if((shape[r] >> (15-c)) & 1)
                gfx_put_pixel(cx+c+1, cy+r+1, COL_SHADOW);
    /* White outline */
    for(int r = 0; r < 16; r++)
        for(int c = 0; c < 12; c++)
            if((shape[r] >> (15-c)) & 1)
                gfx_put_pixel(cx+c, cy+r, COL_WHITE);
    /* Accent inner */
    for(int r = 0; r < 16; r++)
        for(int c = 0; c < 12; c++)
            if((inner[r] >> (15-c)) & 1)
                gfx_put_pixel(cx+c, cy+r, COL_ACCENT);
}
