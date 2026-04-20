/*
 * Instalador gráfico de arranque: inicialización del escritorio y asistente (installer_ui).
 *
 * ARQUITECTURA DE EVENTOS:
 *   start_gui_installer() ya no sondea mouse_buttons ni tecla_nueva directamente.
 *   En cada frame consume todos los Event* del ring buffer y despacha la
 *   lógica interactiva exclusivamente dentro del switch(ev.type).
 */
#include "gui_installer.h"
#include "gui.h"
#include "gfx.h"
#include "installer_ui.h"
#include "memory.h"
#include "mouse.h"
#include "nexus.h"
#include "window.h"
#include "event.h"
#include <stdint.h>

extern volatile uint64_t ticks;

#define INSTALLER_BG RGB(28, 34, 48)

static void installer_paint_frame(void) {
    int sw = gfx_width();
    int sh = gfx_height();
    int ww = sw * 52 / 100;
    int wh = sh * 45 / 100;
    int wx, wy;
    Window win;

    if (ww < 400)
        ww = 400;
    if (ww > sw - 32)
        ww = sw - 32;
    if (wh < 200)
        wh = 200;
    if (wh > sh - 32)
        wh = sh - 32;

    wx = (sw - ww) / 2;
    wy = (sh - wh) / 2;
    if (wx < 8)
        wx = 8;
    if (wy < 8)
        wy = 8;

    win.x         = wx;
    win.y         = wy;
    win.width     = ww;
    win.height    = wh;
    win.title     = "Instalador de NexusOS";
    win.is_visible = 1;
    win.is_active  = 1;
    win.z_index    = 100;

    gfx_fill_screen_solid(INSTALLER_BG);
    draw_window(&win);
    draw_installer_content(&win);
}

void init_desktop(void) {
    VesaBootInfo vbi;
    int          has_vesa = gfx_vesa_detect(&vbi);

    if (has_vesa) {
        if (vbi.bpp == 32u && vbi.pitch > 0u && vbi.height > 0u) {
            if (gui_framebuffer_init_kmalloc(&vbi) != 0)
                kheap_panic_nomem("init_desktop: double buffer");
        } else {
            gfx_init_vesa(vbi.lfb_ptr, vbi.width, vbi.height, vbi.pitch, vbi.bpp);
        }
        if (vesa_console_active)
            vesa_force_refresh();
        mouse_init((int)vbi.width, (int)vbi.height);
    } else {
        gfx_init_vga();
        mouse_init(320, 200);
    }

    gfx_layout_refresh();
    vesa_console_active = 0;
}

void start_gui_installer(void) {
    uint64_t last_frame = 0;

    /* Descartar eventos acumulados durante el arranque. */
    flush_events();

    for (;;) {
        /* ── Limitar a ~30 fps (ticks a 1000 Hz → esperar ≥2 ticks) ─── */
        if (ticks - last_frame < 2) {
            __asm__ volatile("hlt");
            continue;
        }
        last_frame = ticks;

        /* ════════════════════════════════════════════════════════════════
         * BUCLE DE MENSAJES — procesar TODOS los eventos pendientes antes
         * de redibujar el frame.
         * ════════════════════════════════════════════════════════════════ */
        {
            Event ev;
            while (pop_event(&ev)) {
                switch (ev.type) {

                /* ── Movimiento de ratón: actualizar foco hover ──────── */
                case EVENT_MOUSE_MOVE:
                    if (ui_element_count > 0)
                        ui_update_focus_from_mouse(ev.mouse_x, ev.mouse_y);
                    break;

                /* ── Clic de ratón: activar elemento bajo el cursor ──── */
                case EVENT_MOUSE_CLICK:
                    if (ev.mouse_pressed && (ev.mouse_buttons & 1) &&
                        ui_element_count > 0) {
                        int hit = ui_get_element_at(ev.mouse_x, ev.mouse_y);
                        if (hit >= 0) {
                            focused_element_index = hit;
                            ui_activate_focused();
                        }
                    }
                    break;

                /* ── Tecla pulsada ───────────────────────────────────── */
                case EVENT_KEY_PRESS:
                    if (ui_element_count > 0 &&
                        focused_element_index >= 0 &&
                        focused_element_index < ui_element_count &&
                        ui_elements[focused_element_index].type == UI_TYPE_TEXT_INPUT) {

                        /* Dentro de un TEXT_INPUT */
                        if (ev.scancode == 0x0Eu) {
                            /* Backspace */
                            ui_handle_char('\b');
                        } else if (ev.scancode == 0x0Fu || ev.scancode == 0x1Cu) {
                            /* TAB / Enter: avanzar foco */
                            ui_focus_advance();
                        } else if (ev.ascii >= 32) {
                            /* Carácter imprimible */
                            ui_handle_char((unsigned char)ev.ascii);
                        }

                    } else if (ui_element_count > 0) {

                        /* Navegación general entre widgets */
                        if (ev.scancode == 0x0Fu) {
                            /* TAB: siguiente elemento */
                            ui_focus_advance();
                        } else if (ev.scancode == 0x1Cu) {
                            /* Enter: activar elemento enfocado */
                            ui_activate_focused();
                        }
                    }
                    break;

                case EVENT_WINDOW_CLOSE:
                    /* El instalador no gestiona cierres de ventana. */
                    break;

                default:
                    break;
                }
            }
        }

        /* ── Renderizar frame ────────────────────────────────────────── */
        gfx_layout_refresh();
        installer_paint_frame();
        gfx_draw_cursor(mouse_x, mouse_y);
        swap_buffers();
    }
}
