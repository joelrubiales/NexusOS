/*
 * Consola rescue (CLI): prompt, lectura con keyboard_getchar(), comandos básicos.
 */
#include "shell.h"
#include "nexus.h"
#include "gfx.h"
#include "gui.h"
#include "keyboard.h"
#include "boot_info.h"
#include <stddef.h>
#include <stdint.h>

extern void vga_set_text_80x25(void);

#define SHBUF_MAX 256

#define COL_SH_BG  RGB(0, 0, 0)
#define COL_SH_GRN RGB(0, 220, 90)
#define COL_SH_WHT RGB(210, 215, 220)

static int  shell_gfx;
static int  shell_cols;
static int  shell_rows;
static int  pen_r, pen_c;
static int  in_or_r, in_or_c;

/* Glifo 8×8 sobre framebuffer (misma fuente que gfx). */
static void draw_char(int px, int py, char ch, uint32_t fg, uint32_t bg) {
    gfx_draw_char(px, py, (unsigned char)ch, fg, bg);
}

static void shell_scroll_gfx(void) {
    BootInfo*          bi = (BootInfo*)(uintptr_t)BOOT_INFO_ADDR;
    volatile uint32_t* fb = (volatile uint32_t*)(uintptr_t)bi->lfb_ptr;
    int                pitch_dw = (int)bi->pitch / 4;
    int                W = gfx_width();
    int                H = gfx_height();
    int                y, x;

    for (y = 8; y < H; y++) {
        volatile uint32_t* dst = fb + (size_t)(y - 8) * (size_t)pitch_dw;
        volatile uint32_t* src = fb + (size_t)y * (size_t)pitch_dw;
        for (x = 0; x < W; x++)
            dst[x] = src[x];
    }
    for (y = H - 8; y < H; y++) {
        volatile uint32_t* dst = fb + (size_t)y * (size_t)pitch_dw;
        for (x = 0; x < W; x++)
            dst[x] = (uint32_t)COL_SH_BG;
    }
}

static void shell_scroll_vga(void) {
    volatile uint16_t* m = (volatile uint16_t*)0xB8000u;
    int                r, c;

    for (r = 1; r < 25; r++) {
        for (c = 0; c < 80; c++)
            m[(r - 1) * 80 + c] = m[r * 80 + c];
    }
    for (c = 0; c < 80; c++)
        m[24 * 80 + c] = 0x0720u;
}

static void shell_advance_pen(void) {
    pen_c++;
    if (pen_c >= shell_cols) {
        pen_c = 0;
        pen_r++;
        if (pen_r >= shell_rows) {
            if (shell_gfx)
                shell_scroll_gfx();
            else
                shell_scroll_vga();
            pen_r = shell_rows - 1;
        }
    }
}

static void shell_retreat_pen(void) {
    if (pen_c > 0)
        pen_c--;
    else if (pen_r > 0) {
        pen_r--;
        pen_c = shell_cols - 1;
    }
}

static int pen_at_input_origin(void) {
    return pen_r == in_or_r && pen_c == in_or_c;
}

static void shell_erase_at_pen(void) {
    if (shell_gfx)
        gfx_fill_rect(pen_c * 8, pen_r * 8, 8, 8, COL_SH_BG);
    else {
        volatile uint16_t* m = (volatile uint16_t*)0xB8000u;
        m[pen_r * 80 + pen_c] = 0x0720u;
    }
}

static void shell_putc_gfx(char c, uint32_t fg) {
    if (c == '\n') {
        pen_c = 0;
        pen_r++;
        if (pen_r >= shell_rows) {
            shell_scroll_gfx();
            pen_r = shell_rows - 1;
        }
        return;
    }
    draw_char(pen_c * 8, pen_r * 8, c, fg, COL_SH_BG);
    shell_advance_pen();
}

static void shell_putc_vga(char c, uint8_t attr) {
    volatile uint16_t* m = (volatile uint16_t*)0xB8000u;

    if (c == '\n') {
        pen_c = 0;
        pen_r++;
        if (pen_r >= shell_rows) {
            shell_scroll_vga();
            pen_r = shell_rows - 1;
        }
        return;
    }
    m[pen_r * 80 + pen_c] = (uint16_t)(unsigned char)c | ((uint16_t)attr << 8);
    shell_advance_pen();
}

static void shell_puts(const char* s, uint32_t gfx_fg, uint8_t vga_attr) {
    for (; *s; s++) {
        if (shell_gfx)
            shell_putc_gfx(*s, gfx_fg);
        else
            shell_putc_vga(*s, vga_attr);
    }
    vesa_force_refresh();
}

static void shell_clear_screen(void) {
    int i;

    if (shell_gfx) {
        gfx_fill_screen_solid(COL_SH_BG);
    } else {
        volatile uint16_t* m = (volatile uint16_t*)0xB8000u;
        for (i = 0; i < 80 * 25; i++)
            m[i] = 0x0720u;
    }
    pen_r = 0;
    pen_c = 0;
    vesa_force_refresh();
}

static void shell_show_prompt(void) {
    shell_puts("nexus-rescue", COL_SH_GRN, 0x0Au);
    shell_puts("> ", COL_SH_WHT, 0x0Fu);
    in_or_r = pen_r;
    in_or_c = pen_c;
}

static void shell_do_reboot(void) {
    __asm__ volatile("cli");
    outb(0x64u, 0xFEu);
    for (volatile int i = 0; i < 500000; i++) {
    }
    struct {
        unsigned short      lim;
        unsigned long long  base;
    } __attribute__((packed)) z = {0, 0};
    __asm__ volatile("lidt %0\n\tint $3" : : "m"(z));
}

static void shell_run_line(char* buf, int len) {
    if (shell_gfx)
        shell_putc_gfx('\n', COL_SH_WHT);
    else
        shell_putc_vga('\n', 0x0Fu);
    vesa_force_refresh();

    if (len <= 0)
        return;

    if (comparar_cadenas(buf, "help"))
        shell_puts("Comandos disponibles: help, clear, reboot\n", COL_SH_WHT, 0x0Fu);
    else if (comparar_cadenas(buf, "clear"))
        shell_clear_screen();
    else if (comparar_cadenas(buf, "reboot")) {
        shell_puts("Reiniciando...\n", COL_SH_GRN, 0x0Au);
        retraso(400);
        shell_do_reboot();
    } else
        shell_puts("Comando no reconocido\n", COL_SH_WHT, 0x0Fu);
}

void start_shell(void) {
    BootInfo* bi = (BootInfo*)(uintptr_t)BOOT_INFO_ADDR;

    ui_focus_clear();

    if (vesa_console_active && bi->magic == BOOT_INFO_MAGIC && bi->lfb_ptr != 0ull &&
        bi->bpp == 32u) {
        gfx_init_vesa(bi->lfb_ptr, (int)bi->width, (int)bi->height, (int)bi->pitch, (int)bi->bpp);
        shell_gfx = 1;
        gfx_fill_screen_solid(COL_SH_BG);
        shell_cols = gfx_width() / 8;
        shell_rows = gfx_height() / 8;
    } else {
        vga_set_text_80x25();
        shell_gfx = 0;
        shell_cols = 80;
        shell_rows = 25;
        {
            volatile uint16_t* m = (volatile uint16_t*)0xB8000u;
            int                i;
            for (i = 0; i < 80 * 25; i++)
                m[i] = 0x0720u;
        }
    }

    pen_r = 0;
    pen_c = 0;
    vesa_force_refresh();

    for (;;) {
        char buf[SHBUF_MAX];
        int  len = 0;
        int  i;

        shell_show_prompt();

        for (;;) {
            char c = keyboard_getchar();

            if (c == '\n')
                break;

            if (c == '\b') {
                if (len == 0 || pen_at_input_origin())
                    continue;
                len--;
                buf[len] = 0;
                shell_retreat_pen();
                shell_erase_at_pen();
                vesa_force_refresh();
                continue;
            }

            if ((unsigned char)c < 32u || c == 127)
                continue;

            if (len >= SHBUF_MAX - 1)
                continue;

            buf[len++] = c;
            buf[len]   = 0;

            if (shell_gfx)
                shell_putc_gfx(c, COL_SH_WHT);
            else
                shell_putc_vga(c, 0x0Fu);
            vesa_force_refresh();
        }

        buf[len] = 0;
        /* Quitar espacios finales/iniciales mínimos para el parseo */
        while (len > 0 && buf[len - 1] == ' ')
            buf[--len] = 0;
        i = 0;
        while (buf[i] == ' ')
            i++;
        if (i > 0) {
            int j = 0;
            while (buf[i])
                buf[j++] = buf[i++];
            buf[j] = 0;
            len    = j;
        }

        shell_run_line(buf, len);
    }
}
