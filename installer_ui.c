/*
 * Asistente visual de instalación (flujo tipo Calamares, sin backend real aún).
 */
#include "installer_ui.h"
#include "gui.h"
#include "gfx.h"
#include "mouse.h"
#include "nexus.h"
#include "font_data.h"

InstallerState current_step = WELCOME;

static void cb_welcome_next(void) { current_step = TIMEZONE; }

static void cb_tz_next(void) { current_step = DISK_SETUP; }

static void cb_disk_install(void) { current_step = INSTALLING; }

static void cb_reboot(void) {
    outb(0x64u, 0xFEu);
    for (;;)
        __asm__ volatile("hlt");
}

static int ui_push_element(int id, int x, int y, int w, int h, UI_ElementCallback cb) {
    int idx;
    if (ui_element_count >= UI_MAX_ELEMENTS)
        return -1;
    idx = ui_element_count;
    ui_elements[idx].id       = id;
    ui_elements[idx].x        = x;
    ui_elements[idx].y        = y;
    ui_elements[idx].w        = w;
    ui_elements[idx].h        = h;
    ui_elements[idx].callback = cb;
    ui_element_count++;
    return idx;
}

/* ~10 s reales a 1000 Hz (PIT IRQ0). */
#define INSTALL_DURATION_TICKS (10ull * (uint64_t)PIT_TICKS_PER_SEC)

static uint64_t install_start_tick;
static int install_timer_armed;

static void installer_client_rect(const Window* w, int* cx, int* cy, int* cw, int* ch) {
    *cx = w->x + INSTALLER_WIN_INSET;
    *cy = w->y + INSTALLER_WIN_TITLE_H + 2;
    *cw = w->width - 2 * INSTALLER_WIN_INSET;
    *ch = w->height - INSTALLER_WIN_TITLE_H - 6;
}

static void draw_labeled_button(int x, int y, int bw, int bh, const char* label, unsigned int bg,
                                unsigned int stroke_rgb, int focused) {
    int tw = gfx_text_width_hq(label);
    int tx = x + (bw - tw) / 2;
    int ty = y + (bh - FONT_HQ_CELL_H) / 2;
    if (focused) {
        gfx_rounded_rect_stroke_aa(x - 4, y - 4, bw + 8, bh + 8, 12, RGB(0, 255, 240));
        gfx_rounded_rect_stroke_aa(x - 2, y - 2, bw + 4, bh + 4, 10, RGB(160, 255, 248));
    }
    gfx_fill_rounded_rect(x, y, bw, bh, 8, bg);
    gfx_rounded_rect_stroke_aa(x, y, bw, bh, 8, stroke_rgb);
    gfx_draw_text_hq(tx, ty, label, RGB(255, 255, 255));
}

static void draw_lang_option(int x, int y, int w, int h, const char* name, int selected) {
    unsigned int bg = selected ? RGB(220, 235, 255) : RGB(240, 242, 248);
    unsigned int fg = selected ? RGB(20, 50, 120) : RGB(60, 62, 72);
    unsigned int border = selected ? RGB(0, 100, 220) : RGB(190, 195, 210);
    gfx_fill_rounded_rect(x, y, w, h, 6, bg);
    gfx_rounded_rect_stroke_aa(x, y, w, h, 6, border);
    gfx_draw_text_hq(x + 14, y + (h - FONT_HQ_CELL_H) / 2, name, fg);
    if (selected)
        gfx_draw_text_hq(x + w - 36, y + (h - FONT_HQ_CELL_H) / 2, "[*]", RGB(0, 120, 220));
}

static void draw_welcome(int cx, int cy, int cw, int ch) {
    const char* headline = "Bienvenido a NexusOS";
    int title_scale = 3;
    int tw = gfx_text_width_aa(headline, title_scale);
    int tx = cx + (cw - tw) / 2;
    int ty = cy + 24;
    if (tx < cx + 8) tx = cx + 8;

    gfx_draw_text_aa(tx, ty, headline, RGB(28, 32, 44), title_scale);
    gfx_draw_text_hq(cx + 24, ty + 28 * title_scale + 16, "Asistente de instalacion — Live CD", RGB(90, 95, 110));

    {
        int ly = ty + 28 * title_scale + 52;
        int lh = 36;
        int gap = 10;
        draw_lang_option(cx + 24, ly, cw - 48, lh, "Espanol (Espana)", 1);
        draw_lang_option(cx + 24, ly + lh + gap, cw - 48, lh, "English (US)", 0);
        draw_lang_option(cx + 24, ly + 2 * (lh + gap), cw - 48, lh, "Catala", 0);
    }

    {
        int bw = 130, bh = 40;
        int bx = cx + cw - bw - 24;
        int by = cy + ch - bh - 20;
        int fi = ui_push_element(1, bx, by, bw, bh, cb_welcome_next);
        draw_labeled_button(bx, by, bw, bh, "Siguiente", RGB(0, 122, 245), RGB(20, 60, 140),
                            fi >= 0 && focused_element_index == fi);
    }
}

static void draw_timezone_step(int cx, int cy, int cw, int ch) {
    gfx_draw_text_aa(cx + 20, cy + 16, "Zona horaria", RGB(28, 32, 44), 2);
    gfx_draw_text_hq(cx + 20, cy + 52, "Seleccione la region para el reloj del sistema.", RGB(80, 85, 100));

    {
        int ly = cy + 88;
        int lh = 34;
        int gap = 8;
        draw_lang_option(cx + 20, ly, cw - 40, lh, "Europe / Madrid", 1);
        draw_lang_option(cx + 20, ly + lh + gap, cw - 40, lh, "UTC", 0);
        draw_lang_option(cx + 20, ly + 2 * (lh + gap), cw - 40, lh, "America / New_York", 0);
    }

    {
        int bw = 130, bh = 40;
        int bx = cx + cw - bw - 24;
        int by = cy + ch - bh - 20;
        int fi = ui_push_element(2, bx, by, bw, bh, cb_tz_next);
        draw_labeled_button(bx, by, bw, bh, "Siguiente", RGB(0, 122, 245), RGB(20, 60, 140),
                            fi >= 0 && focused_element_index == fi);
    }
}

static void draw_disk_setup(int cx, int cy, int cw, int ch) {
    int disk_y = cy + 20;
    int disk_h = 72;

    gfx_draw_text_aa(cx + 20, disk_y, "Destino de la instalacion", RGB(28, 32, 44), 2);
    disk_y += 40;

    gfx_fill_rounded_rect(cx + 20, disk_y, cw - 40, disk_h, 10, RGB(228, 232, 240));
    gfx_rounded_rect_stroke_aa(cx + 20, disk_y, cw - 40, disk_h, 10, RGB(140, 150, 175));
    /* Icono disco simplificado */
    gfx_fill_rounded_rect(cx + 36, disk_y + 16, 40, 40, 6, RGB(100, 110, 130));
    gfx_fill_rect(cx + 44, disk_y + 24, 24, 8, RGB(200, 205, 220));
    gfx_draw_text_hq(cx + 88, disk_y + 18, "/dev/sda", RGB(40, 44, 55));
    gfx_draw_text_hq(cx + 88, disk_y + 40, "50 GB libres · ATA Disk", RGB(75, 80, 95));

    {
        int oy = disk_y + disk_h + 28;
        gfx_fill_rounded_rect(cx + 20, oy, 22, 22, 4, RGB(255, 255, 255));
        gfx_rounded_rect_stroke_aa(cx + 20, oy, 22, 22, 4, RGB(0, 122, 245));
        gfx_fill_rect(cx + 25, oy + 7, 12, 8, RGB(0, 122, 245));
        gfx_draw_text_hq(cx + 52, oy + 2, "Borrar todo el disco e instalar NexusOS", RGB(35, 38, 48));
        gfx_draw_text_hq(cx + 52, oy + 22, "(se eliminaran todos los datos en /dev/sda)", RGB(120, 125, 135));
    }

    {
        int bw = 160, bh = 42;
        int bx = cx + cw - bw - 24;
        int by = cy + ch - bh - 20;
        int fi = ui_push_element(3, bx, by, bw, bh, cb_disk_install);
        draw_labeled_button(bx, by, bw, bh, "Instalar ahora", RGB(40, 168, 98), RGB(15, 90, 50),
                            fi >= 0 && focused_element_index == fi);
    }
}

static void draw_installing(int cx, int cy, int cw, int ch) {
    int pct;
    int bar_x, bar_y, bar_w, bar_h;
    int fill_w;
    int log_y;
    uint64_t elapsed;

    if (!install_timer_armed) {
        install_start_tick = ticks;
        install_timer_armed = 1;
    }
    elapsed = ticks - install_start_tick;
    pct = (int)(elapsed * 100 / INSTALL_DURATION_TICKS);
    if (pct > 100)
        pct = 100;
    if (pct >= 100) {
        current_step = FINISHED;
        install_timer_armed = 0;
        return;
    }

    gfx_draw_text_aa(cx + 20, cy + 16, "Instalando NexusOS", RGB(28, 32, 44), 2);
    gfx_draw_text_hq(cx + 20, cy + 48, "No apague el equipo.", RGB(100, 55, 55));

    bar_x = cx + 32;
    bar_y = cy + ch / 2 - 40;
    bar_w = cw - 64;
    bar_h = 22;
    if (bar_w < 80) bar_w = 80;

    gfx_fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 8, RGB(220, 224, 232));
    gfx_rounded_rect_stroke_aa(bar_x, bar_y, bar_w, bar_h, 8, RGB(160, 168, 185));
    fill_w = (bar_w - 8) * pct / 100;
    if (fill_w < 0) fill_w = 0;
    if (fill_w > bar_w - 8) fill_w = bar_w - 8;
    gfx_fill_rounded_rect(bar_x + 4, bar_y + 4, fill_w, bar_h - 8, 5, RGB(0, 122, 245));

    {
        char pctbuf[8];
        int n = 0;
        if (pct >= 10) {
            pctbuf[n++] = (char)('0' + pct / 10);
            pctbuf[n++] = (char)('0' + pct % 10);
        } else
            pctbuf[n++] = (char)('0' + pct);
        pctbuf[n++] = '%';
        pctbuf[n] = 0;
        gfx_draw_text_hq(bar_x + bar_w - 40, bar_y - 20, pctbuf, RGB(60, 65, 78));
    }

    log_y = bar_y + bar_h + 28;
    gfx_fill_rect(cx + 20, log_y, cw - 40, ch - (log_y - cy) - 16, RGB(24, 26, 32));
    gfx_rounded_rect_stroke_aa(cx + 20, log_y, cw - 40, ch - (log_y - cy) - 16, 6, RGB(60, 65, 78));

    {
        int ly = log_y + 10;
        gfx_draw_text_aa(cx + 28, ly, "> copiando archivos del sistema...", RGB(180, 220, 180), 1);
        ly += 16;
        gfx_draw_text_aa(cx + 28, ly, "> configurando initramfs...", RGB(160, 200, 160), 1);
        ly += 16;
        gfx_draw_text_aa(cx + 28, ly, "> instalando gestor de arranque GRUB...", RGB(140, 180, 140), 1);
        ly += 16;
        if (pct >= 18)
            gfx_draw_text_aa(cx + 28, ly, "> extrayendo paquetes base (squashfs)...", RGB(120, 170, 120), 1);
        ly += 16;
        if (pct >= 40)
            gfx_draw_text_aa(cx + 28, ly, "> generando fstab...", RGB(100, 160, 100), 1);
        ly += 16;
        if (pct >= 62)
            gfx_draw_text_aa(cx + 28, ly, "> sincronizando disco...", RGB(90, 150, 90), 1);
        ly += 16;
        if (pct >= 82)
            gfx_draw_text_aa(cx + 28, ly, "> finalizando instalacion...", RGB(80, 145, 90), 1);
    }
}

static void draw_finished(int cx, int cy, int cw, int ch) {
    int mx = cx + cw / 2;
    int my = cy + 70;
    const char* line1 = "Instalacion completada.";
    const char* line2 = "Por favor, extraiga el medio de instalacion";
    const char* line3 = "y reinicie el equipo.";
    int tw1, tw2, tw3;
    int y;

    gfx_fill_circle(mx, my, 36, RGB(46, 180, 80));
    gfx_circle_outline_aa(mx, my, 36, RGB(20, 100, 40));
    gfx_wu_line(mx - 14, my, mx - 4, my + 14, RGB(255, 255, 255));
    gfx_wu_line(mx - 4, my + 14, mx + 22, my - 18, RGB(255, 255, 255));

    y = my + 52;
    tw1 = gfx_text_width_aa(line1, 2);
    gfx_draw_text_aa(mx - tw1 / 2, y, line1, RGB(28, 32, 44), 2);
    y += 36;
    tw2 = gfx_text_width_hq(line2);
    gfx_draw_text_hq(cx + (cw - tw2) / 2, y, line2, RGB(55, 60, 72));
    y += 22;
    tw3 = gfx_text_width_hq(line3);
    gfx_draw_text_hq(cx + (cw - tw3) / 2, y, line3, RGB(55, 60, 72));

    {
        int bw = 170, bh = 42;
        int bx = cx + cw - bw - 24;
        int by = cy + ch - bh - 20;
        int fi = ui_push_element(4, bx, by, bw, bh, cb_reboot);
        draw_labeled_button(bx, by, bw, bh, "Reiniciar ahora", RGB(0, 122, 245), RGB(20, 60, 140),
                            fi >= 0 && focused_element_index == fi);
    }
}

void draw_installer_content(const Window* win) {
    int cx, cy, cw, ch;
    static InstallerState last_step = INSTALLING;

    if (!win || !win->is_visible)
        return;
    installer_client_rect(win, &cx, &cy, &cw, &ch);
    if (cw < 80 || ch < 60)
        return;

    ui_element_count = 0;
    if (current_step != last_step) {
        focused_element_index = 0;
        last_step             = current_step;
    }

    if (current_step != INSTALLING)
        install_timer_armed = 0;

    switch (current_step) {
        case WELCOME:
            draw_welcome(cx, cy, cw, ch);
            break;
        case TIMEZONE:
            draw_timezone_step(cx, cy, cw, ch);
            break;
        case DISK_SETUP:
            draw_disk_setup(cx, cy, cw, ch);
            break;
        case INSTALLING:
            draw_installing(cx, cy, cw, ch);
            break;
        case FINISHED:
            draw_finished(cx, cy, cw, ch);
            break;
        default:
            break;
    }

    ui_update_focus_from_mouse((int)mouse_x, (int)mouse_y);
}
