/*
 * Doble búfer: kmalloc(pitch * height) + gfx_attach_double_buffer().
 * Doble búfer en RAM: compositor_render() solo repinta dirty rects; swap_buffers() copia al LFB
 * filas tocadas (memcpy_fast 64B) o pantalla completa si no hay marcas parciales.
 */
#include "gui.h"
#include "gfx.h"
#include "memory.h"
#include "vesa.h"
#include "teclado.h"
#include "mouse.h"
#include "nexus.h"
#include "window.h"
#include "desktop.h"
#include "mouse_gui.h"
#include "apps.h"
#include "top_panel.h"
#include "event.h"
#include "font_data.h"
#include "compositor.h"
#include "xhci.h"
#include "ui_manager.h"
#include <stdint.h>

extern volatile unsigned char tecla_nueva;
extern volatile uint64_t ticks;

volatile bool ui_needs_update = true;

void ui_mark_dirty(void) {
    ui_needs_update = true;
}

/* ── Doble búfer estricto: kmalloc(pitch×height), mismo stride que el LFB ── */
static uint32_t* gui_backbuffer;
static int gui_fb_w, gui_fb_h, gui_fb_pitch;
static uint32_t gui_stride_u32;

int gui_framebuffer_init_kmalloc(const VesaBootInfo* vbi) {
    uint64_t nbytes;
    uint32_t* buf;
    if (!vbi || vbi->bpp != 32u || vbi->pitch < 4u || vbi->width == 0u || vbi->height == 0u)
        return -1;
    nbytes = (uint64_t)vbi->pitch * (uint64_t)vbi->height;
    buf = (uint32_t*)kmalloc(nbytes);
    if (!buf)
        return -1;
    gfx_init_vesa(vbi->lfb_ptr, (int)vbi->width, (int)vbi->height, (int)vbi->pitch, (int)vbi->bpp);
    gfx_attach_double_buffer(buf);
    gui_backbuffer = buf;
    gui_fb_w = (int)vbi->width;
    gui_fb_h = (int)vbi->height;
    gui_fb_pitch = (int)vbi->pitch;
    gui_stride_u32 = (uint32_t)(vbi->pitch / 4u);
    return 0;
}

void gui_put_pixel(int x, int y, uint32_t rgb) {
    if (!gui_backbuffer || x < 0 || y < 0 || x >= gui_fb_w || y >= gui_fb_h)
        return;
    gui_backbuffer[(size_t)(unsigned)y * (size_t)gui_stride_u32 + (size_t)(unsigned)x] = rgb;
}

void gui_draw_rect(int x, int y, int w, int h, uint32_t rgb) {
    int x0, y0, x1, y1, i, j;
    if (!gui_backbuffer || w <= 0 || h <= 0)
        return;
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w > gui_fb_w ? gui_fb_w : x + w;
    y1 = y + h > gui_fb_h ? gui_fb_h : y + h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (j = y0; j < y1; j++) {
        uint32_t* row = gui_backbuffer + (size_t)(unsigned)j * (size_t)gui_stride_u32;
        for (i = x0; i < x1; i++)
            row[i] = rgb;
    }
}

void gui_draw_image_rect(int x, int y, int src_x, int src_y, int src_w, int src_h, const Image* img) {
    int ix, iy;
    if (!gui_backbuffer || !img || !img->pixels || src_w <= 0 || src_h <= 0)
        return;
    if (src_x < 0 || src_y < 0 || src_x + src_w > img->width || src_y + src_h > img->height)
        return;

    for (iy = 0; iy < src_h; iy++) {
        int dy = y + iy;
        if (dy < 0 || dy >= gui_fb_h)
            continue;
        for (ix = 0; ix < src_w; ix++) {
            int           dx = x + ix;
            uint32_t      fg;
            unsigned int  a;
            size_t        di;
            if (dx < 0 || dx >= gui_fb_w)
                continue;
            fg = img->pixels[(size_t)(src_y + iy) * (size_t)img->width + (size_t)(src_x + ix)];
            a  = (fg >> 24) & 0xFFu;
            if (a == 0u)
                continue;
            di = (size_t)(unsigned)dy * (size_t)gui_stride_u32 + (size_t)(unsigned)dx;
            if (a == 255u)
                gui_backbuffer[di] = fg & 0x00FFFFFFu;
            else
                gui_backbuffer[di] = gui_blend_colors(gui_backbuffer[di], fg) & 0x00FFFFFFu;
        }
    }
}

void gui_draw_image(int x, int y, const Image* img) {
    if (!img || img->width < 1 || img->height < 1)
        return;
    gui_draw_image_rect(x, y, 0, 0, img->width, img->height, img);
}

void gui_draw_image_stretch(int x, int y, int dst_w, int dst_h, const Image* img) {
    int dx, dy;
    if (!gui_backbuffer || !img || !img->pixels || dst_w < 1 || dst_h < 1)
        return;
    if (img->width < 1 || img->height < 1)
        return;

    for (dy = 0; dy < dst_h; dy++) {
        int sy = dy * img->height / dst_h;
        int py = y + dy;
        if (py < 0 || py >= gui_fb_h)
            continue;
        for (dx = 0; dx < dst_w; dx++) {
            int           sx = dx * img->width / dst_w;
            int           px = x + dx;
            uint32_t      fg;
            unsigned int  a;
            size_t        di;
            if (px < 0 || px >= gui_fb_w)
                continue;
            fg = img->pixels[(size_t)sy * (size_t)img->width + (size_t)sx];
            a  = (fg >> 24) & 0xFFu;
            if (a == 0u)
                continue;
            di = (size_t)(unsigned)py * (size_t)gui_stride_u32 + (size_t)(unsigned)px;
            if (a == 255u)
                gui_backbuffer[di] = fg & 0x00FFFFFFu;
            else
                gui_backbuffer[di] = gui_blend_colors(gui_backbuffer[di], fg) & 0x00FFFFFFu;
        }
    }
}

/* Paleta estilo Apple / iOS (ARGB). */
uint32_t COLOR_BG_WINDOW    = 0xFFF5F5F7u;
uint32_t COLOR_ACCENT       = 0xFF007AFFu;
uint32_t COLOR_TEXT_PRIMARY = 0xFF1D1D1Fu;
uint32_t COLOR_BORDER       = 0xFFE5E5EAu;
uint32_t COLOR_DESKTOP_BG   = 0xFF2D3138u;

static uint32_t gui_argb_opaque(uint32_t c) {
    return (c & 0x00FFFFFFu) | 0xFF000000u;
}

uint32_t gui_blend_colors(uint32_t bg, uint32_t fg) {
    /* gfx: convención alpha-over estándar (fg sobre bg). */
    return blend_colors(fg, bg);
}

void draw_rounded_rect_filled(int x, int y, int w, int h, int radius, uint32_t color) {
    int r = radius;
    if (w < 1 || h < 1)
        return;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r < 0)
        r = 0;
    gfx_fill_rounded_rect_aa(x, y, w, h, r, color);
}

/*
 * Rectángulo redondeado relleno (anti-aliasing vía motor gfx).
 * `color_argb` admite 0xAARRGGBB; se fuerza relleno opaco.
 */
void draw_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color_argb) {
    draw_rounded_rect_filled(x, y, w, h, radius, gui_argb_opaque(color_argb));
}

/*
 * Sombra: capas concéntricas (mismo radio, desplazamiento creciente), alpha
 * mayor cerca de la tarjeta y más suave hacia fuera.
 */
void draw_drop_shadow(int x, int y, int w, int h, int radius, int spread, uint32_t base_color) {
    int          br, bgc, bb, r, s;
    unsigned int a;
    uint32_t     argb;

    if (w < 1 || h < 1)
        return;
    if (spread < 2)
        spread = 2;
    if (spread > 56)
        spread = 56;

    br  = (int)((base_color >> 16) & 0xFFu);
    bgc = (int)((base_color >> 8) & 0xFFu);
    bb  = (int)(base_color & 0xFFu);

    r = radius;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r < 0)
        r = 0;

    for (s = spread; s >= 1; s--) {
        unsigned inv = (unsigned)(spread - s + 1);
        a = 10u + (unsigned)(215u * inv / (unsigned)spread);
        if (a > 238u)
            a = 238u;
        argb = ((uint32_t)a << 24) | ((uint32_t)(unsigned)br << 16) | ((uint32_t)(unsigned)bgc << 8)
             | (uint32_t)(unsigned)bb;
        {
            int ox = (s * 5) / 8;
            int oy = (s * 3) / 4;
            gfx_fill_rounded_rect_aa(x + ox, y + oy, w, h, r, argb);
        }
    }
}

/*
 * Sombra de elevación: capas semitransparentes desplazadas (simétrico al drop
 * shadow de GTK/Cocoa, sin depender de la forma redondeada del destino).
 */
void draw_shadow_rect(int x, int y, int w, int h) {
    int s;
    if (w < 1 || h < 1)
        return;
    for (s = 10; s >= 1; s--) {
        unsigned a = (unsigned)(12 + s * 7);
        if (a > 140)
            a = 140;
        gfx_blend_rect(x + s, y + s + 2, w, h, RGB(0, 0, 0), a);
    }
}

void swap_buffers(void) {
    gfx_swap_buffers();
}

void gui_render_frame(int sw, int sh, int mx, int my, int* dock_hover, int menu_o, int menu_s,
                      int ax, int ay) {
    compositor_cursor_moved(mx, my);
    compositor_render(sw, sh, mx, my, dock_hover);
    desktop_draw_app_menu(menu_o, menu_s, ax, ay);
    gfx_draw_cursor(mx, my);
    swap_buffers();
}

static int SW, SH;
static int desk_x, desk_y, desk_w, desk_h;

#define MAX_WIN 6
static NWM_Window wins[MAX_WIN];
static int  win_count, focus_id;
static int  menu_open, menu_sel;

static void z_raise(int id) {
    if (id >= 0 && id < win_count)
        nwm_dll_raise(&wins[id]);
}

static void win_show(int id) {
    int i;
    if (id < 0 || id >= win_count) return;
    for (i = 0; i < win_count; i++) wins[i].focused = 0;
    wins[id].focused = 1;
    wins[id].visible = 1;
    focus_id = id;
    z_raise(id);
    nwm_window_mark_dirty(&wins[id]);
    compositor_damage_rect_pad(wins[id].x, wins[id].y, wins[id].w, wins[id].h,
                               NEXUS_COMPOSITOR_SHADOW_PAD);
}

static void win_hide(int id) {
    int i;
    NWM_Window* wp;
    if (id < 0 || id >= win_count) return;
    if (wins[id].visible)
        compositor_damage_rect_pad(wins[id].x, wins[id].y, wins[id].w, wins[id].h,
                                   NEXUS_COMPOSITOR_SHADOW_PAD);
    wins[id].visible = 0;
    wins[id].focused = 0;
    if (focus_id == id) {
        focus_id = -1;
        for (wp = nwm_z_stack_top(); wp; wp = wp->z_prev) {
            i = (int)(wp - wins);
            if (i >= 0 && i < win_count && wins[i].visible) {
                win_show(i);
                return;
            }
        }
    }
}

static void draw_terminal(NWM_Window* w);
static void draw_files(NWM_Window* w);
static void draw_sysmon(NWM_Window* w);
static void draw_about(NWM_Window* w);
static void paint_client_for_win(NWM_Window* w);

static int slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void itoa_s(int n, char* b) {
    if (n <= 0) {
        b[0] = '0';
        b[1] = 0;
        return;
    }
    char t[12];
    int i = 0;
    while (n) {
        t[i++] = '0' + (n % 10);
        n /= 10;
    }
    int j = 0;
    for (int k = i - 1; k >= 0; k--) b[j++] = t[k];
    b[j] = 0;
}

static void draw_terminal(NWM_Window* w) {
    int th = layout_chrome_title_h;
    int ax = w->x + 2, ay = w->y + th + 2, aw = w->w - 4, ah = w->h - th - 4;
    int x2, y2, row, vp;
    char vbuf[40];
    char tmp[12];

    gfx_fill_rect(ax, ay, aw, ah, RGB(30, 34, 48));
    gfx_blend_rect(ax, ay, aw, ah, RGB(12, 18, 32), 120);
    for (row = 0; row < ah; row += 3)
        gfx_blend_rect(ax, ay + row, aw, 1, RGB(0, 0, 0), 22);

    x2 = ax + 12;
    y2 = ay + 12;
    gfx_draw_text_aa(x2, y2, "user@nexusos:~$ neofetch", RGB(72, 225, 130), 2);
    y2 += 22;
    gfx_hline(x2, y2 + 6, aw - 24, RGB(70, 110, 130));
    y2 += 16;
    gfx_draw_text_aa(x2, y2, "OS:     NexusOS 3.0 Gaming x86_64", RGB(190, 195, 210), 2);
    y2 += 22;
    gfx_draw_text_aa(x2, y2, "Kernel: 3.0.0-nexus", RGB(190, 195, 210), 2);
    y2 += 22;
    vp = 0;
    vbuf[vp++] = 'D';
    vbuf[vp++] = 'E';
    vbuf[vp++] = ':';
    vbuf[vp++] = ' ';
    vbuf[vp++] = ' ';
    vbuf[vp++] = ' ';
    vbuf[vp++] = 'N';
    vbuf[vp++] = 'e';
    vbuf[vp++] = 'x';
    vbuf[vp++] = 'u';
    vbuf[vp++] = 's';
    vbuf[vp++] = 'D';
    vbuf[vp++] = 'E';
    vbuf[vp++] = ' ';
    itoa_s(SW, tmp);
    for (int i = 0; tmp[i]; i++) vbuf[vp++] = tmp[i];
    vbuf[vp++] = 'x';
    itoa_s(SH, tmp);
    for (int i = 0; tmp[i]; i++) vbuf[vp++] = tmp[i];
    vbuf[vp] = 0;
    gfx_draw_text_aa(x2, y2, vbuf, RGB(190, 195, 210), 2);
    y2 += 22;
    gfx_draw_text_aa(x2, y2, "Shell:  /bin/nexusbash", RGB(190, 195, 210), 2);
    y2 += 22;
    gfx_draw_text_aa(x2, y2, "WM:     Nexus Window Manager", RGB(190, 195, 210), 2);
    y2 += 22;
    gfx_draw_text_aa(x2, y2, "Memory: ~1MB / 512MB", RGB(190, 195, 210), 2);
    y2 += 26;
    if ((ticks / 400) & 1)
        gfx_draw_text_aa(x2, y2, "user@nexusos:~$ _", RGB(72, 225, 130), 2);
    else
        gfx_draw_text_aa(x2, y2, "user@nexusos:~$", RGB(72, 225, 130), 2);
}

static void draw_files(NWM_Window* w) {
    int th = layout_chrome_title_h;
    int ax = w->x + 2, ay = w->y + th + 2, aw = w->w - 4, ah = w->h - th - 4;
    int sy, sw, fx, fy, fw, maxf, i;

    gfx_fill_rect(ax, ay, aw, 28, RGB(44, 44, 44));
    gfx_draw_text(ax + 10, ay + 10, "< > ^", COL_DIM, RGB(44, 44, 44));
    gfx_fill_rect(ax + 60, ay + 6, aw - 80, 16, RGB(55, 55, 55));
    gfx_draw_text(ax + 68, ay + 10, "/home/user", COL_WHITE, RGB(55, 55, 55));
    sy = ay + 30;
    sw = 120;
    gfx_fill_rect(ax, sy, sw, ah - 30, RGB(38, 38, 38));
    gfx_vline(ax + sw, sy, ah - 30, RGB(55, 55, 55));
    {
        static const char* pl[] = {"Home", "Desktop", "Documents", "Downloads", "Pictures"};
        for (i = 0; i < 5; i++) {
            unsigned int bg = (i == 0) ? RGB(55, 55, 70) : RGB(38, 38, 38);
            if (i == 0) gfx_fill_rect(ax + 2, sy + 4 + i * 22, sw - 4, 20, bg);
            gfx_draw_text(ax + 12, sy + 8 + i * 22, pl[i], (i == 0) ? COL_WHITE : COL_LGRAY, (i == 0) ? bg : RGB(38, 38, 38));
        }
    }
    fx = ax + sw + 8;
    fy = sy + 8;
    fw = aw - sw - 16;
    gfx_draw_text(fx, fy, "Name", COL_DIM, RGB(38, 38, 38));
    gfx_draw_text(fx + fw - 40, fy, "Size", COL_DIM, RGB(38, 38, 38));
    gfx_hline(fx, fy + 12, fw, RGB(55, 55, 55));
    fy += 18;
    {
        static const char* fn[] = {"docs/", "etc/", "bin/", "home/", "readme.txt", "nexus.cfg"};
        static const int fd[] = {1, 1, 1, 1, 0, 0};
        static const char* fs[] = {"4 KB", "4 KB", "4 KB", "4 KB", "512 B", "256 B"};
        maxf = (ah - 60) / 22;
        if (maxf > 6) maxf = 6;
        for (i = 0; i < maxf; i++) {
            if (fd[i]) {
                gfx_fill_rect(fx, fy + 2, 12, 10, COL_YELLOW);
                gfx_fill_rect(fx, fy + 2, 6, 3, RGB(180, 140, 30));
            } else
                gfx_fill_rect(fx + 1, fy + 1, 10, 12, RGB(70, 70, 90));
            gfx_draw_text(fx + 18, fy + 3, fn[i], fd[i] ? COL_CYAN : COL_LGRAY, RGB(38, 38, 38));
            gfx_draw_text(fx + fw - 40, fy + 3, fs[i], COL_DIM, RGB(38, 38, 38));
            fy += 22;
        }
    }
}

static void draw_sysmon(NWM_Window* w) {
    int th = layout_chrome_title_h;
    int ax = w->x + 2, ay = w->y + th + 2, aw = w->w - 4;
    int y2 = ay + 10;
    int bx, bw, cp;
    char pb[8];
    char tb[8];
    unsigned int sec, mn, hr;
    int ox;

    gfx_draw_text(ax + 10, y2, "System Monitor", COL_ACCENT, NWM_COL_BODY);
    y2 += 22;
    bx = ax + 60;
    bw = aw - 130;
    if (bw < 40) bw = 40;
    gfx_draw_text(ax + 10, y2, "CPU", COL_LGRAY, NWM_COL_BODY);
    gfx_fill_rect(bx, y2, bw, 12, RGB(25, 25, 35));
    gfx_draw_rect(bx, y2, bw, 12, RGB(55, 55, 55));
    cp = (int)((ticks % 3000) / 30);
    if (cp > 100) cp = 200 - cp;
    gfx_fill_rect(bx + 1, y2 + 1, (bw - 2) * cp / 100, 10, COL_GREEN);
    itoa_s(cp, pb);
    gfx_draw_text(bx + bw + 6, y2 + 2, pb, COL_DIM, NWM_COL_BODY);
    gfx_draw_text(bx + bw + 6 + slen(pb) * 8, y2 + 2, "%", COL_DIM, NWM_COL_BODY);
    y2 += 24;
    gfx_draw_text(ax + 10, y2, "RAM", COL_LGRAY, NWM_COL_BODY);
    gfx_fill_rect(bx, y2, bw, 12, RGB(25, 25, 35));
    gfx_draw_rect(bx, y2, bw, 12, RGB(55, 55, 55));
    gfx_fill_rect(bx + 1, y2 + 1, (bw - 2) * 3 / 100, 10, COL_CYAN);
    gfx_draw_text(bx + bw + 6, y2 + 2, "3%", COL_DIM, NWM_COL_BODY);
    y2 += 30;
    gfx_hline(ax + 10, y2, aw - 20, RGB(55, 55, 55));
    y2 += 10;
    sec = ticks / 1000;
    mn = sec / 60;
    hr = mn / 60;
    sec %= 60;
    mn %= 60;
    gfx_draw_text(ax + 10, y2, "Uptime:", COL_DIM, NWM_COL_BODY);
    itoa_s((int)hr, tb);
    gfx_draw_text(ax + 80, y2, tb, COL_WHITE, NWM_COL_BODY);
    ox = 80 + slen(tb) * 8;
    gfx_draw_text(ax + ox, y2, "h ", COL_DIM, NWM_COL_BODY);
    ox += 16;
    itoa_s((int)mn, tb);
    gfx_draw_text(ax + ox, y2, tb, COL_WHITE, NWM_COL_BODY);
    y2 += 20;
    gfx_draw_text(ax + 10, y2, "Arch:  x86_64 Long Mode", COL_LGRAY, NWM_COL_BODY);
}

static void draw_about(NWM_Window* w) {
    int th = layout_chrome_title_h;
    int ax = w->x + 2, ay = w->y + th + 2, aw = w->w - 4;
    int y2 = ay + 16;

    gfx_gradient_v(ax + 20, y2, aw - 40, 44, RGB(44, 10, 64), RGB(80, 20, 120));
    gfx_draw_text_transparent(ax + aw / 2 - 28, y2 + 10, "NexusOS", COL_WHITE);
    gfx_draw_text_transparent(ax + aw / 2 - 60, y2 + 26, "Gaming Edition 3.0", RGB(200, 170, 255));
    y2 += 56;
    gfx_draw_text(ax + 20, y2, "Bare-metal x86_64 OS for gaming.", COL_LGRAY, NWM_COL_BODY);
    y2 += 14;
    gfx_draw_text(ax + 20, y2, "Built from scratch in C + ASM.", COL_LGRAY, NWM_COL_BODY);
    y2 += 20;
    gfx_draw_text(ax + 20, y2, "Kernel:  Monolithic, Long Mode", COL_DIM, NWM_COL_BODY);
    y2 += 14;
    gfx_draw_text(ax + 20, y2, "Net:     NIC detected, no TCP/IP", COL_DIM, NWM_COL_BODY);
}

static void paint_client_for_win(NWM_Window* w) {
    if (!w) return;
    switch (w->app_type) {
        case 0: draw_terminal(w); break;
        case 1: draw_files(w); break;
        case 2: apps_draw_nexus_firefox(w); break;
        case 3: draw_sysmon(w); break;
        case 4: draw_about(w); break;
        default: break;
    }
}

#define MENU_N 6
static const int menu_win_id[MENU_N] = {0, 1, 2, 3, 4, -1};
static const int dock_win_id[6] = {1, 0, 2, 3, 4, -1};
#define DOCK_ITEMS 6

void start_gui(void) {
    gui_run();
}

void gui_run(void) {
    VesaBootInfo vbi;
    int has_vesa = gfx_vesa_detect(&vbi);
    int running = 1;
    uint64_t last_frame = 0;
    int dock_hover;
    int lx, ly, lw, lh, anchor_x = 0, anchor_y = 0;

    if (has_vesa) {
        if (vbi.bpp == 32u && vbi.pitch > 0u && vbi.height > 0u) {
            if (gui_framebuffer_init_kmalloc(&vbi) != 0)
                kheap_panic_nomem("gui: double buffer (pitch*height)");
        } else {
            gfx_init_vesa(vbi.lfb_ptr, vbi.width, vbi.height, vbi.pitch, vbi.bpp);
        }
        if (vesa_console_active)
            vesa_force_refresh();
        (void)mouse_init((int32_t)vbi.width, (int32_t)vbi.height);
        xhci_set_screen_dims((int)vbi.width, (int)vbi.height);
    } else {
        gfx_init_vga();
        (void)mouse_init(320, 200);
        xhci_set_screen_dims(320, 200);
    }

    SW = gfx_width();
    SH = gfx_height();
    gfx_layout_refresh();
    desk_x = 0;
    desk_y = layout_top_h;
    desk_w = SW;
    desk_h = layout_dock_y - layout_top_h;
    if (desk_h < 1) desk_h = 1;

    desktop_wm_init();

    vesa_console_active = 0;

    win_count = 0;
    focus_id = -1;
    menu_open = 0;

    {
        int desk_inner_h = layout_dock_y - layout_top_h;
        if (desk_inner_h < 1) desk_inner_h = 1;
        int w0 = SW * 38 / 100;
        int h0 = desk_inner_h * 42 / 100;
        int w_fx = SW * 52 / 100;
        int h_fx = desk_inner_h * 48 / 100;
        int w3 = SW * 32 / 100;
        int h3 = desk_inner_h * 38 / 100;
        int w4 = SW * 30 / 100;
        int h4 = desk_inner_h * 35 / 100;
        int st = SW / 28;
        if (st < 20) st = 20;
        if (st > 56) st = 56;
        if (w0 < 200) w0 = 200;
        if (w0 > 560) w0 = 560;
        if (w0 > SW - 32) w0 = SW - 32;
        if (h0 < 160) h0 = 160;
        if (h0 > 420) h0 = 420;
        if (h0 > desk_inner_h - 24) h0 = desk_inner_h - 24;
        if (w_fx < 280) w_fx = 280;
        if (w_fx > SW - 32) w_fx = SW - 32;
        if (h_fx < 200) h_fx = 200;
        if (h_fx > desk_inner_h - 24) h_fx = desk_inner_h - 24;
        if (w3 < 220) w3 = 220;
        if (w3 > SW - 32) w3 = SW - 32;
        if (h3 < 160) h3 = 160;
        if (h3 > desk_inner_h - 24) h3 = desk_inner_h - 24;
        if (h4 < 160) h4 = 160;
        if (h4 > desk_inner_h - 24) h4 = desk_inner_h - 24;
        if (w4 > SW - 32) w4 = SW - 32;
        {
            int cx0 = (SW - w0) / 2;
            int cy0 = layout_top_h + (desk_inner_h - h0) / 2;
            wins[0] = (NWM_Window){cx0 - st, cy0 - st / 2, w0, h0, 0, 0, 0, 0, 0, "Terminal", 0,
                                   NULL, 0, 0, 0, 1, NULL, NULL};
            wins[1] = (NWM_Window){cx0 + st, cy0 + st / 2, w0, h0, 0, 0, 0, 0, 0, "Files", 1,
                                   NULL, 0, 0, 0, 1, NULL, NULL};
            wins[2] = (NWM_Window){(SW - w_fx) / 2, layout_top_h + (desk_inner_h - h_fx) / 2 + st,
                                   w_fx, h_fx, 0, 0, 0, 0, 0, "Nexus Firefox", 2,
                                   NULL, 0, 0, 0, 1, NULL, NULL};
            wins[3] = (NWM_Window){(SW - w3) / 2 + st * 2, layout_top_h + (desk_inner_h - h3) / 3,
                                   w3, h3, 0, 0, 0, 0, 0, "System Monitor", 3,
                                   NULL, 0, 0, 0, 1, NULL, NULL};
            wins[4] = (NWM_Window){(SW - w4) / 2 - st, layout_top_h + (2 * desk_inner_h) / 3 - h4 / 2,
                                   w4, h4, 0, 0, 0, 0, 0, "About NexusOS", 4,
                                   NULL, 0, 0, 0, 1, NULL, NULL};
        }
        win_count = 5;
        for (int wi = 0; wi < win_count; wi++) {
            if (wins[wi].x < 4) wins[wi].x = 4;
            if (wins[wi].y < layout_top_h + 2) wins[wi].y = layout_top_h + 2;
            if (wins[wi].x + wins[wi].w > SW - 4) wins[wi].x = SW - wins[wi].w - 4;
            if (wins[wi].y + wins[wi].h > LAYOUT_WORK_BOTTOM - 4)
                wins[wi].y = LAYOUT_WORK_BOTTOM - wins[wi].h - 4;
        }
    }

    nwm_dll_init_ring(wins, win_count);

    wins[0].visible = 1;
    wins[1].visible = 1;
    for (int i = 0; i < win_count; i++) wins[i].focused = 0;
    wins[0].focused = 1;
    focus_id = 0;
    nwm_dll_raise(&wins[0]);

    {
        int stride_u = gui_stride_u32 > 0 ? (int)gui_stride_u32 : SW;
        compositor_init(SW, SH, stride_u);
        compositor_bake_wallpaper_layer();
        compositor_mark_full();
    }

    {
        KbdState kbd;
        kbd_init(&kbd);

    /* Descartar eventos acumulados durante el arranque del escritorio. */
    flush_events();

    while (running) {
        if (ticks - last_frame < 2) {
            __asm__ volatile("hlt");
            continue;
        }
        last_frame = ticks;

        /* ════════════════════════════════════════════════════════════════
         * BUCLE DE MENSAJES — drena TODOS los Event antes de renderizar.
         *
         * IRQ1/12 y xhci_poll solo encolan os_event_t; pop_event traduce.
         * Ratón: MOUSE_MOVE / MOUSE_CLICK. Teclado: KEY_PRESS se ignora aquí;
         * la entrada de texto sigue por tecla_nueva + KbdState.
         * ════════════════════════════════════════════════════════════════ */
        {
            Event nev;
            while (pop_event(&nev)) {
                switch (nev.type) {

                /* ── Movimiento: arrastrar ventana si hay drag activo ─── */
                case EVENT_MOUSE_MOVE:
                    if (installer_win.is_visible && ui_element_count > 0)
                        ui_manager_update_hover(nev.mouse_x, nev.mouse_y);
                    if (nev.mouse_buttons & 1) {
                        int i;
                        for (i = 0; i < win_count; i++) {
                            if (wins[i].dragging) {
                                nwm_apply_window_drag(&wins[i],
                                    nev.mouse_x, nev.mouse_y,
                                    layout_top_h, LAYOUT_WORK_BOTTOM);
                                break;
                            }
                        }
                    }
                    break;

                /* ── Clic / soltar ──────────────────────────────────── */
                case EVENT_MOUSE_CLICK:
                    if (!nev.mouse_pressed) {
                        /* Soltar: cancelar arrastres. */
                        int i;
                        for (i = 0; i < win_count; i++) wins[i].dragging = 0;
                        break;
                    }
                    if (!(nev.mouse_buttons & 1))
                        break;  /* solo botón izquierdo */

                    /* Instalador: si el clic golpea un widget, no propagar al dock. */
                    if (installer_win.is_visible && ui_element_count > 0
                        && ui_manager_handle_primary_click(nev.mouse_x, nev.mouse_y))
                        break;

                    {
                        int dh = desktop_dock_hit(SW, SH, nev.mouse_x, nev.mouse_y);
                        if (menu_open) {
                            if (desktop_menu_contains(anchor_x, anchor_y,
                                                      nev.mouse_x, nev.mouse_y)) {
                                int sel = desktop_menu_hit(anchor_x, anchor_y,
                                                           nev.mouse_x, nev.mouse_y);
                                if (sel >= 0) {
                                    menu_open = 0;
                                    if (sel == MENU_N - 1) { running = 0; break; }
                                    { int wid = menu_win_id[sel];
                                      if (wid >= 0 && wid < win_count) win_show(wid); }
                                }
                            } else {
                                menu_open = 0;
                            }
                            break;
                        }
                        if (dh == -2) { menu_open = !menu_open; menu_sel = 0; break; }
                        if (dh >= 0 && dh < DOCK_ITEMS) {
                            if (dh == DOCK_ITEMS - 1) { running = 0; break; }
                            { int wid = dock_win_id[dh];
                              if (wid >= 0 && wid < win_count) {
                                if (wins[wid].visible && wins[wid].focused)
                                    win_hide(wid);
                                else
                                    win_show(wid);
                              }
                            }
                            break;
                        }
                        {
                            NWM_Window* wp;
                            for (wp = nwm_z_stack_top(); wp; wp = wp->z_prev) {
                                int id = (int)(wp - wins);
                                if (id < 0 || id >= win_count) continue;
                                if (!wins[id].visible) continue;
                                if (nev.mouse_x >= wins[id].x &&
                                    nev.mouse_x <  wins[id].x + wins[id].w &&
                                    nev.mouse_y >= wins[id].y &&
                                    nev.mouse_y <  wins[id].y + wins[id].h) {
                                    if (nwm_hit_close_button(&wins[id],
                                            nev.mouse_x, nev.mouse_y,
                                            layout_chrome_title_h)) {
                                        win_hide(id);
                                    } else if (nwm_hit_titlebar(&wins[id],
                                            nev.mouse_x, nev.mouse_y,
                                            layout_chrome_title_h)) {
                                        win_show(id);
                                        wins[id].dragging = 1;
                                        wins[id].drag_ox = nev.mouse_x - wins[id].x;
                                        wins[id].drag_oy = nev.mouse_y - wins[id].y;
                                    } else {
                                        win_show(id);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    break;

                case EVENT_KEY_PRESS:
                    if (installer_win.is_visible && ui_element_count > 0) {
                        if (nev.key_extended) {
                            if (nev.scancode == 0x50u || nev.scancode == 0x4Du) {
                                ui_focus_tab_next();
                                ui_manager_sync_focus_flags();
                                break;
                            }
                            if (nev.scancode == 0x48u || nev.scancode == 0x4Bu) {
                                ui_focus_tab_prev();
                                ui_manager_sync_focus_flags();
                                break;
                            }
                            break;
                        }
                        if (nev.scancode == 0x0Fu || nev.ascii == '\t') {
                            ui_focus_tab_next();
                            ui_manager_sync_focus_flags();
                            break;
                        }
                        if (nev.scancode == 0x1Cu) {
                            ui_activate_focused();
                            break;
                        }
                        if (focused_element_index >= 0
                            && focused_element_index < ui_element_count) {
                            UI_Element* fel = &ui_elements[focused_element_index];
                            if (fel->on_keypress) {
                                if (nev.ascii)
                                    fel->on_keypress(nev.ascii);
                                break;
                            }
                            if (fel->type == UI_TYPE_TEXT_INPUT) {
                                if (nev.scancode == 0x0Eu)
                                    ui_handle_char('\b');
                                else if (nev.ascii >= 32)
                                    ui_handle_char((unsigned char)nev.ascii);
                            }
                        }
                    }
                    break;

                /* ── KEY_PRESS: drenar para evitar desbordamiento ────── */
                case EVENT_WINDOW_CLOSE:
                default:
                    break;
                }
            }
        }

        if (!running) continue;  /* salida por evento de ratón */

        /* ── Compositor (damage tracking) + capas superiores ─────────── */
        gfx_layout_refresh();

        if (menu_open)
            compositor_mark_full();
        if (installer_win.is_visible)
            compositor_mark_full();

        {
            static int term_phase = -1;
            int p = (int)((ticks / 400) & 1);
            if (p != term_phase) {
                term_phase = p;
                if (wins[0].visible)
                    nwm_window_mark_dirty(&wins[0]);
            }
        }
        if (wins[3].visible)
            nwm_window_mark_dirty(&wins[3]);

        {
            int wi;
            for (wi = 0; wi < win_count; wi++) {
                if (wins[wi].visible)
                    nwm_window_ensure_backing(&wins[wi], paint_client_for_win);
            }
        }

        desktop_get_launcher_rect(SW, SH, &lx, &ly, &lw, &lh);
        anchor_x = lx + lw / 2;
        anchor_y = ly;

        gui_render_frame(SW, SH, (int)mouse_get_x(), (int)mouse_get_y(), &dock_hover, menu_open,
                           menu_sel, anchor_x, anchor_y);

        /* ── Teclado (ruta legacy: tecla_nueva + KbdState) ──────────── */
        if (!tecla_nueva) continue;
        /* Instalador: entrada vía EVENT_KEY_PRESS (pop_event); evitar doble Tab/Enter. */
        if (installer_win.is_visible && ui_element_count > 0) {
            tecla_nueva = 0;
            continue;
        }
        {
            unsigned char sc = tecla_nueva;
            KbdEvent kev;
            tecla_nueva = 0;
            kev = kbd_handle_scancode(&kbd, sc);
            if (kev.type == KBD_EV_NONE) continue;
            if (kev.type == KBD_EV_ESC) {
                if (menu_open) { menu_open = 0; continue; }
                running = 0;
                continue;
            }
            if (kev.type == KBD_EV_UP && menu_open) {
                menu_sel = (menu_sel - 1 + MENU_N) % MENU_N;
                continue;
            }
            if (kev.type == KBD_EV_DOWN && menu_open) {
                menu_sel = (menu_sel + 1) % MENU_N;
                continue;
            }
            if (kev.type == KBD_EV_ENTER && menu_open) {
                menu_open = 0;
                if (menu_sel == MENU_N - 1) { running = 0; continue; }
                { int wid = menu_win_id[menu_sel];
                  if (wid >= 0 && wid < win_count) win_show(wid); }
                continue;
            }
            if (kev.type == KBD_EV_CHAR) {
                if (kev.ch >= '1' && kev.ch <= '5') {
                    int id = kev.ch - '1';
                    if (id < win_count) {
                        if (wins[id].visible && wins[id].focused)
                            win_hide(id);
                        else
                            win_show(id);
                    }
                    continue;
                }
                if (kev.ch == '\t') {
                    if (ui_element_count > 0) { ui_focus_advance(); continue; }
                    int n = (focus_id + 1) % win_count;
                    int t;
                    for (t = 0; t < win_count; t++) {
                        if (wins[n].visible) { win_show(n); break; }
                        n = (n + 1) % win_count;
                    }
                    continue;
                }
                if ((kev.ch == 'w' || kev.ch == 'W') && focus_id >= 0) {
                    win_hide(focus_id);
                    continue;
                }
                if ((kev.ch == 'n' || kev.ch == 'N') && focus_id < 0) {
                    menu_open = !menu_open;
                    menu_sel = 0;
                    continue;
                }
            }
        }
    }
    }

    {
        int bi;
        for (bi = 0; bi < win_count; bi++)
            nwm_window_free_backing(&wins[bi]);
    }
    win_count = 0;
    focus_id = -1;
    menu_open = 0;
    compositor_shutdown();
    {
        VesaBootInfo vbi2;
        if (gfx_vesa_detect(&vbi2)) {
            gfx_init_vesa(vbi2.lfb_ptr, vbi2.width, vbi2.height, vbi2.pitch, vbi2.bpp);
            vesa_console_init();
        }
    }
    limpiar_pantalla();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Widget system — estado global, registro, renderizado, entrada.
 * ═══════════════════════════════════════════════════════════════════════════ */

UI_Element ui_elements[UI_MAX_ELEMENTS];
int        ui_element_count;
int        focused_element_index;

/* Orden de Tab: índices en ui_elements[]. */
static int ui_focus_order[UI_MAX_ELEMENTS];
static int ui_focus_order_count;
static int ui_focus_ring_index;
static int ui_tracked_focus_id = -1;

/* ── Foco / anillo focusable ───────────────────────────────────────────── */
void ui_focus_reset_step(void) {
    ui_tracked_focus_id = -1;
}

void gui_blur_widget(int focus_idx) {
    (void)focus_idx;
}

void gui_focus_widget(int focus_idx) {
    if (focus_idx < 0 || focus_idx >= ui_focus_order_count)
        return;
    ui_focus_ring_index     = focus_idx;
    focused_element_index   = ui_focus_order[focus_idx];
    ui_tracked_focus_id     = ui_elements[focused_element_index].id;
}

void ui_sync_focus_ring_from_mouse(void) {
    int i;
    if (focused_element_index < 0 || focused_element_index >= ui_element_count)
        return;
    for (i = 0; i < ui_focus_order_count; i++) {
        if (ui_focus_order[i] == focused_element_index) {
            ui_focus_ring_index = i;
            ui_tracked_focus_id = ui_elements[focused_element_index].id;
            return;
        }
    }
}

void ui_focus_chain_rebuild(void) {
    int i, slot;

    ui_focus_order_count = 0;
    for (i = 0; i < ui_element_count; i++) {
        if (ui_elements[i].is_focusable)
            ui_focus_order[ui_focus_order_count++] = i;
    }

    if (ui_focus_order_count <= 0) {
        ui_focus_ring_index   = 0;
        focused_element_index = 0;
        return;
    }

    slot = 0;
    if (ui_tracked_focus_id >= 0) {
        for (i = 0; i < ui_focus_order_count; i++) {
            if (ui_elements[ui_focus_order[i]].id == ui_tracked_focus_id) {
                slot = i;
                break;
            }
        }
    }

    ui_focus_ring_index     = slot;
    focused_element_index   = ui_focus_order[slot];
    ui_tracked_focus_id     = ui_elements[focused_element_index].id;
}

void ui_focus_tab_next(void) {
    if (ui_focus_order_count <= 0)
        return;
    gui_blur_widget(ui_focus_ring_index);
    ui_focus_ring_index = (ui_focus_ring_index + 1) % ui_focus_order_count;
    gui_focus_widget(ui_focus_ring_index);
}

void ui_focus_tab_prev(void) {
    if (ui_focus_order_count <= 0)
        return;
    gui_blur_widget(ui_focus_ring_index);
    ui_focus_ring_index = (ui_focus_ring_index + ui_focus_order_count - 1) % ui_focus_order_count;
    gui_focus_widget(ui_focus_ring_index);
}

void ui_focus_advance(void) {
    ui_focus_tab_next();
}

void ui_redraw(void) {
    /* El bucle gráfico repinta cada frame; hook disponible para extensiones. */
}

void ui_focus_clear(void) {
    ui_element_count       = 0;
    focused_element_index  = 0;
    ui_focus_order_count   = 0;
    ui_focus_ring_index    = 0;
    ui_tracked_focus_id    = -1;
    ui_manager_clear();
}

/* ── Hit-test ───────────────────────────────────────────────────────────── */
int ui_get_element_at(int x, int y) {
    int i;
    for (i = ui_element_count - 1; i >= 0; i--) {
        const UI_Element* e = &ui_elements[i];
        if (x >= e->x && x < e->x + e->w && y >= e->y && y < e->y + e->h)
            return i;
    }
    return -1;
}

void ui_update_focus_from_mouse(int mx, int my) {
    int idx = ui_get_element_at(mx, my);
    if (idx >= 0 && ui_elements[idx].is_focusable) {
        focused_element_index = idx;
        ui_sync_focus_ring_from_mouse();
    }
}

/* ── Activación ─────────────────────────────────────────────────────────── */
void ui_activate_focused(void) {
    UI_Element* el;
    if (ui_element_count <= 0) return;
    if (focused_element_index < 0 || focused_element_index >= ui_element_count) return;

    el = &ui_elements[focused_element_index];

    /* CHECKBOX: toggle al activar; ejecutar callback si existe. */
    if (el->type == UI_TYPE_CHECKBOX) {
        el->is_checked = !el->is_checked;
        if (el->callback) el->callback();
        return;
    }

    if (el->callback) el->callback();
}

/* ── Registro de widgets ────────────────────────────────────────────────── */
/*
 * Helper interno.  Solo toca campos estructurales; deja intactos text_buffer,
 * text_len, is_checked y progress_value para que persistan entre frames.
 */
static int ui_push_base(int id, int x, int y, int w, int h,
                        int type, UI_ElementCallback cb) {
    UI_Element* e;
    int idx;
    if (ui_element_count >= UI_MAX_ELEMENTS) return -1;
    idx = ui_element_count++;
    e   = &ui_elements[idx];
    e->id       = id;
    e->x  = x; e->y = y; e->w = w; e->h = h;
    e->type     = type;
    e->callback = cb;
    e->on_keypress   = 0;
    e->is_focusable  = (type != UI_TYPE_PROGRESS_BAR) ? 1 : 0;
    return idx;
}

static void ui_copy_str(char* dst, const char* src, int cap) {
    int i = 0;
    if (!src) { dst[0] = 0; return; }
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int ui_push_button(int id, int x, int y, int w, int h, UI_ElementCallback cb) {
    return ui_push_base(id, x, y, w, h, UI_TYPE_BUTTON, cb);
}

int ui_push_text_input(int id, int x, int y, int w, int h, const char* placeholder,
                       int is_password) {
    int idx = ui_push_base(id, x, y, w, h, UI_TYPE_TEXT_INPUT, 0);
    if (idx >= 0) {
        ui_copy_str(ui_elements[idx].placeholder, placeholder, 64);
        ui_elements[idx].is_password = is_password ? 1 : 0;
    }
    return idx;
}

int ui_push_checkbox(int id, int x, int y, int w,
                     const char* label, UI_ElementCallback cb) {
    int idx = ui_push_base(id, x, y, w, 26, UI_TYPE_CHECKBOX, cb);
    if (idx >= 0) ui_copy_str(ui_elements[idx].label, label, 64);
    return idx;
}

int ui_push_progress_bar(int id, int x, int y, int w, int h, int value) {
    int idx = ui_push_base(id, x, y, w, h, UI_TYPE_PROGRESS_BAR, 0);
    if (idx >= 0) ui_elements[idx].progress_value = value;
    return idx;
}

/* ── Renderizado de widgets ─────────────────────────────────────────────── */
void ui_draw_element(int idx) {
    UI_Element* e;
    int focused;

    if (idx < 0 || idx >= ui_element_count) return;
    e       = &ui_elements[idx];
    focused = (idx == focused_element_index);

    switch (e->type) {

    /* BUTTON: dibujado por installer_ui con draw_labeled_button; aquí no-op. */
    case UI_TYPE_BUTTON:
        break;

    /* ── TEXT_INPUT ─────────────────────────────────────────────────────── */
    case UI_TYPE_TEXT_INPUT: {
        int pad_x = 10;
        int ty    = e->y + (e->h - FONT_HQ_CELL_H) / 2;

        /* Fondo */
        gfx_fill_rounded_rect(e->x, e->y, e->w, e->h, 6, RGB(22, 26, 42));

        /* Borde + anillo de foco (+2 px exterior, azul claro) */
        if (focused) {
            gfx_rounded_rect_stroke_aa(e->x - 2, e->y - 2,
                                       e->w + 4, e->h + 4, 8,
                                       RGB(130, 215, 255));
            gfx_rounded_rect_stroke_aa(e->x - 1, e->y - 1,
                                       e->w + 2, e->h + 2, 7,
                                       RGB(200, 235, 255));
            gfx_rounded_rect_stroke_aa(e->x, e->y, e->w, e->h, 6,
                                       RGB(0, 150, 210));
        } else {
            gfx_rounded_rect_stroke_aa(e->x, e->y, e->w, e->h, 6,
                                       RGB(55, 60, 85));
        }

        /* Contenido: texto o placeholder */
        if (e->text_len > 0) {
            /* Clip scroll: mostrar solo el sufijo que cabe */
            const char* buf = e->text_buffer;
            int avail = e->w - 2 * pad_x - (e->is_password ? 30 : 0);
            int start = 0;
            int i;
            char disp[256];

            while (start < e->text_len) {
                int tlen = e->text_len - start;
                if (e->is_password) {
                    for (i = 0; i < tlen; i++) disp[i] = '*';
                    disp[i] = 0;
                } else {
                    for (i = 0; i < tlen; i++) disp[i] = buf[start + i];
                    disp[i] = 0;
                }
                if (gfx_text_width_hq(disp) <= avail) break;
                start++;
            }
            gfx_draw_text_hq(e->x + pad_x, ty, disp, RGB(220, 224, 240));

            /* Icono "ojo" simple (GTK): revelar que hay campo oculto) */
            if (e->is_password) {
                int ex = e->x + e->w - 28;
                int ey = e->y + e->h / 2;
                gfx_circle_outline_aa(ex, ey, 7, RGB(120, 130, 160));
                gfx_fill_circle(ex, ey, 3, RGB(220, 224, 240));
            }

            /* Cursor parpadeante al final del texto */
            if (focused && ((ticks >> 9) & 1u)) {
                int cx = e->x + pad_x + gfx_text_width_hq(disp);
                gfx_fill_rect(cx, ty + 2, 2, FONT_HQ_CELL_H - 4,
                              RGB(0, 180, 240));
            }
        } else {
            /* Placeholder */
            gfx_draw_text_hq(e->x + pad_x, ty, e->placeholder, RGB(75, 80, 108));
            /* Cursor al inicio */
            if (focused && ((ticks >> 9) & 1u)) {
                gfx_fill_rect(e->x + pad_x, ty + 2, 2, FONT_HQ_CELL_H - 4,
                              RGB(0, 180, 240));
            }
        }
        break;
    }

    /* ── CHECKBOX ───────────────────────────────────────────────────────── */
    case UI_TYPE_CHECKBOX: {
        int bx = e->x, by = e->y;
        int bs = 24; /* tamaño del cuadrado */
        int ly = by + (bs - FONT_HQ_CELL_H) / 2;

        if (e->is_checked) {
            /* Relleno verde */
            gfx_fill_rounded_rect(bx, by, bs, bs, 5, RGB(46, 180, 80));
            gfx_rounded_rect_stroke_aa(bx, by, bs, bs, 5, RGB(20, 110, 45));
            /* Check mark (V): trazo grueso doble para visibilidad) */
            gfx_wu_line(bx + 4,  by + bs/2,      bx + bs/2 - 1, by + bs - 5, RGB(255, 255, 255));
            gfx_wu_line(bx + 5,  by + bs/2 + 1,  bx + bs/2,     by + bs - 4, RGB(255, 255, 255));
            gfx_wu_line(bx + bs/2 - 1, by + bs - 5,  bx + bs - 4, by + 4,    RGB(255, 255, 255));
            gfx_wu_line(bx + bs/2,     by + bs - 4,  bx + bs - 3, by + 5,    RGB(255, 255, 255));
        } else {
            /* Fondo oscuro */
            gfx_fill_rounded_rect(bx, by, bs, bs, 5, RGB(22, 26, 42));
            if (focused) {
                gfx_rounded_rect_stroke_aa(bx - 2, by - 2, bs + 4, bs + 4, 7,
                                           RGB(130, 215, 255));
                gfx_rounded_rect_stroke_aa(bx - 1, by - 1, bs + 2, bs + 2, 6,
                                           RGB(200, 235, 255));
                gfx_rounded_rect_stroke_aa(bx, by, bs, bs, 5, RGB(0, 150, 210));
            } else {
                gfx_rounded_rect_stroke_aa(bx, by, bs, bs, 5, RGB(55, 60, 85));
            }
        }

        /* Etiqueta a la derecha */
        if (e->label[0]) {
            unsigned int col = focused ? RGB(230, 235, 255) : RGB(195, 200, 218);
            gfx_draw_text_hq(bx + bs + 12, ly, e->label, col);
        }
        break;
    }

    /* ── PROGRESS_BAR ───────────────────────────────────────────────────── */
    case UI_TYPE_PROGRESS_BAR: {
        int pct = e->progress_value;
        int bx = e->x, by = e->y, bw = e->w, bh = e->h;
        int inner_w;
        int r = bh / 2;

        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;

        /* Pista */
        gfx_fill_rounded_rect(bx, by, bw, bh, r, RGB(20, 23, 36));
        gfx_rounded_rect_stroke_aa(bx, by, bw, bh, r, RGB(45, 50, 72));

        /* Relleno */
        inner_w = (bw - 8) * pct / 100;
        if (inner_w > 0) {
            int ir = (bh - 8) / 2;
            gfx_fill_rounded_rect(bx + 4, by + 4, inner_w, bh - 8, ir,
                                  RGB(0, 122, 245));
            /* Shimmer superior (brillo) */
            gfx_blend_rect(bx + 4, by + 4, inner_w, (bh - 8) / 2,
                           RGB(100, 200, 255), 55);
        }

        /* Porcentaje encima a la derecha */
        {
            char buf[8];
            int  n = 0;
            if (pct == 100) {
                buf[n++] = '1'; buf[n++] = '0'; buf[n++] = '0';
            } else if (pct >= 10) {
                buf[n++] = (char)('0' + pct / 10);
                buf[n++] = (char)('0' + pct % 10);
            } else {
                buf[n++] = (char)('0' + pct);
            }
            buf[n++] = '%'; buf[n] = 0;

            gfx_draw_text_hq(bx + bw - gfx_text_width_hq(buf) - 6,
                             by - FONT_HQ_CELL_H - 4,
                             buf, RGB(0, 150, 210));
        }
        break;
    }

    default:
        break;
    }
}

void ui_draw_all_elements(void) {
    int i;
    for (i = 0; i < ui_element_count; i++) {
        if (ui_elements[i].type != UI_TYPE_BUTTON)
            ui_draw_element(i);
    }
}

/* ── Entrada de texto ───────────────────────────────────────────────────── */
void ui_handle_char(unsigned char ch) {
    UI_Element* el;
    if (ui_element_count <= 0) return;
    if (focused_element_index < 0 || focused_element_index >= ui_element_count) return;
    el = &ui_elements[focused_element_index];
    if (el->type != UI_TYPE_TEXT_INPUT) return;

    if (ch == (unsigned char)'\b') {
        if (el->text_len > 0) {
            el->text_len--;
            el->text_buffer[el->text_len] = 0;
        }
    } else if (ch >= 32u && ch < 127u && el->text_len < 255) {
        el->text_buffer[el->text_len++] = (char)ch;
        el->text_buffer[el->text_len]   = 0;
    }
}
