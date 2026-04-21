/*
 * Instalador gráfico de arranque: inicialización del escritorio y asistente (installer_ui).
 *
 * Pipeline visual: fondo/ventana/botones vía BMP (installer_ui); repintado solo si
 * ui_needs_update o animación del paso INSTALLING.
 */
#include "gui_installer.h"
#include "gui.h"
#include "gfx.h"
#include "installer_ui.h"
#include "memory.h"
#include "mouse.h"
#include "xhci.h"
#include "nexus.h"
#include "window.h"
#include "event.h"
#include "ui_manager.h"
#include <stdint.h>

extern volatile uint64_t ticks;

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

    win.x          = wx;
    win.y          = wy;
    win.width      = ww;
    win.height     = wh;
    win.title      = "Instalador de NexusOS";
    win.is_visible = 1;
    win.is_active  = 1;
    win.z_index    = 100;

    installer_paint_background_fullscreen();
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
        (void)mouse_init((int32_t)vbi.width, (int32_t)vbi.height);
        xhci_set_screen_dims((int)vbi.width, (int)vbi.height);
    } else {
        gfx_init_vga();
        (void)mouse_init(320, 200);
        xhci_set_screen_dims(320, 200);
    }

    gfx_layout_refresh();
    vesa_console_active = 0;
}

static void installer_process_events(void) {
    Event ev;
    int   any = 0;

    while (pop_event(&ev)) {
        any = 1;
        switch (ev.type) {
        case EVENT_MOUSE_MOVE:
            if (ui_element_count > 0)
                ui_manager_update_hover(ev.mouse_x, ev.mouse_y);
            break;
        case EVENT_MOUSE_CLICK:
            if (ev.mouse_pressed && (ev.mouse_buttons & 1) && ui_element_count > 0)
                (void)ui_manager_handle_primary_click(ev.mouse_x, ev.mouse_y);
            break;
        case EVENT_KEY_PRESS:
            if (ui_element_count <= 0)
                break;

            if (ev.key_extended) {
                if (ev.scancode == 0x50u || ev.scancode == 0x4Du)
                    ui_focus_tab_next();
                else if (ev.scancode == 0x48u || ev.scancode == 0x4Bu)
                    ui_focus_tab_prev();
                ui_manager_sync_focus_flags();
                break;
            }

            if (ev.scancode == 0x0Fu) {
                ui_focus_tab_next();
                ui_manager_sync_focus_flags();
                break;
            }

            if (ev.scancode == 0x1Cu) {
                ui_activate_focused();
                break;
            }

            if (focused_element_index >= 0 && focused_element_index < ui_element_count) {
                UI_Element* fel = &ui_elements[focused_element_index];
                if (fel->on_keypress) {
                    if (ev.ascii)
                        fel->on_keypress(ev.ascii);
                    break;
                }
                if (fel->type == UI_TYPE_TEXT_INPUT) {
                    if (ev.scancode == 0x0Eu)
                        ui_handle_char('\b');
                    else if (ev.ascii >= 32)
                        ui_handle_char((unsigned char)ev.ascii);
                }
            }
            break;
        case EVENT_WINDOW_CLOSE:
            break;
        default:
            break;
        }
    }
    if (any)
        ui_mark_dirty();
}

void installer_desktop_step(void) {
    /* API syscall / userland: un frame completo siempre. */
    installer_process_events();
    installer_paint_frame();
    ui_needs_update = false;
    gfx_layout_refresh();
    gfx_draw_cursor((int)mouse_get_x(), (int)mouse_get_y());
    gfx_mark_present_full();
    swap_buffers();
}

void start_gui_installer(void) {
    uint64_t last_anim_tick = 0;

    flush_events();
    ui_mark_dirty();

    for (;;) {
        installer_process_events();

        {
            int need = ui_needs_update ? 1 : 0;
            if (current_step == INSTALLING) {
                if (ticks - last_anim_tick >= 2) {
                    need            = 1;
                    last_anim_tick  = ticks;
                }
            }
            if (!need) {
                __asm__ volatile("hlt");
                continue;
            }
        }

        ui_needs_update = false;
        installer_paint_frame();

        gfx_layout_refresh();
        gfx_draw_cursor((int)mouse_get_x(), (int)mouse_get_y());
        gfx_mark_present_full();
        swap_buffers();
    }
}
