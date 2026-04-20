#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "boot_info.h"

/* Alias histórico — misma estructura que NexusBootInfo @ NEXUS_BOOT_INFO_PHYS. */
typedef NexusBootInfo VesaBootInfo;
#define NEXUS_VBE_HANDOFF_MAGIC NEXUS_BOOT_HANDOFF_MAGIC

/* Resolución física actual (tras gfx_init_vesa / gfx_init_vga). */
extern int screen_width;
extern int screen_height;

/* Motor de layout: derivado solo de screen_width/height (VBE). */
extern int layout_ui_scale; /* min(w,h) — base para iconos y proporciones */
extern int layout_top_h;
extern int layout_dock_w, layout_dock_h, layout_dock_x, layout_dock_y;
extern int layout_dock_margin_bottom;
extern int layout_dock_r;
extern int layout_icon_size;
extern int layout_slot_sp;
extern int layout_chrome_title_h;
extern int layout_chrome_corner_r;

/* Recalcula layout (llamar tras fijar screen_*). */
void gfx_layout_refresh(void);

/* Borde superior del dock (ventanas: y + h <= esto). */
#define LAYOUT_WORK_BOTTOM layout_dock_y

/* Read VESA info placed by bootloader. Returns 1 if valid. */
int  gfx_vesa_detect(VesaBootInfo* out);

/* Initialize with given framebuffer params. Allocates backbuffer. */
void gfx_init_vesa(uint64_t lfb_phys, int w, int h, int pitch, int bpp);

/* Fallback: old VGA 320x200 8-bit init */
void gfx_init_vga(void);

int  gfx_width(void);
int  gfx_height(void);

/* Core primitives — all work on backbuffer */
void gfx_clear(unsigned int color);
/* Rellena el backbuffer/LFB activo (scr_w×scr_h, stride real). Prueba de vida. */
void gfx_fill_screen_solid(unsigned int rgb);
void gfx_put_pixel(int x, int y, unsigned int color);
void gfx_fill_rect(int x, int y, int w, int h, unsigned int color);
void gfx_draw_rect(int x, int y, int w, int h, unsigned int color);
void gfx_hline(int x, int y, int w, unsigned int color);
void gfx_vline(int x, int y, int h, unsigned int color);
void gfx_fill_rounded_rect(int x, int y, int w, int h, int r, unsigned int color);

/* Text (8x8 bitmap font) */
void gfx_draw_char(int x, int y, unsigned char ch, unsigned int fg, unsigned int bg);
void gfx_draw_text(int x, int y, const char* s, unsigned int fg, unsigned int bg);
void gfx_draw_text_transparent(int x, int y, const char* s, unsigned int fg);
int  gfx_text_width(const char* s);

/* Texto escalado x2 con suavizado (16px/glyph): más nítido que 8x8 crudo escalado */
void gfx_draw_char_aa(int x, int y, unsigned char ch, unsigned int fg, int scale);
void gfx_draw_text_aa(int x, int y, const char* s, unsigned int fg, int scale);
int  gfx_text_width_aa(const char* s, int scale);

void gfx_blend_rect(int x, int y, int w, int h, unsigned int fg, unsigned int alpha);

/* Gradient rectangle (vertical) */
void gfx_gradient_v(int x, int y, int w, int h, unsigned int top_col, unsigned int bot_col);

/* Gradiente diagonal (esquina sup-izq -> inf-der) */
void gfx_gradient_diagonal(int x, int y, int w, int h, unsigned int c_tl, unsigned int c_br);

unsigned int gfx_get_pixel(int x, int y);
void gfx_blend_pixel(int x, int y, unsigned int fg, unsigned int alpha /* 0-255 */);

void gfx_fill_circle(int cx, int cy, int r, unsigned int color);

/* Xiaolin Wu — líneas y contornos con antialiasing (canal alfa en bordes) */
void gfx_wu_line(int x0, int y0, int x1, int y1, unsigned int rgb);
void gfx_circle_outline_aa(int cx, int cy, int r, unsigned int rgb);
void gfx_rounded_rect_stroke_aa(int x, int y, int w, int h, int corner_r, unsigned int rgb);

/* RGBA 0xAARRGGBB por píxel, fila mayor a menor (top-left origin) */
void gfx_draw_image_rgba(int x, int y, int w, int h, const unsigned int* argb);

/*
 * Nearest-neighbor scale-blit: copia src (sw×sh, 0xAARRGGBB) al backbuffer
 * escalando a (dx,dy,dw,dh).  Ideal para wallpapers desde el VFS.
 */
void gfx_blit_scaled(int dx, int dy, int dw, int dh,
                     const uint32_t* src, int sw, int sh);

/* Texto HQ (16×24 celda): supersampling + subpixel ligero */
void gfx_draw_text_hq(int x, int y, const char* s, unsigned int fg);
int  gfx_text_width_hq(const char* s);

/* Sombra difusa (drop shadow) y “mica” sobre fondo ya dibujado */
void gfx_drop_shadow_soft(int x, int y, int w, int h, int corner_r, int spread);
void gfx_rect_mica(int x, int y, int w, int h, int corner_r, unsigned int tint, unsigned int glass_alpha);

/* Mouse cursor */
void gfx_draw_cursor(int cx, int cy);

/* Volcado rápido double_buffer → LFB (copia por filas en bloques de 64 bits). */
void gfx_swap_buffers(void);

/* Alias histórico — llama a gfx_swap_buffers. */
void gfx_present(void);

/* Enlaza un búfer RAM ya reservado (p. ej. kmalloc(pitch*height)); present copia al LFB. */
void gfx_attach_double_buffer(uint32_t* buf);

/* Reserva backbuffer con kmalloc y copia al LFB en present (sin parpadeo directo) */
void gfx_enable_double_buffer_kmalloc(void);

/* Return to text mode */
void gfx_shutdown_to_text(void);

/* ── Color helpers (píxel 0x00RRGGBB; imágenes gfx_draw_image_rgba: 0xAARRGGBB) ─ */
#define RGB(r,g,b)   (((uint32_t)(uint8_t)(r)<<16)|((uint32_t)(uint8_t)(g)<<8)|(uint32_t)(uint8_t)(b))
#define ARGB(a,r,g,b) (((uint32_t)(uint8_t)(a)<<24)|RGB(r,g,b))
#define RGB_R(c)     ((uint8_t)(((uint32_t)(c)>>16)&0xFFu))
#define RGB_G(c)     ((uint8_t)(((uint32_t)(c)>>8)&0xFFu))
#define RGB_B(c)     ((uint8_t)((uint32_t)(c)&0xFFu))
#define ARGB_A(c)    ((uint8_t)(((uint32_t)(c)>>24)&0xFFu))
/* Mezcla lineal 0..255 entre dos RGB (sin alpha premultiplicado). */
uint32_t gfx_lerp_rgb(uint32_t a, uint32_t b, int t, int t_max);

/* NexusOS "Gaming Mode" palette */
#define COL_BG          RGB(12,12,18)
#define COL_TASKBAR     RGB(18,18,28)
#define COL_TASKBAR_HI  RGB(30,30,50)
#define COL_WIN_BG      RGB(25,25,38)
#define COL_TITLE_ACT   RGB(20,20,32)
#define COL_TITLE_INACT RGB(30,30,40)
#define COL_BORDER      RGB(50,50,70)
#define COL_ACCENT      RGB(0,150,210)
#define COL_GREEN       RGB(60,190,80)
#define COL_RED         RGB(200,55,55)
#define COL_YELLOW      RGB(220,180,50)
#define COL_ORANGE      RGB(230,130,40)
#define COL_SHADOW      RGB(5,5,10)
#define COL_MENU_BG     RGB(22,22,34)
#define COL_MENU_SEL    RGB(35,65,140)
#define COL_LGRAY       RGB(175,175,190)
#define COL_DGRAY       RGB(70,70,90)
#define COL_DIM         RGB(100,100,120)
#define COL_CYAN        RGB(0,180,220)
#define COL_WHITE       RGB(235,235,245)
#define COL_BLACK       RGB(0,0,0)
#define COL_URLBAR      RGB(40,40,55)
#define COL_CONTENT     RGB(240,240,245)

#endif
