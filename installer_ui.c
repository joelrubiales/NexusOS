/*
 * Asistente visual de instalación (flujo tipo Calamares, sin backend real).
 *
 * Pasos:
 *   WELCOME    → pantalla de bienvenida + selector de idioma.
 *   TIMEZONE   → ahora "Configuración del sistema": TEXT_INPUT hostname + usuario.
 *   DISK_SETUP → destino + CHECKBOXes de opciones de disco.
 *   INSTALLING → PROGRESS_BAR animado + log de consola.
 *   FINISHED   → círculo check + botón Reiniciar.
 */
#include "installer_ui.h"
#include "gui.h"
#include "gfx.h"
#include "mouse.h"
#include "nexus.h"
#include "font_data.h"
#include <stddef.h>

InstallerState current_step = WELCOME;

/* ── Callbacks de navegación ─────────────────────────────────────────────── */
static void cb_welcome_next(void) { current_step = TIMEZONE; }
static void cb_tz_next     (void) { current_step = DISK_SETUP; }
static void cb_disk_install(void) { current_step = INSTALLING; }
static void cb_reboot(void) {
    outb(0x64u, 0xFEu);
    for (;;) __asm__ volatile("hlt");
}

/* ~10 s reales a 1000 Hz (PIT IRQ0). */
#define INSTALL_DURATION_TICKS (10ull * (uint64_t)PIT_TICKS_PER_SEC)

static uint64_t install_start_tick;
static int      install_timer_armed;

/* ── Helpers de layout ───────────────────────────────────────────────────── */
static void installer_client_rect(const Window* w,
                                  int* cx, int* cy, int* cw, int* ch) {
    *cx = w->x + INSTALLER_WIN_INSET;
    *cy = w->y + INSTALLER_WIN_TITLE_H + 2;
    *cw = w->width  - 2 * INSTALLER_WIN_INSET;
    *ch = w->height - INSTALLER_WIN_TITLE_H - 6;
}

/* ── draw_labeled_button: dibuja un botón con focus ring opcional ─────────── */
static void draw_labeled_button(int x, int y, int bw, int bh,
                                const char* label,
                                unsigned int bg, unsigned int stroke_rgb,
                                int focused) {
    int tw = gfx_text_width_hq(label);
    int tx = x + (bw - tw) / 2;
    int ty = y + (bh - FONT_HQ_CELL_H) / 2;
    if (focused) {
        gfx_rounded_rect_stroke_aa(x - 4, y - 4, bw + 8, bh + 8, 12,
                                   RGB(0, 255, 240));
        gfx_rounded_rect_stroke_aa(x - 2, y - 2, bw + 4, bh + 4, 10,
                                   RGB(160, 255, 248));
    }
    gfx_fill_rounded_rect(x, y, bw, bh, 8, bg);
    gfx_rounded_rect_stroke_aa(x, y, bw, bh, 8, stroke_rgb);
    gfx_draw_text_hq(tx, ty, label, RGB(255, 255, 255));
}

static void draw_lang_option(int x, int y, int w, int h,
                             const char* name, int selected) {
    unsigned int bg     = selected ? RGB(220, 235, 255) : RGB(240, 242, 248);
    unsigned int fg     = selected ? RGB(20, 50, 120)   : RGB(60, 62, 72);
    unsigned int border = selected ? RGB(0, 100, 220)   : RGB(190, 195, 210);
    gfx_fill_rounded_rect(x, y, w, h, 6, bg);
    gfx_rounded_rect_stroke_aa(x, y, w, h, 6, border);
    gfx_draw_text_hq(x + 14, y + (h - FONT_HQ_CELL_H) / 2, name, fg);
    if (selected)
        gfx_draw_text_hq(x + w - 36, y + (h - FONT_HQ_CELL_H) / 2,
                         "[*]", RGB(0, 120, 220));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 0 — WELCOME
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_welcome(int cx, int cy, int cw, int ch) {
    const char* headline   = "Bienvenido a NexusOS";
    int         title_scale = 3;
    int         tw  = gfx_text_width_aa(headline, title_scale);
    int         tx  = cx + (cw - tw) / 2;
    int         ty  = cy + 24;
    if (tx < cx + 8) tx = cx + 8;

    gfx_draw_text_aa(tx, ty, headline, RGB(28, 32, 44), title_scale);
    gfx_draw_text_hq(cx + 24, ty + 28 * title_scale + 16,
                     "Asistente de instalacion — Live CD",
                     RGB(90, 95, 110));

    {
        int ly  = ty + 28 * title_scale + 52;
        int lh  = 36;
        int gap = 10;
        draw_lang_option(cx + 24, ly,                  cw - 48, lh, "Espanol (Espana)", 1);
        draw_lang_option(cx + 24, ly + lh + gap,       cw - 48, lh, "English (US)",     0);
        draw_lang_option(cx + 24, ly + 2*(lh + gap),   cw - 48, lh, "Catala",           0);
    }

    {
        int bw = 130, bh = 40;
        int bx = cx + cw - bw - 24;
        int by = cy + ch - bh - 20;
        int fi = ui_push_button(1, bx, by, bw, bh, cb_welcome_next);
        draw_labeled_button(bx, by, bw, bh, "Siguiente",
                            RGB(0, 122, 245), RGB(20, 60, 140),
                            fi >= 0 && focused_element_index == fi);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 1 — CONFIGURACIÓN DEL SISTEMA (antes "Zona horaria")
 *  Nuevos widgets: dos TEXT_INPUT + un BUTTON.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_timezone_step(int cx, int cy, int cw, int ch) {
    int field_w  = cw - 40;
    int field_h  = 44;
    int field_x  = cx + 20;
    int label_y, field_y;
    int fi;

    gfx_draw_text_aa(cx + 20, cy + 16, "Configuracion del sistema",
                     RGB(28, 32, 44), 2);
    gfx_draw_text_hq(cx + 20, cy + 54,
                     "Personaliza el nombre de tu equipo y cuenta de usuario.",
                     RGB(80, 85, 100));

    /* ── TEXT_INPUT: Nombre del equipo ───────────────────────────────── */
    label_y  = cy + 94;
    field_y  = label_y + FONT_HQ_CELL_H + 8;

    gfx_draw_text_hq(field_x, label_y, "Nombre del equipo (hostname):",
                     RGB(60, 65, 82));
    fi = ui_push_text_input(10, field_x, field_y, field_w, field_h, "nexusos");
    (void)fi;

    /* ── TEXT_INPUT: Nombre de usuario ───────────────────────────────── */
    label_y  = field_y + field_h + 22;
    field_y  = label_y + FONT_HQ_CELL_H + 8;

    gfx_draw_text_hq(field_x, label_y, "Nombre de usuario:",
                     RGB(60, 65, 82));
    fi = ui_push_text_input(11, field_x, field_y, field_w, field_h, "usuario");
    (void)fi;

    /* ── BUTTON: Siguiente ────────────────────────────────────────────── */
    {
        int bw = 130, bh = 40;
        int bx = cx + cw - bw - 24;
        int by = cy + ch - bh - 20;
        fi = ui_push_button(12, bx, by, bw, bh, cb_tz_next);
        draw_labeled_button(bx, by, bw, bh, "Siguiente",
                            RGB(0, 122, 245), RGB(20, 60, 140),
                            fi >= 0 && focused_element_index == fi);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 2 — DISCO DE DESTINO
 *  Nuevos widgets: dos CHECKBOX + BUTTON.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_disk_setup(int cx, int cy, int cw, int ch) {
    int disk_y = cy + 20;
    int disk_h = 72;

    gfx_draw_text_aa(cx + 20, disk_y, "Destino de la instalacion",
                     RGB(28, 32, 44), 2);
    disk_y += 42;

    /* Tarjeta del disco */
    gfx_fill_rounded_rect(cx + 20, disk_y, cw - 40, disk_h, 10, RGB(228, 232, 240));
    gfx_rounded_rect_stroke_aa(cx + 20, disk_y, cw - 40, disk_h, 10, RGB(140, 150, 175));
    gfx_fill_rounded_rect(cx + 36, disk_y + 16, 40, 40, 6, RGB(100, 110, 130));
    gfx_fill_rect(cx + 44, disk_y + 24, 24, 8, RGB(200, 205, 220));
    gfx_draw_text_hq(cx + 88, disk_y + 18, "/dev/sda",               RGB(40, 44, 55));
    gfx_draw_text_hq(cx + 88, disk_y + 40, "50 GB libres · ATA Disk", RGB(75, 80, 95));

    /* ── CHECKBOX 1: Formatear disco ──────────────────────────────────── */
    {
        int oy  = disk_y + disk_h + 22;
        int lbl_w = 24 + 12 + gfx_text_width_hq("Formatear disco completo (/dev/sda)");
        int fi  = ui_push_checkbox(20, cx + 20, oy, lbl_w,
                                   "Formatear disco completo (/dev/sda)", NULL);
        (void)fi;
    }

    /* ── CHECKBOX 2: Instalar gestor de arranque ──────────────────────── */
    {
        int oy  = disk_y + disk_h + 22 + 26 + 14;
        int lbl_w = 24 + 12 + gfx_text_width_hq("Instalar gestor de arranque GRUB");
        int fi  = ui_push_checkbox(21, cx + 20, oy, lbl_w,
                                   "Instalar gestor de arranque GRUB", NULL);
        /* Pre-marcar como activo en el primer frame (is_checked se preserva después) */
        if (ui_elements[fi >= 0 ? fi : 0].is_checked == 0 &&
            fi >= 0 && ui_elements[fi].progress_value == 0) {
            /* Marcamos el primer frame usando progress_value como flag de init */
            ui_elements[fi].is_checked      = 1;
            ui_elements[fi].progress_value  = 1; /* ya inicializado */
        }
        (void)fi;
    }

    /* ── BUTTON: Instalar ahora ───────────────────────────────────────── */
    {
        int bw = 160, bh = 42;
        int bx = cx + cw - bw - 24;
        int by = cy + ch - bh - 20;
        int fi = ui_push_button(22, bx, by, bw, bh, cb_disk_install);
        draw_labeled_button(bx, by, bw, bh, "Instalar ahora",
                            RGB(40, 168, 98), RGB(15, 90, 50),
                            fi >= 0 && focused_element_index == fi);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 3 — INSTALANDO
 *  Widget: PROGRESS_BAR + log de consola.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_installing(int cx, int cy, int cw, int ch) {
    int     pct;
    int     bar_x, bar_y, bar_w, bar_h;
    int     log_y;
    uint64_t elapsed;

    if (!install_timer_armed) {
        install_start_tick = ticks;
        install_timer_armed = 1;
    }
    elapsed = ticks - install_start_tick;
    pct = (int)(elapsed * 100 / INSTALL_DURATION_TICKS);
    if (pct > 100) pct = 100;
    if (pct >= 100) {
        current_step        = FINISHED;
        install_timer_armed = 0;
        return;
    }

    gfx_draw_text_aa(cx + 20, cy + 16, "Instalando NexusOS",
                     RGB(28, 32, 44), 2);
    gfx_draw_text_hq(cx + 20, cy + 48,
                     "No apague el equipo durante la instalacion.",
                     RGB(100, 55, 55));

    /* ── PROGRESS_BAR widget ───────────────────────────────────────── */
    bar_x = cx + 32;
    bar_y = cy + ch / 2 - 50;
    bar_w = cw - 64;
    bar_h = 26;
    if (bar_w < 80) bar_w = 80;

    /* Registrar widget con el progreso actual (se renderiza en ui_draw_all_elements) */
    ui_push_progress_bar(30, bar_x, bar_y, bar_w, bar_h, pct);

    /* ── Log de consola ──────────────────────────────────────────────── */
    log_y = bar_y + bar_h + 36;
    gfx_fill_rect(cx + 20, log_y, cw - 40, ch - (log_y - cy) - 16, RGB(24, 26, 32));
    gfx_rounded_rect_stroke_aa(cx + 20, log_y, cw - 40, ch - (log_y - cy) - 16,
                                6, RGB(60, 65, 78));
    {
        int ly = log_y + 10;
        gfx_draw_text_aa(cx + 28, ly,     "> copiando archivos del sistema...", RGB(180, 220, 180), 1); ly += 16;
        gfx_draw_text_aa(cx + 28, ly,     "> configurando initramfs...",        RGB(160, 200, 160), 1); ly += 16;
        gfx_draw_text_aa(cx + 28, ly,     "> instalando GRUB...",               RGB(140, 180, 140), 1); ly += 16;
        if (pct >= 18) { gfx_draw_text_aa(cx + 28, ly, "> extrayendo squashfs...", RGB(120, 170, 120), 1); ly += 16; }
        if (pct >= 40) { gfx_draw_text_aa(cx + 28, ly, "> generando fstab...",    RGB(100, 160, 100), 1); ly += 16; }
        if (pct >= 62) { gfx_draw_text_aa(cx + 28, ly, "> sincronizando disco...", RGB(90, 150, 90),  1); ly += 16; }
        if (pct >= 82) { gfx_draw_text_aa(cx + 28, ly, "> finalizando...",         RGB(80, 145, 90),  1); }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 4 — FINALIZADO
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_finished(int cx, int cy, int cw, int ch) {
    int         mx  = cx + cw / 2;
    int         my  = cy + 70;
    const char* l1  = "Instalacion completada.";
    const char* l2  = "Por favor, extraiga el medio de instalacion";
    const char* l3  = "y reinicie el equipo.";
    int tw, y;

    gfx_fill_circle(mx, my, 36, RGB(46, 180, 80));
    gfx_circle_outline_aa(mx, my, 36, RGB(20, 100, 40));
    gfx_wu_line(mx - 14, my, mx - 4, my + 14, RGB(255, 255, 255));
    gfx_wu_line(mx - 4, my + 14, mx + 22, my - 18, RGB(255, 255, 255));

    y  = my + 52;
    tw = gfx_text_width_aa(l1, 2);
    gfx_draw_text_aa(mx - tw/2, y, l1, RGB(28, 32, 44), 2);
    y += 36;
    tw = gfx_text_width_hq(l2);
    gfx_draw_text_hq(cx + (cw - tw)/2, y, l2, RGB(55, 60, 72));
    y += 22;
    tw = gfx_text_width_hq(l3);
    gfx_draw_text_hq(cx + (cw - tw)/2, y, l3, RGB(55, 60, 72));

    {
        int bw = 170, bh = 42;
        int bx = cx + cw - bw - 24;
        int by = cy + ch - bh - 20;
        int fi = ui_push_button(4, bx, by, bw, bh, cb_reboot);
        draw_labeled_button(bx, by, bw, bh, "Reiniciar ahora",
                            RGB(0, 122, 245), RGB(20, 60, 140),
                            fi >= 0 && focused_element_index == fi);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Punto de entrada: repintar el contenido del instalador (llamado cada frame).
 * ═══════════════════════════════════════════════════════════════════════════ */
void draw_installer_content(const Window* win) {
    int cx, cy, cw, ch;
    static InstallerState last_step = INSTALLING;

    if (!win || !win->is_visible) return;
    installer_client_rect(win, &cx, &cy, &cw, &ch);
    if (cw < 80 || ch < 60) return;

    /* Resetear lista de widgets cada frame. */
    ui_element_count = 0;
    if (current_step != last_step) {
        focused_element_index = 0;
        last_step             = current_step;
    }
    if (current_step != INSTALLING)
        install_timer_armed = 0;

    /* Dibujar el contenido del paso actual (que también registra los widgets). */
    switch (current_step) {
    case WELCOME:    draw_welcome       (cx, cy, cw, ch); break;
    case TIMEZONE:   draw_timezone_step (cx, cy, cw, ch); break;
    case DISK_SETUP: draw_disk_setup    (cx, cy, cw, ch); break;
    case INSTALLING: draw_installing    (cx, cy, cw, ch); break;
    case FINISHED:   draw_finished      (cx, cy, cw, ch); break;
    default: break;
    }

    /*
     * Dibujar todos los widgets no-BUTTON (TEXT_INPUT, CHECKBOX, PROGRESS_BAR).
     * Se hace DESPUÉS del switch para que aparezcan encima del fondo de cada paso.
     */
    ui_draw_all_elements();
}
