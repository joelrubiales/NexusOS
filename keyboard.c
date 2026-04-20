/*
 * Driver teclado PS/2 — Scan Code Set 1, US QWERTY básico.
 * IRQ1 ya entra en keyboard_body (idt.c); aquí se traduce a ASCII y cola para getchar.
 */
#include "keyboard.h"
#include "gui.h"
#include "nexus.h"

#define KBD_BUF_SIZE 256

static volatile char     kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head;
static volatile uint32_t kbd_tail;
static volatile int kbd_shift;

/* Set 1 make codes; 0 = ignorar o manejado aparte. */
static const char sc_us_normal[128] = {
    [0x01] = 0x1B, /* Esc → convenio getchar (ASCII ESC) */
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n', /* Enter */
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x39] = ' ',
};

static const char sc_us_shift[128] = {
    [0x01] = 0x1B,
    [0x02] = '!',
    [0x03] = '@',
    [0x04] = '#',
    [0x05] = '$',
    [0x06] = '%',
    [0x07] = '^',
    [0x08] = '&',
    [0x09] = '*',
    [0x0A] = '(',
    [0x0B] = ')',
    [0x0C] = '_',
    [0x0D] = '+',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'Q',
    [0x11] = 'W',
    [0x12] = 'E',
    [0x13] = 'R',
    [0x14] = 'T',
    [0x15] = 'Y',
    [0x16] = 'U',
    [0x17] = 'I',
    [0x18] = 'O',
    [0x19] = 'P',
    [0x1A] = '{',
    [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A',
    [0x1F] = 'S',
    [0x20] = 'D',
    [0x21] = 'F',
    [0x22] = 'G',
    [0x23] = 'H',
    [0x24] = 'J',
    [0x25] = 'K',
    [0x26] = 'L',
    [0x27] = ':',
    [0x28] = '"',
    [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z',
    [0x2D] = 'X',
    [0x2E] = 'C',
    [0x2F] = 'V',
    [0x30] = 'B',
    [0x31] = 'N',
    [0x32] = 'M',
    [0x33] = '<',
    [0x34] = '>',
    [0x35] = '?',
    [0x39] = ' ',
};

static int kbd_queue_full(void) {
    return (((uint32_t)kbd_tail + 1u) % KBD_BUF_SIZE) == (uint32_t)kbd_head;
}

static void kbd_queue_put(char c) {
    if (kbd_queue_full())
        return;
    kbd_buf[kbd_tail] = c;
    kbd_tail          = (kbd_tail + 1u) % KBD_BUF_SIZE;
}

void keyboard_init(void) {
    kbd_head  = 0;
    kbd_tail  = 0;
    kbd_shift = 0;
}

int keyboard_irq(uint8_t sc, int extended) {
    char ch;
    char base;
    char sh;

    if (extended)
        return 0;

    /* Break codes (liberación) */
    if (sc >= 0x80u) {
        if (sc == 0xAAu || sc == 0xB6u)
            kbd_shift = 0;
        return 0;
    }

    /* Shift pulsación */
    if (sc == 0x2Au || sc == 0x36u) {
        kbd_shift = 1;
        return 0;
    }

    /* Caps Lock 0x3A — ignorado en mapa básico US */
    if (sc == 0x3Au)
        return 0;

    /* Ctrl / Alt izq. make — no encolan carácter */
    if (sc == 0x1Du || sc == 0x38u)
        return 0;

    /* ── TEXT_INPUT con foco: capturar toda la entrada de texto ──────────── */
    if (ui_element_count > 0 &&
        focused_element_index >= 0 &&
        focused_element_index < ui_element_count &&
        ui_elements[focused_element_index].type == UI_TYPE_TEXT_INPUT) {

        /* Backspace */
        if (sc == 0x0Eu) { ui_handle_char('\b'); return 1; }

        /* TAB / Enter: avanzar foco al siguiente widget */
        if (sc == 0x0Fu || sc == 0x1Cu) { ui_focus_advance(); return 1; }

        /* ESC: dejar caer (no consumir) para que el bucle exterior lo vea */
        if (sc == 0x01u) return 0;

        /* Caracteres imprimibles */
        base = (sc < 128u) ? sc_us_normal[sc] : 0;
        sh   = (sc < 128u) ? sc_us_shift[sc] : 0;
        if (kbd_shift) {
            ch = sh ? sh : base;
            if (!ch && base >= 'a' && base <= 'z') ch = (char)(base - 32);
        } else {
            ch = base;
        }
        if (ch >= 32) ui_handle_char((unsigned char)ch);
        return 1;  /* consumir siempre cuando TEXT_INPUT tiene el foco */
    }

    /* ── TAB (0x0F) y Enter (0x1C): navegación general en instalador ─────── */
    if (ui_element_count > 0) {
        if (sc == 0x0Fu) {
            ui_focus_advance();
            ui_redraw();
            return 1;
        }
        if (sc == 0x1Cu) {
            ui_activate_focused();
            ui_redraw();
            return 1;
        }
    }

    base = (sc < 128u) ? sc_us_normal[sc] : 0;
    sh   = (sc < 128u) ? sc_us_shift[sc] : 0;

    if (kbd_shift) {
        ch = sh ? sh : base;
        if (!ch && base >= 'a' && base <= 'z')
            ch = (char)(base - 32);
    } else
        ch = base;

    if (!ch)
        return 0;

    kbd_queue_put(ch);
    return 0;
}

char keyboard_getchar(void) {
    char c;

    while (kbd_head == kbd_tail)
        __asm__ volatile("hlt");

    c        = kbd_buf[kbd_head];
    kbd_head = (kbd_head + 1u) % KBD_BUF_SIZE;
    return c;
}
