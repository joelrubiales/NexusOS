/*
 * Asistente visual NexusOS — flujo inspirado en Calamares, macOS y GTK4.
 *
 * Pasos: Bienvenida → Localización → Usuario → Disco → Resumen →
 *        Instalación (TAR real + slideshow) → Finalizado.
 *
 * Barra lateral + tarjeta de contenido (ver comentarios anteriores en el repo).
 */
#include "installer_ui.h"
#include "gui.h"
#include "gfx.h"
#include "ui_manager.h"
#include "mouse.h"
#include "nexus.h"
#include "font_aa.h"
#include "vfs.h"
#include <stddef.h>

Image* installer_bg_image;
Image* installer_window_bg;
Image* installer_btn_hover;

void installer_paint_background_fullscreen(void) {
    int sw = gfx_width();
    int sh = gfx_height();
    if (sw < 1 || sh < 1)
        return;
    if (installer_bg_image && installer_bg_image->pixels)
        gui_draw_image_stretch(0, 0, sw, sh, installer_bg_image);
    else
        gfx_fill_screen_solid((unsigned int)(COLOR_DESKTOP_BG & 0xFFFFFFu));
}

/* Ancho de la barra lateral en píxeles lógicos. */
#define SIDEBAR_W_MIN  100
#define SIDEBAR_W_MAX  200
#define SIDEBAR_W_FRAC 4   /* 1/4 del ancho del contenido */

#define INS_RGB(c)      ((unsigned int)((c) & 0xFFFFFFu))
#define OPAQUE_ARGB(c)  ((uint32_t)(((c) & 0xFFFFFFu) | 0xFF000000u))

/* Botones: padding mínimo vertical recomendado GTK (~10 px). */
#define BTN_PAD_MIN_V   10
#define BTN_PAD_MIN_H   10
#define BTN_CORNER_R    10

/* Radio del círculo de indicador de paso. */
#define STEP_DOT_R 7

#define SIDEBAR_DONE    RGB(46, 155, 95)
#define SIDEBAR_MUTED   RGB(130, 135, 145)

InstallerState current_step = WELCOME;

/* ── Datos persistidos entre pasos (evita depender de slots de widgets) ─── */
static char ins_hostname[64];
static char ins_display_name[64];
static char ins_username[64];
static char ins_password[64];

static int  ins_dual_boot;       /* Instalar junto a Windows */
static int  ins_erase_all;       /* Borrar todo el disco */
static int  ins_show_warn_modal; /* Diálogo destructivo GTK */

static void copy_input_by_id(int id, char* dst, int cap) {
    int i, j;
    if (!dst || cap <= 0) return;
    dst[0] = 0;
    for (i = 0; i < ui_element_count; i++) {
        if (ui_elements[i].type != UI_TYPE_TEXT_INPUT) continue;
        if (ui_elements[i].id != id) continue;
        for (j = 0; j < cap - 1 && ui_elements[i].text_buffer[j]; j++)
            dst[j] = ui_elements[i].text_buffer[j];
        dst[j] = 0;
        return;
    }
}

static void restore_text_input(int idx, const char* s) {
    int j;
    if (idx < 0 || idx >= ui_element_count) return;
    if (ui_elements[idx].type != UI_TYPE_TEXT_INPUT) return;
    if (!s || !s[0]) return;
    for (j = 0; j < 255 && s[j]; j++)
        ui_elements[idx].text_buffer[j] = s[j];
    ui_elements[idx].text_buffer[j] = 0;
    ui_elements[idx].text_len = j;
}

/* ── Callbacks de navegación ─────────────────────────────────────────────── */
static void cb_welcome_next(void) { current_step = LOCALE; }

static void cb_locale_next(void) { current_step = USER_ACCOUNT; }

static void cb_user_back(void) { current_step = LOCALE; }
static void cb_user_next(void) {
    copy_input_by_id(200, ins_hostname,     sizeof ins_hostname);
    copy_input_by_id(201, ins_display_name, sizeof ins_display_name);
    copy_input_by_id(202, ins_username,     sizeof ins_username);
    copy_input_by_id(203, ins_password,     sizeof ins_password);
    if (ins_hostname[0] == 0) {
        ins_hostname[0] = 'n'; ins_hostname[1] = 'e'; ins_hostname[2] = 'x';
        ins_hostname[3] = 'u'; ins_hostname[4] = 's'; ins_hostname[5] = 'o';
        ins_hostname[6] = 's'; ins_hostname[7] = 0;
    }
    if (ins_username[0] == 0) {
        ins_username[0] = 'u'; ins_username[1] = 's'; ins_username[2] = 'e';
        ins_username[3] = 'r'; ins_username[4] = 0;
    }
    current_step = DISK_SETUP;
}

static void cb_disk_back(void) { current_step = USER_ACCOUNT; }
static void cb_disk_next(void) { current_step = SUMMARY; }

static void cb_summary_back(void) {
    ins_show_warn_modal = 0;
    current_step = DISK_SETUP;
}
static void cb_summary_install(void) {
    if (ins_erase_all)
        ins_show_warn_modal = 1;
    else
        current_step = INSTALLING;
}

static void cb_warn_cancel(void) { ins_show_warn_modal = 0; }
static void cb_warn_confirm(void) {
    ins_show_warn_modal = 0;
    current_step = INSTALLING;
}

static void cb_reboot(void) {
    outb(0x64u, 0xFEu);
    for (;;) __asm__ volatile("hlt");
}

static void cb_view_log(void) {
    /* Placeholder: en un SO completo abriría el registro; aquí no-op. */
}

/* ── Motor de extracción TAR ─────────────────────────────────────────────── */
/*
 * Avanza 2 KiB por frame × 30 fps ≈ 60 KiB/s.
 * Con ~415 KiB de payload (sistema + assets) → ~7 s de instalación.
 */
#define EXTRACT_BYTES_PER_FRAME 2048u
#define EXTRACT_LOG_N           7

/* Offset en el TAR (avanza cuando se termina de "extraer" un archivo). */
static uint32_t extr_tar_off      = 0;
/* Bytes totales en el TAR (calculado al inicio de la instalación). */
static uint32_t extr_total        = 0;
/* Bytes "extraídos" hasta ahora. */
static uint32_t extr_done         = 0;
/* Bytes que quedan por extraer del archivo actual. */
static uint32_t extr_remain       = 0;
/* Porcentaje 0–100. */
static int      extr_pct          = 0;
/* 1 cuando se han procesado todos los archivos. */
static int      extr_complete     = 0;
/* 0 = necesita reiniciar la próxima vez que entremos en INSTALLING. */
static int      extr_init_done    = 0;

/* Ring buffer de líneas de log. */
static char extr_log[EXTRACT_LOG_N][120];
static int  extr_log_start = 0;   /* índice del registro más antiguo */
static int  extr_log_count = 0;   /* número de registros válidos */

/* Fallback con ticks para cuando no hay initrd. */
#define INSTALL_DURATION_TICKS (10ull * (uint64_t)PIT_TICKS_PER_SEC)
static uint64_t install_start_tick;
static int      install_timer_armed;

/* ── Helpers de extracción ────────────────────────────────────────────────── */

/* Añade una nueva entrada al ring buffer del log ("> copiando: <name>"). */
static void extr_log_push(const char* name) {
    char*       dst;
    int         di = 0, i;
    const char* prefix = "> copiando: ";
    int         slot;

    if (extr_log_count < EXTRACT_LOG_N) {
        slot = (extr_log_start + extr_log_count) % EXTRACT_LOG_N;
        extr_log_count++;
    } else {
        /* Sobreescribir la entrada más antigua. */
        slot = extr_log_start;
        extr_log_start = (extr_log_start + 1) % EXTRACT_LOG_N;
    }
    dst = extr_log[slot];
    for (i = 0; prefix[i] && di < 118; i++) dst[di++] = prefix[i];
    for (i = 0; name[i]   && di < 118; i++) dst[di++] = name[i];
    dst[di] = '\0';
}

/* Avanza la extracción en EXTRACT_BYTES_PER_FRAME bytes. */
static void extr_advance_frame(void) {
    uint32_t  advance;
    VFS_Node  node;

    if (extr_complete) return;

    if (extr_remain == 0) {
        /* Pasar al siguiente archivo del TAR. */
        if (tar_next_entry(&extr_tar_off, &node)) {
            extr_remain = (node.size > 0) ? node.size : 1u;
            extr_log_push(node.name);
        } else {
            /* Fin del TAR — instalación completa. */
            extr_complete = 1;
            extr_done     = extr_total;
            extr_pct      = 100;
            return;
        }
    }

    /* Consumir EXTRACT_BYTES_PER_FRAME del archivo actual. */
    advance = (extr_remain > EXTRACT_BYTES_PER_FRAME)
                  ? EXTRACT_BYTES_PER_FRAME
                  : extr_remain;
    extr_remain -= advance;
    extr_done   += advance;

    if (extr_total > 0) {
        extr_pct = (int)((uint64_t)extr_done * 100u / extr_total);
        if (extr_pct > 99) extr_pct = 99;   /* 100 % solo al completar */
    }
}

/* ── Helpers de layout ───────────────────────────────────────────────────── */
static void installer_client_rect(const Window* w,
                                  int* cx, int* cy, int* cw, int* ch) {
    *cx = w->x + INSTALLER_WIN_INSET;
    *cy = w->y + INSTALLER_WIN_TITLE_H + 4;
    *cw = w->width  - 2 * INSTALLER_WIN_INSET;
    *ch = w->height - INSTALLER_WIN_TITLE_H - 10;
}

/* Radio y sombra del panel principal (estilo ventana macOS / glass). */
#define INSTALLER_SHELL_RADIUS  12
#define INSTALLER_SHADOW_SPREAD 20

/*
 * Panel flotante: sombra redondeada profunda + relleno glass + título.
 */
static void installer_draw_floating_shell(const Window* w) {
    int         x, y, ww, hh;
    const char* t;
    int         R = INSTALLER_SHELL_RADIUS;

    if (!w || !w->is_visible)
        return;
    x  = w->x;
    y  = w->y;
    ww = w->width;
    hh = w->height;
    if (ww < 120 || hh < 80)
        return;

    if (installer_window_bg && installer_window_bg->pixels) {
        gui_draw_image_stretch(x, y, ww, hh, installer_window_bg);
    } else {
        draw_drop_shadow(x, y, ww, hh, R, INSTALLER_SHADOW_SPREAD, 0xFF2C2C2Eu);
        draw_rounded_rect_filled(x, y, ww, hh, R, COLOR_BG_WINDOW);

        if (hh > INSTALLER_WIN_TITLE_H + 16) {
            int mx = x + 2;
            int my = y + INSTALLER_WIN_TITLE_H - 2;
            int mw = ww - 4;
            int mh = hh - INSTALLER_WIN_TITLE_H;
            int mr = R > 6 ? R - 4 : 4;
            if (mr > mw / 2)
                mr = mw / 2;
            if (mr > mh / 2)
                mr = mh / 2;
            gfx_rect_mica(mx, my, mw, mh, mr, 0xFFFFFFu, 38u);
        }
    }

    t = w->title ? w->title : "Instalador";
    {
        int tw = gfx_aa_text_w(t, 1);
        int tx = x + (ww - tw) / 2;
        int ty = y + 16;
        if (tx < x + 24)
            tx = x + 24;
        gfx_aa_text(tx, ty, t, INS_RGB(COLOR_TEXT_PRIMARY), 1);
    }
    gfx_blend_rect(x + R + 4, y + 46, ww - 2 * (R + 4), 2, INS_RGB(COLOR_BORDER), 34);
}

/*
 * Hitbox invisible: solo etiqueta + imagen hover (asset BMP). btn_style afecta al color del texto.
 * elem_idx: índice en ui_elements[]; si < 0 no hay hit-test de hover.
 */
static void draw_installer_button(int x, int y, int bw, int bh, const char* label, int btn_style,
                                  int elem_idx) {
    int          tw = gfx_aa_text_w(label, 1);
    int          tx = x + (bw - tw) / 2;
    int          ty = y + (bh - FONT_AA_GLYPH_H) / 2;
    unsigned int fg;

    if (bh < FONT_AA_GLYPH_H + 2 * BTN_PAD_MIN_V)
        ty = y + (bh - FONT_AA_GLYPH_H) / 2;

    if (elem_idx >= 0 && ui_manager_element_is_hovered(elem_idx) && installer_btn_hover
        && installer_btn_hover->pixels)
        gui_draw_image_stretch(x, y, bw, bh, installer_btn_hover);

    if (btn_style == 2)
        fg = RGB(255, 252, 252);
    else if (btn_style == 1)
        fg = RGB(255, 255, 255);
    else
        fg = INS_RGB(COLOR_TEXT_PRIMARY);

    gfx_aa_text(tx, ty, label, fg, 1);
}

static void draw_lang_option(int x, int y, int w, int h,
                             const char* name, int selected) {
    unsigned int fg = selected ? INS_RGB(COLOR_ACCENT) : INS_RGB(COLOR_TEXT_PRIMARY);
    gfx_fill_rounded_rect_aa(x, y, w, h, 8, OPAQUE_ARGB(COLOR_BG_WINDOW));
    if (selected)
        gfx_blend_rect(x, y, w, h, INS_RGB(COLOR_ACCENT), 22);
    else
        gfx_blend_rect(x, y, w, h, RGB(248, 249, 251), 55);
    gfx_aa_text(x + 14, y + (h - FONT_AA_GLYPH_H) / 2, name, fg, 1);
    if (selected)
        gfx_aa_text(x + w - 36, y + (h - FONT_AA_GLYPH_H) / 2,
                    "[*]", INS_RGB(COLOR_ACCENT), 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 0 — WELCOME
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_welcome(int cx, int cy, int cw, int ch) {
    const char* headline    = "Bienvenido a NexusOS";
    int         title_scale = 2;
    int         tw  = gfx_aa_text_w(headline, title_scale);
    int         tx  = cx + (cw - tw) / 2;
    int         ty  = cy + 24;
    if (tx < cx + 8) tx = cx + 8;

    gfx_aa_text(tx, ty, headline, INS_RGB(COLOR_TEXT_PRIMARY), title_scale);
    gfx_aa_text(cx + 24, ty + gfx_aa_line_h(title_scale) + 8,
                "Elija el idioma del asistente y del sistema instalado.",
                SIDEBAR_MUTED, 1);

    {
        int ly  = ty + gfx_aa_line_h(title_scale) + 44;
        int lh  = 36;
        int gap = 10;
        /* Mini “banderas” vectoriales: rectángulos tricolor estilizados */
        {
            int fx = cx + 28;
            int fy = ly + lh / 2 - 6;
            gfx_fill_rect(fx,     fy, 5, 12, RGB(170, 0, 0));
            gfx_fill_rect(fx + 6, fy, 5, 12, RGB(240, 180, 0));
            gfx_fill_rect(fx + 12, fy, 5, 12, RGB(170, 0, 0));
        }
        draw_lang_option(cx + 52, ly,                  cw - 76, lh, "Espanol (Espana)", 1);
        {
            int fx = cx + 28;
            int fy = ly + lh + gap + lh / 2 - 6;
            gfx_fill_rect(fx,     fy, 5, 12, RGB(40, 60, 140));
            gfx_fill_rect(fx + 6, fy, 5, 12, RGB(240, 240, 240));
            gfx_fill_rect(fx + 12, fy, 5, 12, RGB(200, 40, 40));
        }
        draw_lang_option(cx + 52, ly + lh + gap,       cw - 76, lh, "English (US)",     0);
        {
            int fx = cx + 28;
            int fy = ly + 2 * (lh + gap) + lh / 2 - 6;
            gfx_fill_rect(fx,     fy, 5, 12, RGB(200, 40, 40));
            gfx_fill_rect(fx + 6, fy, 5, 12, RGB(240, 180, 0));
            gfx_fill_rect(fx + 12, fy, 5, 12, RGB(200, 40, 40));
        }
        draw_lang_option(cx + 52, ly + 2 * (lh + gap), cw - 76, lh, "Catala",           0);
    }

    {
        int bw = 168, bh = 44;
        int bx = cx + cw - bw - 24;
        int by = cy + ch - bh - 20;
        int fi = ui_push_button(1, bx, by, bw, bh, cb_welcome_next);
        ui_manager_sync_from_elements();
        ui_manager_update_hover((int)mouse_get_x(), (int)mouse_get_y());
        draw_installer_button(bx, by, bw, bh, "Continuar", 1, fi);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 1 — LOCALIZACIÓN (mapa + huso simulado, estilo macOS)
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_locale_step(int cx, int cy, int cw, int ch) {
    int mx, my, mw, mh;

    gfx_aa_text(cx + 20, cy + 16, "Region y zona horaria",
                INS_RGB(COLOR_TEXT_PRIMARY), 2);
    gfx_aa_text(cx + 20, cy + 16 + gfx_aa_line_h(2) + 4,
                "Seleccione su ubicacion. El instalador usara Europa/Madrid por defecto.",
                SIDEBAR_MUTED, 1);

    /* Mapa mundial estilizado (elipse + grilla + marcadores) */
    mx = cx + 24;
    my = cy + 88;
    mw = cw - 48;
    mh = ch - 160;
    if (mh < 80) mh = 80;

    gfx_fill_rounded_rect(mx, my, mw, mh, 12, RGB(38, 42, 52));
    gfx_rounded_rect_stroke_aa(mx, my, mw, mh, 12, RGB(70, 78, 98));

    /* Meridianos / paralelos sutiles */
    {
        int i;
        for (i = 1; i < 5; i++) {
            int vx = mx + (mw * i) / 5;
            gfx_blend_rect(vx, my + 8, 1, mh - 16, RGB(90, 100, 120), 40);
        }
        for (i = 1; i < 4; i++) {
            int hy = my + (mh * i) / 4;
            gfx_blend_rect(mx + 8, hy, mw - 16, 1, RGB(90, 100, 120), 40);
        }
    }

    /* Marcadores (puntos azules) */
    gfx_fill_circle(mx + mw * 48 / 100, my + mh * 35 / 100, 5, RGB(0, 140, 255));
    gfx_fill_circle(mx + mw * 72 / 100, my + mh * 42 / 100, 5, RGB(0, 140, 255));
    gfx_fill_circle(mx + mw * 22 / 100, my + mh * 38 / 100, 5, RGB(0, 140, 255));

    /* Pop-over simulado */
    gfx_drop_shadow_soft(mx + mw / 2 - 70, my + mh / 2 - 36, 200, 52, 10, 6);
    gfx_fill_rounded_rect_aa(mx + mw / 2 - 70, my + mh / 2 - 36, 200, 52, 12,
                             RGB(252, 253, 255));
    gfx_rounded_rect_stroke_aa(mx + mw / 2 - 70, my + mh / 2 - 36, 200, 52, 12,
                               RGB(200, 205, 220));
    gfx_aa_text(mx + mw / 2 - 58, my + mh / 2 - 22,
                "Huso horario: Europa/Madrid", RGB(40, 44, 55), 1);

    /* Desplegable simulado */
    {
        int dx = cx + 24, dy = my + mh + 14, dw = cw - 48, dh = 36;
        gfx_fill_rounded_rect(dx, dy, dw, dh, 8, RGB(248, 249, 252));
        gfx_rounded_rect_stroke_aa(dx, dy, dw, dh, 8, RGB(180, 186, 200));
        gfx_aa_text(dx + 12, dy + (dh - FONT_AA_GLYPH_H) / 2,
                    "Espana (Espana) — Catala disponible", RGB(50, 54, 68), 1);
        /* Flecha del combo */
        gfx_wu_line(dx + dw - 22, dy + dh / 2 - 3, dx + dw - 16, dy + dh / 2 + 3,
                    RGB(90, 95, 110));
        gfx_wu_line(dx + dw - 16, dy + dh / 2 + 3, dx + dw - 10, dy + dh / 2 - 3,
                    RGB(90, 95, 110));
    }

    {
        int bw = 168, bh = 44;
        int bx = cx + cw - bw - 24;
        int by = cy + ch - bh - 20;
        int fi = ui_push_button(100, bx, by, bw, bh, cb_locale_next);
        ui_manager_sync_from_elements();
        ui_manager_update_hover((int)mouse_get_x(), (int)mouse_get_y());
        draw_installer_button(bx, by, bw, bh, "Continuar", 1, fi);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 2 — CUENTA DE USUARIO (estilo GNOME / GTK4)
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_user_account_step(int cx, int cy, int cw, int ch) {
    int field_w  = cw - 40;
    int field_h  = 44;
    int field_x  = cx + 20;
    int label_y, field_y;
    int idx;

    gfx_aa_text(cx + 20, cy + 16, "Cree su cuenta",
                INS_RGB(COLOR_TEXT_PRIMARY), 2);
    gfx_aa_text(cx + 20, cy + 16 + gfx_aa_line_h(2) + 4,
                "Nombre para mostrar, usuario, contrasena y nombre del equipo.",
                SIDEBAR_MUTED, 1);

    label_y = cy + 88;
    field_y = label_y + FONT_AA_GLYPH_H + 6;

    gfx_aa_text(field_x, label_y, "Nombre del equipo (hostname):",
                SIDEBAR_MUTED, 1);
    idx = ui_push_text_input(200, field_x, field_y, field_w, field_h,
                              "nexusos", 0);
    if (ins_hostname[0]) restore_text_input(idx, ins_hostname);

    label_y = field_y + field_h + 18;
    field_y = label_y + FONT_AA_GLYPH_H + 6;
    gfx_aa_text(field_x, label_y, "Su nombre:", SIDEBAR_MUTED, 1);
    idx = ui_push_text_input(201, field_x, field_y, field_w, field_h,
                              "Linus Torvalds", 0);
    if (ins_display_name[0]) restore_text_input(idx, ins_display_name);

    label_y = field_y + field_h + 18;
    field_y = label_y + FONT_AA_GLYPH_H + 6;
    gfx_aa_text(field_x, label_y, "Nombre de usuario:", SIDEBAR_MUTED, 1);
    idx = ui_push_text_input(202, field_x, field_y, field_w, field_h,
                              "linus", 0);
    if (ins_username[0]) restore_text_input(idx, ins_username);

    label_y = field_y + field_h + 18;
    field_y = label_y + FONT_AA_GLYPH_H + 6;
    gfx_aa_text(field_x, label_y, "Contrasena:", SIDEBAR_MUTED, 1);
    idx = ui_push_text_input(203, field_x, field_y, field_w, field_h,
                              "********", 1);
    if (ins_password[0]) restore_text_input(idx, ins_password);

    {
        int bw0 = 132, bh0 = 44;
        int bx0 = cx + 24;
        int by0 = cy + ch - bh0 - 20;
        int bw1 = 168, bh1 = 44;
        int bx1 = cx + cw - bw1 - 24;
        int fi_b = ui_push_button(204, bx0, by0, bw0, bh0, cb_user_back);
        int fi_n = ui_push_button(205, bx1, by0, bw1, bh1, cb_user_next);
        ui_manager_sync_from_elements();
        ui_manager_update_hover((int)mouse_get_x(), (int)mouse_get_y());
        draw_installer_button(bx0, by0, bw0, bh0, "Atras", 0, fi_b);
        draw_installer_button(bx1, by0, bw1, bh1, "Continuar", 1, fi_n);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 3 — DISCO (barra tipo GParted / Calamares)
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_disk_setup(int cx, int cy, int cw, int ch) {
    int disk_y = cy + 20;
    int bar_y, bar_h = 56;
    int bar_x = cx + 24;
    int bar_w = cw - 48;
    int w_win, w_free;
    int fi, i;

    gfx_aa_text(cx + 20, disk_y, "Particionado del disco",
                INS_RGB(COLOR_TEXT_PRIMARY), 2);
    disk_y += gfx_aa_line_h(2) + 8;

    gfx_aa_text(cx + 20, disk_y, "/dev/sda  (100 GB) — ATA Disk",
                RGB(70, 75, 90), 1);
    disk_y += gfx_aa_line_h(1) + 10;

    bar_y = disk_y;
    gfx_fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 10, RGB(210, 214, 222));
    gfx_rounded_rect_stroke_aa(bar_x, bar_y, bar_w, bar_h, 10, RGB(150, 158, 175));

    w_win  = bar_w * 30 / 100;
    w_free = bar_w - w_win - 8;
    if (w_win < 40) w_win = 40;
    w_free = bar_w - w_win - 8;
    gfx_fill_rounded_rect(bar_x + 4, bar_y + 4, w_win, bar_h - 8, 8,
                          RGB(230, 120, 40));
    gfx_rounded_rect_stroke_aa(bar_x + 4, bar_y + 4, w_win, bar_h - 8, 8,
                               RGB(180, 80, 20));
    gfx_aa_text(bar_x + w_win / 2 - gfx_aa_text_w("Windows", 1) / 2,
                bar_y + (bar_h - FONT_AA_GLYPH_H) / 2,
                "Windows", RGB(255, 255, 255), 1);

    gfx_fill_rounded_rect(bar_x + 4 + w_win + 4, bar_y + 4, w_free, bar_h - 8, 8,
                          RGB(200, 205, 215));
    gfx_rounded_rect_stroke_aa(bar_x + 4 + w_win + 4, bar_y + 4, w_free, bar_h - 8, 8,
                               RGB(160, 168, 182));
    gfx_aa_text(bar_x + 4 + w_win + 4 + w_free / 2
                - gfx_aa_text_w("Espacio libre", 1) / 2,
                bar_y + (bar_h - FONT_AA_GLYPH_H) / 2,
                "Espacio libre", RGB(70, 74, 85), 1);

    gfx_aa_text(bar_x, bar_y + bar_h + 6, "30 GB  ·  70 GB libres",
                RGB(95, 100, 115), 1);

    {
        int oy    = bar_y + bar_h + 36;
        int lbl_w = 24 + 12 + gfx_aa_text_w("Instalar junto a Windows", 1);
        fi = ui_push_checkbox(30, cx + 20, oy, lbl_w,
                              "Instalar junto a Windows", NULL);
        (void)fi;
    }
    {
        int oy    = bar_y + bar_h + 36 + 40;
        int lbl_w = 24 + 12 + gfx_aa_text_w("Borrar todo el disco e instalar NexusOS", 1);
        fi = ui_push_checkbox(31, cx + 20, oy, lbl_w,
                              "Borrar todo el disco e instalar NexusOS", NULL);
        (void)fi;
    }

    ins_dual_boot = 0;
    ins_erase_all = 0;
    for (i = 0; i < ui_element_count; i++) {
        if (ui_elements[i].type != UI_TYPE_CHECKBOX) continue;
        if (ui_elements[i].id == 30) ins_dual_boot = ui_elements[i].is_checked;
        if (ui_elements[i].id == 31) ins_erase_all  = ui_elements[i].is_checked;
    }

    {
        int bw0 = 132, bh0 = 44;
        int bx0 = cx + 24;
        int by0 = cy + ch - bh0 - 20;
        int bw1 = 168, bh1 = 44;
        int bx1 = cx + cw - bw1 - 24;
        int fi0 = ui_push_button(32, bx0, by0, bw0, bh0, cb_disk_back);
        int fi1 = ui_push_button(33, bx1, by0, bw1, bh1, cb_disk_next);
        ui_manager_sync_from_elements();
        ui_manager_update_hover((int)mouse_get_x(), (int)mouse_get_y());
        draw_installer_button(bx0, by0, bw0, bh0, "Atras", 0, fi0);
        draw_installer_button(bx1, by0, bw1, bh1, "Continuar", 1, fi1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 4 — RESUMEN (Calamares)
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_summary_step(int cx, int cy, int cw, int ch) {
    int lx = cx + 32;
    int rx = cx + cw / 2 + 8;
    int y;
    char line[96];
    int p;
    const char* a;

    gfx_aa_text(cx + 20, cy + 16, "Resumen de la instalacion",
                INS_RGB(COLOR_TEXT_PRIMARY), 2);
    gfx_aa_text(cx + 20, cy + 16 + gfx_aa_line_h(2) + 4,
                "Revise los datos antes de escribir en el disco.",
                SIDEBAR_MUTED, 1);

    y = cy + 72;
    gfx_aa_text(lx, y, "Idioma",     RGB(110, 115, 130), 1);
    gfx_aa_text(rx, y, "Espanol (Espana)", RGB(40, 44, 58), 1);
    y += gfx_aa_line_h(1) + 8;
    gfx_aa_text(lx, y, "Teclado",    RGB(110, 115, 130), 1);
    gfx_aa_text(rx, y, "US QWERTY",  RGB(40, 44, 58), 1);
    y += gfx_aa_line_h(1) + 8;
    gfx_aa_text(lx, y, "Usuario",    RGB(110, 115, 130), 1);
    gfx_aa_text(rx, y, ins_username[0] ? ins_username : "(usuario)",
                RGB(40, 44, 58), 1);
    y += gfx_aa_line_h(1) + 8;
    gfx_aa_text(lx, y, "Nombre",     RGB(110, 115, 130), 1);
    gfx_aa_text(rx, y, ins_display_name[0] ? ins_display_name : "-",
                RGB(40, 44, 58), 1);
    y += gfx_aa_line_h(1) + 8;
    gfx_aa_text(lx, y, "Equipo",     RGB(110, 115, 130), 1);
    gfx_aa_text(rx, y, ins_hostname[0] ? ins_hostname : "nexusos",
                RGB(40, 44, 58), 1);
    y += gfx_aa_line_h(1) + 8;
    gfx_aa_text(lx, y, "Disco",      RGB(110, 115, 130), 1);
    p = 0;
    a = "/dev/sda (70 GB para Nexus)";
    while (a[p] && p < (int)sizeof(line) - 1) { line[p] = a[p]; p++; }
    line[p] = 0;
    gfx_aa_text(rx, y, line, RGB(40, 44, 58), 1);
    y += gfx_aa_line_h(1) + 8;
    gfx_aa_text(lx, y, "Arranque",   RGB(110, 115, 130), 1);
    gfx_aa_text(rx, y, "GRUB en sda — EFI opcional",
                RGB(40, 44, 58), 1);

    if (ins_show_warn_modal)
        return;

    {
        int bw0 = 120, bh0 = 42;
        int bx0 = cx + 24;
        int by0 = cy + ch - bh0 - 20;
        int bw1 = 196, bh1 = 44;
        int bx1 = cx + cw - bw1 - 24;
        int fi0 = ui_push_button(40, bx0, by0, bw0, bh0, cb_summary_back);
        int fi1 = ui_push_button(41, bx1, by0, bw1, bh1, cb_summary_install);
        ui_manager_sync_from_elements();
        ui_manager_update_hover((int)mouse_get_x(), (int)mouse_get_y());
        draw_installer_button(bx0, by0, bw0, bh0, "Atras", 0, fi0);
        draw_installer_button(bx1, by0, bw1, bh1, "Instalar ahora", 1, fi1);
    }
}

static void draw_destructive_modal(int cx, int cy, int cw, int ch) {
    int mx, my, mw, mh;

    gfx_blend_rect(cx, cy, cw, ch, RGB(10, 10, 18), 140);

    mw = cw * 72 / 100;
    if (mw < 320) mw = 320;
    if (mw > 440) mw = 440;
    mh = 200;
    mx = cx + (cw - mw) / 2;
    my = cy + (ch - mh) / 2;

    draw_drop_shadow(mx, my, mw, mh, 16, 16, 0xFF2C2C2Eu);
    draw_rounded_rect_filled(mx, my, mw, mh, 16, ARGB(255, 62, 36, 38));

    {
        int tx = mx + 36, ty = my + 36;
        int i;
        for (i = 0; i < 22; i++) {
            int py = ty + i;
            int half = (i < 11) ? i : (21 - i);
            gfx_blend_rect(tx + 11 - half, py, half * 2 + 1, 1,
                           RGB(255, 160, 40), 220);
        }
        gfx_fill_circle(tx + 11, ty + 8, 2, RGB(40, 20, 10));
    }

    gfx_aa_text(mx + 72, my + 28, "ADVERTENCIA: Accion destructiva",
                RGB(255, 240, 240), 1);
    gfx_aa_text(mx + 28, my + 56,
                "Esta opcion borrara particiones en /dev/sda (100 GB).",
                RGB(235, 210, 210), 1);
    gfx_aa_text(mx + 28, my + 56 + gfx_aa_line_h(1),
                "Todos los datos seleccionados se perderan. Continuar?",
                RGB(235, 210, 210), 1);

    {
        int bw0 = 132, bh0 = 42;
        int bx0 = mx + 24;
        int by0 = my + mh - bh0 - 20;
        int bw1 = 148, bh1 = 42;
        int bx1 = mx + mw - bw1 - 24;
        int fi0 = ui_push_button(300, bx0, by0, bw0, bh0, cb_warn_cancel);
        int fi1 = ui_push_button(301, bx1, by0, bw1, bh1, cb_warn_confirm);
        ui_manager_sync_from_elements();
        ui_manager_update_hover((int)mouse_get_x(), (int)mouse_get_y());
        draw_installer_button(bx0, by0, bw0, bh0, "Cancelar", 0, fi0);
        draw_installer_button(bx1, by0, bw1, bh1, "Borrar disco", 2, fi1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 5 — INSTALANDO (TAR + slideshow marketing)
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_installing(int cx, int cy, int cw, int ch) {
    int bar_x, bar_y, bar_w, bar_h, log_y;
    int slide;
    const char *st0, *st1, *sub;

    /* ── Inicializar el motor de extracción (primera llamada) ───────── */
    if (!extr_init_done) {
        extr_init_done  = 1;
        extr_tar_off    = 0;
        extr_done       = 0;
        extr_remain     = 0;
        extr_pct        = 0;
        extr_complete   = 0;
        extr_log_start  = 0;
        extr_log_count  = 0;
        extr_total      = tar_total_payload_bytes();

        /* Fallback con ticks si no hay initrd cargado. */
        if (extr_total == 0) {
            install_start_tick  = ticks;
            install_timer_armed = 1;
        }
    }

    /* ── Avanzar la extracción un frame ─────────────────────────────── */
    if (extr_total > 0) {
        extr_advance_frame();

        if (extr_complete) {
            current_step    = FINISHED;
            extr_init_done  = 0;
            install_timer_armed = 0;
            ui_mark_dirty();
            return;
        }
    } else {
        /* Fallback: progreso basado en ticks (sin initrd). */
        uint64_t elapsed = ticks - install_start_tick;
        extr_pct = (int)(elapsed * 100 / INSTALL_DURATION_TICKS);
        if (extr_pct > 100) extr_pct = 100;
        if (extr_pct >= 100) {
            current_step        = FINISHED;
            install_timer_armed = 0;
            ui_mark_dirty();
            return;
        }
    }

    /* ── Slideshow (3 diapositivas, ~4 s cada una) ───────────────────── */
    slide = (int)((ticks >> 7) % 3);
    if (slide == 0) {
        st0 = "Rapido y seguro";
        st1 = "NexusOS esta escrito en C para un rendimiento predecible.";
    } else if (slide == 1) {
        st0 = "Listo para el escritorio";
        st1 = "Instalador moderno, firma visual macOS / GTK4.";
    } else {
        st0 = "Open Source";
        st1 = "Construido con transparencia — explore el codigo en GitHub.";
    }
    gfx_aa_text(cx + 20, cy + 16, st0, RGB(28, 32, 44), 2);
    gfx_aa_text(cx + 20, cy + 16 + gfx_aa_line_h(2) + 4, st1,
                RGB(80, 85, 100), 1);

    if (extr_pct < 35)
        sub = "Copiando archivos del sistema...";
    else if (extr_pct < 70)
        sub = "Configurando hardware...";
    else
        sub = "Finalizando instalacion...";
    gfx_aa_text(cx + 20, cy + 16 + gfx_aa_line_h(2) + 4 + gfx_aa_line_h(1) + 6,
                sub, RGB(0, 110, 200), 1);

    /* ── PROGRESS_BAR ───────────────────────────────────────────────── */
    bar_x = cx + 32;
    bar_y = cy + ch / 2 - 20;
    bar_w = cw - 64;
    bar_h = 26;
    if (bar_w < 80) bar_w = 80;
    ui_push_progress_bar(60, bar_x, bar_y, bar_w, bar_h, extr_pct);

    /* ── Log de consola con archivos reales del TAR ─────────────────── */
    log_y = bar_y + bar_h + 36;
    gfx_fill_rect(cx + 20, log_y, cw - 40, ch - (log_y - cy) - 16,
                  RGB(24, 26, 32));
    gfx_rounded_rect_stroke_aa(cx + 20, log_y, cw - 40, ch - (log_y - cy) - 16,
                                6, RGB(60, 65, 78));
    {
        /*
         * Colores degradados: entrada más antigua (top) = tenue,
         * más reciente (bottom) = brillante.
         */
        static const unsigned int log_cols[EXTRACT_LOG_N] = {
            RGB(60, 105, 60),  RGB(75, 120, 75),  RGB(90, 135, 90),
            RGB(110, 155, 110), RGB(135, 175, 135), RGB(158, 198, 158),
            RGB(180, 220, 180)
        };
        int step = gfx_aa_line_h(1);
        int ly   = log_y + 10;
        int i;

        if (extr_log_count > 0) {
            for (i = 0; i < extr_log_count; i++) {
                int slot  = (extr_log_start + i) % EXTRACT_LOG_N;
                int ci    = (EXTRACT_LOG_N - extr_log_count) + i;
                unsigned int col = (ci >= 0 && ci < EXTRACT_LOG_N)
                                   ? log_cols[ci] : log_cols[EXTRACT_LOG_N - 1];
                gfx_aa_text(cx + 28, ly, extr_log[slot], col, 1);
                ly += step;
            }
        } else {
            gfx_aa_text(cx + 28, ly, "> iniciando instalacion...",
                        log_cols[EXTRACT_LOG_N - 1], 1);
        }
        (void)ly;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PASO 4 — FINALIZADO
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_finished(int cx, int cy, int cw, int ch) {
    int         mx  = cx + cw / 2;
    int         my  = cy + 70;
    const char* l1  = "Instalacion completada";
    const char* l2  = "Extraiga el medio de instalacion y reinicie";
    const char* l3  = "para comenzar a usar NexusOS.";
    int tw, y;

    gfx_fill_circle(mx, my, 36, SIDEBAR_DONE);
    gfx_circle_outline_aa(mx, my, 36, RGB(36, 120, 72));
    gfx_wu_line(mx - 14, my, mx - 4, my + 14, RGB(255, 255, 255));
    gfx_wu_line(mx - 4, my + 14, mx + 22, my - 18, RGB(255, 255, 255));

    y  = my + 52;
    tw = gfx_aa_text_w(l1, 2);
    gfx_aa_text(mx - tw/2, y, l1, INS_RGB(COLOR_TEXT_PRIMARY), 2);
    y += gfx_aa_line_h(2);
    tw = gfx_aa_text_w(l2, 1);
    gfx_aa_text(cx + (cw - tw)/2, y, l2, SIDEBAR_MUTED, 1);
    y += gfx_aa_line_h(1);
    tw = gfx_aa_text_w(l3, 1);
    gfx_aa_text(cx + (cw - tw)/2, y, l3, SIDEBAR_MUTED, 1);

    {
        int bw0 = 168, bh0 = 44;
        int bx0 = cx + cw - 2 * bw0 - 32;
        int by0 = cy + ch - bh0 - 20;
        int bw1 = 188, bh1 = 44;
        int bx1 = cx + cw - bw1 - 24;
        int fi0 = ui_push_button(5, bx0, by0, bw0, bh0, cb_view_log);
        int fi1 = ui_push_button(4, bx1, by0, bw1, bh1, cb_reboot);
        ui_manager_sync_from_elements();
        ui_manager_update_hover((int)mouse_get_x(), (int)mouse_get_y());
        draw_installer_button(bx0, by0, bw0, bh0, "Ver registro", 0, fi0);
        draw_installer_button(bx1, by0, bw1, bh1, "Reiniciar ahora", 1, fi1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Barra lateral — lista de pasos con indicadores visuales.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_installer_sidebar(int sx, int sy, int sw, int sh) {
    unsigned int accent = INS_RGB(COLOR_ACCENT);

    gfx_fill_rounded_rect_aa(sx, sy, sw, sh, 4, ARGB(255, 244, 245, 248));
    gfx_blend_rect(sx, sy, sw, sh, INS_RGB(COLOR_BORDER), 18);
    gfx_blend_rect(sx, sy, sw, 3, accent, 90);

    {
        int logo_y = sy + 18;
        int dot_r  = 8;
        int dot_cx = sx + dot_r + 14;

        gfx_fill_circle(dot_cx, logo_y + dot_r, dot_r, accent);
        gfx_circle_outline_aa(dot_cx, logo_y + dot_r, dot_r, RGB(130, 175, 235));

        gfx_aa_text(dot_cx + dot_r + 8, logo_y + 1, "NexusOS",
                    INS_RGB(COLOR_TEXT_PRIMARY), 1);
    }

    gfx_blend_rect(sx + 12, sy + 44, sw - 24, 2, INS_RGB(COLOR_BORDER), 35);

    /* ── Lista de pasos ──────────────────────────────────────────────── */
    static const char* step_labels[] = {
        "Bienvenida", "Localizacion", "Usuario", "Disco", "Resumen",
        "Instalacion", "Completado"
    };
    static const InstallerState step_ids[] = {
        WELCOME, LOCALE, USER_ACCOUNT, DISK_SETUP, SUMMARY, INSTALLING, FINISHED
    };
    const int n_steps  = 7;
    const int step_gap = (sh - 60) / n_steps;   /* espacio vertical entre pasos */
    int item_y = sy + 60;

    for (int i = 0; i < n_steps; i++) {
        InstallerState sid = step_ids[i];
        int is_current  = (current_step == sid);
        int is_done     = (int)current_step > (int)sid;

        int dot_cx = sx + 18;
        int dot_cy = item_y + STEP_DOT_R;

        /* ── Fondo resaltado para el paso actual ─────────────────────── */
        if (is_current) {
            gfx_blend_rect(sx + 4, item_y - 4,
                           sw - 8, STEP_DOT_R * 2 + 10,
                           accent, 28);
        }

        /* ── Indicador (círculo) ─────────────────────────────────────── */
        if (is_done) {
            gfx_fill_circle(dot_cx, dot_cy, STEP_DOT_R, SIDEBAR_DONE);
            /* Checkmark simplificado */
            gfx_wu_line(dot_cx - 4, dot_cy,
                        dot_cx - 1, dot_cy + 3, RGB(255, 255, 255));
            gfx_wu_line(dot_cx - 1, dot_cy + 3,
                        dot_cx + 5, dot_cy - 4, RGB(255, 255, 255));
        } else if (is_current) {
            gfx_fill_circle(dot_cx, dot_cy, STEP_DOT_R, accent);
            gfx_circle_outline_aa(dot_cx, dot_cy, STEP_DOT_R, RGB(140, 185, 240));
            /* Punto interior blanco */
            gfx_fill_circle(dot_cx, dot_cy, 3, RGB(255, 255, 255));
        } else {
            /* Pendiente: sólo contorno */
            gfx_circle_outline_aa(dot_cx, dot_cy, STEP_DOT_R, SIDEBAR_MUTED);
            gfx_blend_pixel(dot_cx, dot_cy, INS_RGB(COLOR_BORDER), 140);
        }

        /* ── Conector vertical entre indicadores ─────────────────────── */
        if (i < n_steps - 1) {
            int conn_y0 = dot_cy + STEP_DOT_R + 2;
            int conn_y1 = item_y + step_gap - STEP_DOT_R - 2;
            unsigned int conn_col = is_done ? SIDEBAR_DONE : SIDEBAR_MUTED;
            for (int yy = conn_y0; yy < conn_y1; yy++)
                gfx_blend_pixel(dot_cx, yy, conn_col, is_done ? 180 : 80);
        }

        /* ── Etiqueta de texto ───────────────────────────────────────── */
        {
            unsigned int tcol = is_done    ? SIDEBAR_DONE  :
                                is_current ? accent :
                                             SIDEBAR_MUTED;
            int text_x = dot_cx + STEP_DOT_R + 8;
            int text_y = item_y + STEP_DOT_R - FONT_AA_GLYPH_H / 2;
            gfx_aa_text(text_x, text_y, step_labels[i], tcol, 1);
        }

        item_y += step_gap;
    }

    gfx_blend_rect(sx + sw - 2, sy, 2, sh, INS_RGB(COLOR_BORDER), 40);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Punto de entrada: repintar el contenido del instalador (llamado cada frame).
 * ═══════════════════════════════════════════════════════════════════════════ */
void draw_installer_content(const Window* win) {
    int cx, cy, cw, ch;
    static InstallerState last_step = WELCOME;

    if (!win || !win->is_visible)
        return;
    installer_draw_floating_shell(win);
    installer_client_rect(win, &cx, &cy, &cw, &ch);
    if (cw < 80 || ch < 60)
        return;

    /* ── Resetear lista de widgets cada frame ────────────────────────── */
    ui_element_count = 0;
    if (current_step != last_step) {
        __builtin_memset(ui_elements, 0, sizeof(ui_elements));
        focused_element_index = 0;
        ui_focus_reset_step();
        last_step             = current_step;
    }
    if (current_step != INSTALLING) {
        install_timer_armed = 0;
        extr_init_done      = 0;   /* reiniciar si se re-entra en INSTALLING */
    }

    /* ── Calcular ancho de la barra lateral ──────────────────────────── */
    int sidebar_w = cw / SIDEBAR_W_FRAC;
    if (sidebar_w > SIDEBAR_W_MAX) sidebar_w = SIDEBAR_W_MAX;
    if (sidebar_w < SIDEBAR_W_MIN) sidebar_w = 0;  /* ocultar en pantallas pequeñas */

    /* ── 1. Barra lateral ────────────────────────────────────────────── */
    if (sidebar_w > 0)
        draw_installer_sidebar(cx, cy, sidebar_w, ch);

    /* ── 2. Contenido: sin tarjeta anidada; el panel ya es la superficie única ─ */
    {
        const int gap_side = 14;
        int       inner_x  = cx + sidebar_w + (sidebar_w > 0 ? gap_side : 12);
        int       inner_y  = cy + 10;
        int       inner_w  = cw - sidebar_w - (sidebar_w > 0 ? gap_side + 10 : 20);
        int       inner_h  = ch - 20;

        if (inner_w < 80 || inner_h < 60)
            goto draw_content_fallback;

        if (sidebar_w > 0)
            gfx_blend_rect(cx + sidebar_w + 4, cy + 6, 2, ch - 12,
                           INS_RGB(COLOR_BORDER), 35);

        switch (current_step) {
        case WELCOME:
            draw_welcome(inner_x, inner_y, inner_w, inner_h);
            break;
        case LOCALE:
            draw_locale_step(inner_x, inner_y, inner_w, inner_h);
            break;
        case USER_ACCOUNT:
            draw_user_account_step(inner_x, inner_y, inner_w, inner_h);
            break;
        case DISK_SETUP:
            draw_disk_setup(inner_x, inner_y, inner_w, inner_h);
            break;
        case SUMMARY:
            draw_summary_step(inner_x, inner_y, inner_w, inner_h);
            if (ins_show_warn_modal)
                draw_destructive_modal(inner_x, inner_y, inner_w, inner_h);
            break;
        case INSTALLING:
            draw_installing(inner_x, inner_y, inner_w, inner_h);
            break;
        case FINISHED:
            draw_finished(inner_x, inner_y, inner_w, inner_h);
            break;
        default:
            break;
        }

        goto after_content;
    }

draw_content_fallback:
    /* Pantalla demasiado pequeña: sin tarjeta, layout original. */
    switch (current_step) {
    case WELCOME:       draw_welcome          (cx, cy, cw, ch); break;
    case LOCALE:        draw_locale_step      (cx, cy, cw, ch); break;
    case USER_ACCOUNT:  draw_user_account_step(cx, cy, cw, ch); break;
    case DISK_SETUP:    draw_disk_setup       (cx, cy, cw, ch); break;
    case SUMMARY:       draw_summary_step     (cx, cy, cw, ch);
        if (ins_show_warn_modal)
            draw_destructive_modal(cx, cy, cw, ch);
        break;
    case INSTALLING:    draw_installing       (cx, cy, cw, ch); break;
    case FINISHED:      draw_finished         (cx, cy, cw, ch); break;
    default: break;
    }

after_content:
    /*
     * Dibujar todos los widgets no-BUTTON (TEXT_INPUT, CHECKBOX, PROGRESS_BAR).
     * Se hace DESPUÉS del switch para que aparezcan encima del fondo del paso.
     */
    ui_manager_sync_from_elements();
    ui_draw_all_elements();
    ui_focus_chain_rebuild();
    ui_manager_sync_focus_flags();
    ui_manager_draw_focus_rings();
}
