/*
 * Asistente visual de instalación (flujo tipo Calamares, sin backend real).
 *
 * Pasos:
 *   WELCOME    → pantalla de bienvenida + selector de idioma.
 *   TIMEZONE   → ahora "Configuración del sistema": TEXT_INPUT hostname + usuario.
 *   DISK_SETUP → destino + CHECKBOXes de opciones de disco.
 *   INSTALLING → PROGRESS_BAR animado + log de consola.
 *   FINISHED   → círculo check + botón Reiniciar.
 *
 * DISEÑO VISUAL:
 *   La ventana se divide en dos zonas:
 *     • Barra lateral izquierda (SIDEBAR_W px): panel de navegación con
 *       80 % de opacidad sobre el fondo blanco de la ventana, lista de pasos
 *       y logotipo del SO.
 *     • Área de contenido (resto): tarjeta blanca con bordes redondeados (AA),
 *       sombra difusa y degradado de encabezado.
 */
#include "installer_ui.h"
#include "gui.h"
#include "gfx.h"
#include "mouse.h"
#include "nexus.h"
#include "font_aa.h"
#include "vfs.h"
#include <stddef.h>

/* Ancho de la barra lateral en píxeles lógicos. */
#define SIDEBAR_W_MIN  100
#define SIDEBAR_W_MAX  200
#define SIDEBAR_W_FRAC 4   /* 1/4 del ancho del contenido */

/* Colores de la barra lateral (dark navy, ~80 % opacidad). */
#define SIDEBAR_BG_RGB   RGB(14, 21, 45)
#define SIDEBAR_BG_ALPHA 204u          /* 204/255 ≈ 80 % */
#define SIDEBAR_ACCENT   RGB(0, 120, 220)
#define SIDEBAR_TEXT     RGB(195, 205, 230)
#define SIDEBAR_DONE     RGB(50, 180, 80)
#define SIDEBAR_DIM      RGB(80, 100, 145)

/* Radio del círculo de indicador de paso. */
#define STEP_DOT_R 7

/* Colores de la tarjeta de contenido. */
#define CARD_RADIUS  14
#define CARD_SHADOW  10   /* spread de sombra */

InstallerState current_step = WELCOME;

/* ── Callbacks de navegación ─────────────────────────────────────────────── */
static void cb_welcome_next(void) { current_step = TIMEZONE; }
static void cb_tz_next     (void) { current_step = DISK_SETUP; }
static void cb_disk_install(void) { current_step = INSTALLING; }
static void cb_reboot(void) {
    outb(0x64u, 0xFEu);
    for (;;) __asm__ volatile("hlt");
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
    *cy = w->y + INSTALLER_WIN_TITLE_H + 2;
    *cw = w->width  - 2 * INSTALLER_WIN_INSET;
    *ch = w->height - INSTALLER_WIN_TITLE_H - 6;
}

/* ── draw_labeled_button: dibuja un botón con focus ring opcional ─────────── */
static void draw_labeled_button(int x, int y, int bw, int bh,
                                const char* label,
                                unsigned int bg, unsigned int stroke_rgb,
                                int focused) {
    int tw = gfx_aa_text_w(label, 1);
    int tx = x + (bw - tw) / 2;
    int ty = y + (bh - FONT_AA_GLYPH_H) / 2;
    if (focused) {
        gfx_rounded_rect_stroke_aa(x - 4, y - 4, bw + 8, bh + 8, 12,
                                   RGB(0, 255, 240));
        gfx_rounded_rect_stroke_aa(x - 2, y - 2, bw + 4, bh + 4, 10,
                                   RGB(160, 255, 248));
    }
    gfx_fill_rounded_rect(x, y, bw, bh, 8, bg);
    gfx_rounded_rect_stroke_aa(x, y, bw, bh, 8, stroke_rgb);
    gfx_aa_text(tx, ty, label, RGB(255, 255, 255), 1);
}

static void draw_lang_option(int x, int y, int w, int h,
                             const char* name, int selected) {
    unsigned int bg     = selected ? RGB(220, 235, 255) : RGB(240, 242, 248);
    unsigned int fg     = selected ? RGB(20, 50, 120)   : RGB(60, 62, 72);
    unsigned int border = selected ? RGB(0, 100, 220)   : RGB(190, 195, 210);
    gfx_fill_rounded_rect(x, y, w, h, 6, bg);
    gfx_rounded_rect_stroke_aa(x, y, w, h, 6, border);
    gfx_aa_text(x + 14, y + (h - FONT_AA_GLYPH_H) / 2, name, fg, 1);
    if (selected)
        gfx_aa_text(x + w - 36, y + (h - FONT_AA_GLYPH_H) / 2,
                    "[*]", RGB(0, 120, 220), 1);
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

    gfx_aa_text(tx, ty, headline, RGB(28, 32, 44), title_scale);
    gfx_aa_text(cx + 24, ty + gfx_aa_line_h(title_scale) + 8,
                "Asistente de instalacion — Live CD",
                RGB(90, 95, 110), 1);

    {
        int ly  = ty + gfx_aa_line_h(title_scale) + 44;
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

    gfx_aa_text(cx + 20, cy + 16, "Configuracion del sistema",
                RGB(28, 32, 44), 2);
    gfx_aa_text(cx + 20, cy + 16 + gfx_aa_line_h(2) + 4,
                "Personaliza el nombre de tu equipo y cuenta de usuario.",
                RGB(80, 85, 100), 1);

    /* ── TEXT_INPUT: Nombre del equipo ───────────────────────────────── */
    label_y  = cy + 98;
    field_y  = label_y + FONT_AA_GLYPH_H + 6;

    gfx_aa_text(field_x, label_y, "Nombre del equipo (hostname):",
                RGB(60, 65, 82), 1);
    fi = ui_push_text_input(10, field_x, field_y, field_w, field_h, "nexusos");
    (void)fi;

    /* ── TEXT_INPUT: Nombre de usuario ───────────────────────────────── */
    label_y  = field_y + field_h + 22;
    field_y  = label_y + FONT_AA_GLYPH_H + 6;

    gfx_aa_text(field_x, label_y, "Nombre de usuario:",
                RGB(60, 65, 82), 1);
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

    gfx_aa_text(cx + 20, disk_y, "Destino de la instalacion",
                RGB(28, 32, 44), 2);
    disk_y += gfx_aa_line_h(2) + 6;

    /* Tarjeta del disco */
    gfx_fill_rounded_rect(cx + 20, disk_y, cw - 40, disk_h, 10, RGB(228, 232, 240));
    gfx_rounded_rect_stroke_aa(cx + 20, disk_y, cw - 40, disk_h, 10, RGB(140, 150, 175));
    gfx_fill_rounded_rect(cx + 36, disk_y + 16, 40, 40, 6, RGB(100, 110, 130));
    gfx_fill_rect(cx + 44, disk_y + 24, 24, 8, RGB(200, 205, 220));
    gfx_aa_text(cx + 88, disk_y + 18, "/dev/sda",               RGB(40, 44, 55), 1);
    gfx_aa_text(cx + 88, disk_y + 40, "50 GB libres · ATA Disk", RGB(75, 80, 95), 1);

    /* ── CHECKBOX 1: Formatear disco ──────────────────────────────────── */
    {
        int oy    = disk_y + disk_h + 22;
        int lbl_w = 24 + 12 + gfx_aa_text_w("Formatear disco completo (/dev/sda)", 1);
        int fi    = ui_push_checkbox(20, cx + 20, oy, lbl_w,
                                     "Formatear disco completo (/dev/sda)", NULL);
        (void)fi;
    }

    /* ── CHECKBOX 2: Instalar gestor de arranque ──────────────────────── */
    {
        int oy    = disk_y + disk_h + 22 + 26 + 14;
        int lbl_w = 24 + 12 + gfx_aa_text_w("Instalar gestor de arranque GRUB", 1);
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
    int bar_x, bar_y, bar_w, bar_h, log_y;

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
            return;
        }
    }

    /* ── Renderizar ─────────────────────────────────────────────────── */
    gfx_aa_text(cx + 20, cy + 16, "Instalando NexusOS",
                RGB(28, 32, 44), 2);
    gfx_aa_text(cx + 20, cy + 16 + gfx_aa_line_h(2) + 4,
                "No apague el equipo durante la instalacion.",
                RGB(100, 55, 55), 1);

    /* ── PROGRESS_BAR ───────────────────────────────────────────────── */
    bar_x = cx + 32;
    bar_y = cy + ch / 2 - 50;
    bar_w = cw - 64;
    bar_h = 26;
    if (bar_w < 80) bar_w = 80;
    ui_push_progress_bar(30, bar_x, bar_y, bar_w, bar_h, extr_pct);

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
    const char* l1  = "Instalacion completada.";
    const char* l2  = "Por favor, extraiga el medio de instalacion";
    const char* l3  = "y reinicie el equipo.";
    int tw, y;

    gfx_fill_circle(mx, my, 36, RGB(46, 180, 80));
    gfx_circle_outline_aa(mx, my, 36, RGB(20, 100, 40));
    gfx_wu_line(mx - 14, my, mx - 4, my + 14, RGB(255, 255, 255));
    gfx_wu_line(mx - 4, my + 14, mx + 22, my - 18, RGB(255, 255, 255));

    y  = my + 52;
    tw = gfx_aa_text_w(l1, 2);
    gfx_aa_text(mx - tw/2, y, l1, RGB(28, 32, 44), 2);
    y += gfx_aa_line_h(2);
    tw = gfx_aa_text_w(l2, 1);
    gfx_aa_text(cx + (cw - tw)/2, y, l2, RGB(55, 60, 72), 1);
    y += gfx_aa_line_h(1);
    tw = gfx_aa_text_w(l3, 1);
    gfx_aa_text(cx + (cw - tw)/2, y, l3, RGB(55, 60, 72), 1);

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
 *  Barra lateral — lista de pasos con indicadores visuales.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_installer_sidebar(int sx, int sy, int sw, int sh) {
    /* ── Fondo: 80 % de opacidad de navy sobre el blanco de la ventana ── */
    gfx_blend_rect(sx, sy, sw, sh, SIDEBAR_BG_RGB, SIDEBAR_BG_ALPHA);

    /* ── Franja acentuada en el borde superior ─────────────────────── */
    gfx_fill_rect(sx, sy, sw, 3, SIDEBAR_ACCENT);

    /* ── Logotipo / nombre del SO ────────────────────────────────────── */
    {
        int logo_y = sy + 18;
        int dot_r  = 8;
        int dot_cx = sx + dot_r + 14;

        /* Círculo coloreado (logo simplificado) */
        gfx_fill_circle(dot_cx, logo_y + dot_r, dot_r, SIDEBAR_ACCENT);
        gfx_circle_outline_aa(dot_cx, logo_y + dot_r, dot_r, RGB(80, 160, 255));

        /* Nombre */
        gfx_aa_text(dot_cx + dot_r + 8, logo_y + 1, "NexusOS",
                    RGB(235, 240, 255), 1);
    }

    /* ── Línea divisoria tras el logo ────────────────────────────────── */
    gfx_blend_rect(sx + 12, sy + 44, sw - 24, 1, RGB(80, 110, 175), 120);

    /* ── Lista de pasos ──────────────────────────────────────────────── */
    static const char* step_labels[] = {
        "Bienvenida", "Sistema", "Disco", "Instalando", "Completado"
    };
    static const InstallerState step_ids[] = {
        WELCOME, TIMEZONE, DISK_SETUP, INSTALLING, FINISHED
    };
    const int n_steps  = 5;
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
                           RGB(0, 80, 180), 70);
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
            gfx_fill_circle(dot_cx, dot_cy, STEP_DOT_R, SIDEBAR_ACCENT);
            gfx_circle_outline_aa(dot_cx, dot_cy, STEP_DOT_R, RGB(100, 180, 255));
            /* Punto interior blanco */
            gfx_fill_circle(dot_cx, dot_cy, 3, RGB(255, 255, 255));
        } else {
            /* Pendiente: sólo contorno */
            gfx_circle_outline_aa(dot_cx, dot_cy, STEP_DOT_R, SIDEBAR_DIM);
            gfx_blend_pixel(dot_cx, dot_cy, RGB(60, 80, 120), 120);
        }

        /* ── Conector vertical entre indicadores ─────────────────────── */
        if (i < n_steps - 1) {
            int conn_y0 = dot_cy + STEP_DOT_R + 2;
            int conn_y1 = item_y + step_gap - STEP_DOT_R - 2;
            unsigned int conn_col = is_done ? SIDEBAR_DONE : SIDEBAR_DIM;
            for (int yy = conn_y0; yy < conn_y1; yy++)
                gfx_blend_pixel(dot_cx, yy, conn_col, is_done ? 180 : 80);
        }

        /* ── Etiqueta de texto ───────────────────────────────────────── */
        {
            unsigned int tcol = is_done    ? SIDEBAR_DONE  :
                                is_current ? RGB(220, 235, 255) :
                                             SIDEBAR_DIM;
            int text_x = dot_cx + STEP_DOT_R + 8;
            int text_y = item_y + STEP_DOT_R - FONT_AA_GLYPH_H / 2;
            gfx_aa_text(text_x, text_y, step_labels[i], tcol, 1);
        }

        item_y += step_gap;
    }

    /* ── Borde derecho de separación ─────────────────────────────────── */
    for (int yy = sy; yy < sy + sh; yy++)
        gfx_blend_pixel(sx + sw - 1, yy, RGB(60, 90, 160), 100);
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

    /* ── Resetear lista de widgets cada frame ────────────────────────── */
    ui_element_count = 0;
    if (current_step != last_step) {
        focused_element_index = 0;
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

    /* ── 2. Tarjeta de contenido principal ───────────────────────────── */
    {
        int card_x  = cx + sidebar_w + 6;
        int card_y  = cy + 6;
        int card_w  = cw - sidebar_w - 12;
        int card_h  = ch - 12;

        if (card_w < 80 || card_h < 60)
            goto draw_content_fallback;

        /* Sombra difusa detrás de la tarjeta. */
        gfx_drop_shadow_soft(card_x, card_y, card_w, card_h, CARD_RADIUS, CARD_SHADOW);

        /* Superficie de la tarjeta: blanco puro con esquinas AA. */
        gfx_fill_rounded_rect_aa(card_x, card_y, card_w, card_h,
                                  CARD_RADIUS, RGB(252, 253, 255));

        /* Franja de encabezado superior con degradado sutil. */
        {
            int hdr_h = 40;
            for (int yy = 0; yy < hdr_h; yy++) {
                /* De blanco casi puro a blanco con tinte frío leve. */
                uint32_t col = gfx_lerp_rgb(RGB(245, 247, 255),
                                             RGB(252, 253, 255), yy, hdr_h);
                gfx_hline(card_x + CARD_RADIUS, card_y + yy,
                          card_w - 2 * CARD_RADIUS, col);
            }
            /* Reflejar el degradado en las esquinas superiores redondeadas. */
            for (int yy = 0; yy < CARD_RADIUS && yy < hdr_h; yy++) {
                uint32_t col = gfx_lerp_rgb(RGB(245, 247, 255),
                                             RGB(252, 253, 255), yy, hdr_h);
                for (int xx = 0; xx < CARD_RADIUS; xx++) {
                    int cdx = CARD_RADIUS - 1 - xx;
                    int cdy = CARD_RADIUS - 1 - yy;
                    if (cdx * cdx + cdy * cdy <= CARD_RADIUS * CARD_RADIUS)
                        gfx_put_pixel(card_x + xx,            card_y + yy, col);
                    if (cdx * cdx + cdy * cdy <= CARD_RADIUS * CARD_RADIUS)
                        gfx_put_pixel(card_x + card_w - 1 - xx, card_y + yy, col);
                }
            }
        }

        /* Borde sutil alrededor de la tarjeta (1 px, gris muy claro). */
        gfx_rounded_rect_stroke_aa(card_x, card_y, card_w, card_h,
                                    CARD_RADIUS, RGB(215, 218, 230));

        /* Área interior utilizable por el contenido del paso. */
        int inner_x = card_x + 4;
        int inner_y = card_y + 4;
        int inner_w = card_w - 8;
        int inner_h = card_h - 8;

        /* ── 3. Contenido del paso actual ────────────────────────────── */
        switch (current_step) {
        case WELCOME:    draw_welcome       (inner_x, inner_y, inner_w, inner_h); break;
        case TIMEZONE:   draw_timezone_step (inner_x, inner_y, inner_w, inner_h); break;
        case DISK_SETUP: draw_disk_setup    (inner_x, inner_y, inner_w, inner_h); break;
        case INSTALLING: draw_installing    (inner_x, inner_y, inner_w, inner_h); break;
        case FINISHED:   draw_finished      (inner_x, inner_y, inner_w, inner_h); break;
        default: break;
        }

        goto after_content;
    }

draw_content_fallback:
    /* Pantalla demasiado pequeña: sin tarjeta, layout original. */
    switch (current_step) {
    case WELCOME:    draw_welcome       (cx, cy, cw, ch); break;
    case TIMEZONE:   draw_timezone_step (cx, cy, cw, ch); break;
    case DISK_SETUP: draw_disk_setup    (cx, cy, cw, ch); break;
    case INSTALLING: draw_installing    (cx, cy, cw, ch); break;
    case FINISHED:   draw_finished      (cx, cy, cw, ch); break;
    default: break;
    }

after_content:
    /*
     * Dibujar todos los widgets no-BUTTON (TEXT_INPUT, CHECKBOX, PROGRESS_BAR).
     * Se hace DESPUÉS del switch para que aparezcan encima del fondo del paso.
     */
    ui_draw_all_elements();
}
