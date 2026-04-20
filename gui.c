/*
 * Doble búfer: kmalloc(pitch * height) + gfx_attach_double_buffer().
 * Dibujar solo en el backbuffer; al final del frame, swap_buffers() copia al LFB (rep movsq).
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
#include <stdint.h>

extern volatile unsigned char tecla_nueva;
extern volatile uint64_t ticks;

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

void swap_buffers(void) {
    gfx_swap_buffers();
}

static int SW, SH;
static int desk_x, desk_y, desk_w, desk_h;

#define MAX_WIN 6
static NWM_Window wins[MAX_WIN];
static int  win_count, focus_id;
static int  zorder[MAX_WIN], zcount;
static int  menu_open, menu_sel;

static void z_raise(int id) {
    nwm_raise_window(zorder, zcount, id);
}

static void win_show(int id) {
    int i;
    if (id < 0 || id >= win_count) return;
    for (i = 0; i < win_count; i++) wins[i].focused = 0;
    wins[id].focused = 1;
    wins[id].visible = 1;
    focus_id = id;
    z_raise(id);
}

static void win_hide(int id) {
    int i;
    if (id < 0 || id >= win_count) return;
    wins[id].visible = 0;
    wins[id].focused = 0;
    if (focus_id == id) {
        focus_id = -1;
        for (i = zcount - 1; i >= 0; i--) {
            int w = zorder[i];
            if (wins[w].visible) {
                win_show(w);
                return;
            }
        }
    }
}

static void draw_terminal(NWM_Window* w);
static void draw_files(NWM_Window* w);
static void draw_sysmon(NWM_Window* w);
static void draw_about(NWM_Window* w);
static void paint_client_cb(void* user, int win_idx);

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

static void paint_client_cb(void* user, int win_idx) {
    (void)user;
    if (win_idx < 0 || win_idx >= win_count) return;
    if (!wins[win_idx].visible) return;
    switch (wins[win_idx].app_type) {
        case 0: draw_terminal(&wins[win_idx]); break;
        case 1: draw_files(&wins[win_idx]); break;
        case 2: apps_draw_nexus_firefox(&wins[win_idx]); break;
        case 3: draw_sysmon(&wins[win_idx]); break;
        case 4: draw_about(&wins[win_idx]); break;
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
    unsigned char prev_btns = 0;
    int dock_hover;
    int lx, ly, lw, lh, anchor_x, anchor_y;

    if (has_vesa) {
        if (vbi.bpp == 32u && vbi.pitch > 0u && vbi.height > 0u) {
            if (gui_framebuffer_init_kmalloc(&vbi) != 0)
                kheap_panic_nomem("gui: double buffer (pitch*height)");
        } else {
            gfx_init_vesa(vbi.lfb_ptr, vbi.width, vbi.height, vbi.pitch, vbi.bpp);
        }
        if (vesa_console_active)
            vesa_force_refresh();
        mouse_init(vbi.width, vbi.height);
    } else {
        gfx_init_vga();
        mouse_init(320, 200);
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
    zcount = 0;

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
            wins[0] = (NWM_Window){cx0 - st, cy0 - st / 2, w0, h0, 0, 0, 0, 0, 0, "Terminal", 0, 0};
            wins[1] = (NWM_Window){cx0 + st, cy0 + st / 2, w0, h0, 0, 0, 0, 0, 0, "Files", 1, 0};
            wins[2] = (NWM_Window){(SW - w_fx) / 2, layout_top_h + (desk_inner_h - h_fx) / 2 + st,
                                   w_fx, h_fx, 0, 0, 0, 0, 0, "Nexus Firefox", 2, 0};
            wins[3] = (NWM_Window){(SW - w3) / 2 + st * 2, layout_top_h + (desk_inner_h - h3) / 3,
                                   w3, h3, 0, 0, 0, 0, 0, "System Monitor", 3, 0};
            wins[4] = (NWM_Window){(SW - w4) / 2 - st, layout_top_h + (2 * desk_inner_h) / 3 - h4 / 2,
                                   w4, h4, 0, 0, 0, 0, 0, "About NexusOS", 4, 0};
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

    for (int i = 0; i < win_count; i++) zorder[i] = i;
    zcount = win_count;

    wins[0].visible = 1;
    wins[1].visible = 1;
    for (int i = 0; i < win_count; i++) wins[i].focused = 0;
    wins[0].focused = 1;
    focus_id = 0;
    nwm_raise_window(zorder, zcount, 0);

    {
        KbdState kbd;
        kbd_init(&kbd);

    while (running) {
        if (ticks - last_frame < 2) {
            __asm__ volatile("hlt");
            continue;
        }
        last_frame = ticks;

        gfx_layout_refresh();
        draw_desktop();
        desktop_draw_top_bar();
        top_panel_draw(SW, SH);
        nwm_paint_painter_order(wins, win_count, zorder, zcount, paint_client_cb, 0);

        /* WM compuesto (p. ej. instalador): encima del contenido NWM, debajo del dock. */
        desktop_paint_wm_windows();

        desktop_get_launcher_rect(SW, SH, &lx, &ly, &lw, &lh);
        anchor_x = lx + lw / 2;
        anchor_y = ly;

        desktop_draw_dock(SW, SH, mouse_x, mouse_y, &dock_hover);
        desktop_draw_app_menu(menu_open, menu_sel, anchor_x, anchor_y);

        /* Cursor encima de todo; volcado rápido al LFB físico. */
        gfx_draw_cursor(mouse_x, mouse_y);
        swap_buffers();

        {
            unsigned char btns = mouse_buttons;
            int click = (btns & 1) && !(prev_btns & 1);
            int held = (btns & 1);
            int release = !(btns & 1) && (prev_btns & 1);
            prev_btns = btns;

            if (held && !click) {
                int i;
                for (i = 0; i < win_count; i++) {
                    if (wins[i].dragging) {
                        nwm_apply_window_drag(&wins[i], mouse_x, mouse_y, layout_top_h, LAYOUT_WORK_BOTTOM);
                        break;
                    }
                }
            }
            if (release) {
                int i;
                for (i = 0; i < win_count; i++) wins[i].dragging = 0;
            }

            if (click) {
                if (installer_win.is_visible && ui_element_count > 0) {
                    int ui_hit = ui_get_element_at((int)mouse_x, (int)mouse_y);
                    if (ui_hit >= 0) {
                        focused_element_index = ui_hit;
                        ui_activate_focused();
                        goto done;
                    }
                }
                int dh = desktop_dock_hit(SW, SH, mouse_x, mouse_y);
                if (menu_open) {
                    if (desktop_menu_contains(anchor_x, anchor_y, mouse_x, mouse_y)) {
                        int sel = desktop_menu_hit(anchor_x, anchor_y, mouse_x, mouse_y);
                        if (sel >= 0) {
                            menu_open = 0;
                            if (sel == MENU_N - 1) {
                                running = 0;
                                continue;
                            }
                            {
                                int wid = menu_win_id[sel];
                                if (wid >= 0 && wid < win_count) win_show(wid);
                            }
                        }
                    } else {
                        menu_open = 0;
                    }
                    goto done;
                }

                if (dh == -2) {
                    menu_open = !menu_open;
                    menu_sel = 0;
                    goto done;
                }
                if (dh >= 0 && dh < DOCK_ITEMS) {
                    if (dh == DOCK_ITEMS - 1) {
                        running = 0;
                        continue;
                    }
                    {
                        int wid = dock_win_id[dh];
                        if (wid >= 0 && wid < win_count) {
                            if (wins[wid].visible && wins[wid].focused)
                                win_hide(wid);
                            else
                                win_show(wid);
                        }
                    }
                    goto done;
                }

                {
                    int i;
                    for (i = zcount - 1; i >= 0; i--) {
                        int id = zorder[i];
                        if (!wins[id].visible) continue;
                        if (mouse_x >= wins[id].x && mouse_x < wins[id].x + wins[id].w &&
                            mouse_y >= wins[id].y && mouse_y < wins[id].y + wins[id].h) {
                            if (nwm_hit_close_button(&wins[id], mouse_x, mouse_y, layout_chrome_title_h))
                                win_hide(id);
                            else if (nwm_hit_titlebar(&wins[id], mouse_x, mouse_y, layout_chrome_title_h)) {
                                win_show(id);
                                wins[id].dragging = 1;
                                wins[id].drag_ox = mouse_x - wins[id].x;
                                wins[id].drag_oy = mouse_y - wins[id].y;
                            } else
                                win_show(id);
                            goto done;
                        }
                    }
                }
            }
        }
    done:

        if (!tecla_nueva) continue;
        {
            unsigned char sc = tecla_nueva;
            KbdEvent ev;
            tecla_nueva = 0;
            ev = kbd_handle_scancode(&kbd, sc);
            if (ev.type == KBD_EV_NONE) continue;
            if (ev.type == KBD_EV_ESC) {
                if (menu_open) {
                    menu_open = 0;
                    continue;
                }
                running = 0;
                continue;
            }
            if (ev.type == KBD_EV_UP && menu_open) {
                menu_sel = (menu_sel - 1 + MENU_N) % MENU_N;
                continue;
            }
            if (ev.type == KBD_EV_DOWN && menu_open) {
                menu_sel = (menu_sel + 1) % MENU_N;
                continue;
            }
            if (ev.type == KBD_EV_ENTER && menu_open) {
                menu_open = 0;
                if (menu_sel == MENU_N - 1) {
                    running = 0;
                    continue;
                }
                {
                    int wid = menu_win_id[menu_sel];
                    if (wid >= 0 && wid < win_count) win_show(wid);
                }
                continue;
            }
            if (ev.type == KBD_EV_CHAR) {
                if (ev.ch >= '1' && ev.ch <= '5') {
                    int id = ev.ch - '1';
                    if (id < win_count) {
                        if (wins[id].visible && wins[id].focused)
                            win_hide(id);
                        else
                            win_show(id);
                    }
                    continue;
                }
                if (ev.ch == '\t') {
                    if (ui_element_count > 0) {
                        ui_focus_advance();
                        continue;
                    }
                    int n = (focus_id + 1) % win_count;
                    int t;
                    for (t = 0; t < win_count; t++) {
                        if (wins[n].visible) {
                            win_show(n);
                            break;
                        }
                        n = (n + 1) % win_count;
                    }
                    continue;
                }
                if ((ev.ch == 'w' || ev.ch == 'W') && focus_id >= 0) {
                    win_hide(focus_id);
                    continue;
                }
                if ((ev.ch == 'n' || ev.ch == 'N') && focus_id < 0) {
                    menu_open = !menu_open;
                    menu_sel = 0;
                    continue;
                }
            }
        }
    }
    }

    win_count = 0;
    focus_id = -1;
    menu_open = 0;
    zcount = 0;
    {
        VesaBootInfo vbi2;
        if (gfx_vesa_detect(&vbi2)) {
            gfx_init_vesa(vbi2.lfb_ptr, vbi2.width, vbi2.height, vbi2.pitch, vbi2.bpp);
            vesa_console_init();
        }
    }
    limpiar_pantalla();
}

/* ── Gestor de foco (TAB / Enter en instalador y UI con teclado) ───────── */
UI_Element ui_elements[UI_MAX_ELEMENTS];
int ui_element_count;
int focused_element_index;

void ui_focus_advance(void) {
    if (ui_element_count <= 0)
        return;
    focused_element_index = (focused_element_index + 1) % ui_element_count;
}

void ui_redraw(void) {
    /* Los bucles gráficos ya repintan cada frame; hook para extensiones. */
}

void ui_focus_clear(void) {
    ui_element_count        = 0;
    focused_element_index   = 0;
}

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
    if (idx >= 0)
        focused_element_index = idx;
}

void ui_activate_focused(void) {
    UI_ElementCallback cb;
    if (ui_element_count <= 0)
        return;
    if (focused_element_index < 0 || focused_element_index >= ui_element_count)
        return;
    cb = ui_elements[focused_element_index].callback;
    if (cb)
        cb();
}
