/*
 * Instalador gráfico de arranque: inicialización del escritorio y asistente (installer_ui).
 */
#include "gui_installer.h"
#include "gui.h"
#include "gfx.h"
#include "installer_ui.h"
#include "memory.h"
#include "mouse.h"
#include "nexus.h"
#include "window.h"
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
    uint64_t        last_frame = 0;
    unsigned char   prev_btns  = 0;

    for (;;) {
        if (ticks - last_frame < 2) {
            __asm__ volatile("hlt");
            continue;
        }
        last_frame = ticks;

        gfx_layout_refresh();
        installer_paint_frame();

        {
            unsigned char btns = mouse_buttons;
            int           click = (btns & 1) && !(prev_btns & 1);
            prev_btns           = btns;
            if (click && ui_element_count > 0) {
                int hit = ui_get_element_at((int)mouse_x, (int)mouse_y);
                if (hit >= 0) {
                    focused_element_index = hit;
                    ui_activate_focused();
                }
            }
        }

        gfx_draw_cursor(mouse_x, mouse_y);
        swap_buffers();
    }
}
