/*
 * Driver teclado PS/2 — Scan Code Set 1, US QWERTY básico.
 *
 * IRQ1 (keyboard_body → keyboard_irq): solo encola KEY_EVENT crudo en el ring
 * hardware; traducción ASCII y EVENT_KEY_PRESS ocurren al consumir (pop_event).
 */
#include "keyboard.h"
#include "event.h"
#include "event_queue.h"
#include "nexus.h"

static int kbd_shift;

/* Set 1 make codes; 0 = ignorar o manejado aparte. */
static const char sc_us_normal[128] = {
    [0x01] = 0x1B,
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
    [0x1C] = '\n',
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

void keyboard_init(void) {
    kbd_shift = 0;
}

void keyboard_event_flush_state(void) {
    kbd_shift = 0;
}

int keyboard_irq(uint8_t sc, int extended) {
    os_event_t o;

    o.type     = KEY_EVENT;
    o.mouse_x  = extended ? 1 : 0;
    o.mouse_y  = 0;
    o.key_code = (unsigned)sc;
    event_queue_push(&o);
    return 0;
}

/*
 * Convierte un KEY_EVENT del ring hardware en EVENT_KEY_PRESS para la GUI.
 * Devuelve 1 si *out es válido; 0 si el byte solo actualizó estado interno.
 */
int keyboard_os_event_to_gui(const os_event_t* o, Event* out) {
    uint8_t sc;
    char    ch, base, sh;
    int     extended;

    if (!o || !out || o->type != KEY_EVENT)
        return 0;

    sc       = (uint8_t)(o->key_code & 0xFFu);
    extended = o->mouse_x ? 1 : 0;

    if (extended) {
        if (sc >= 0x80u)
            return 0;
        out->type          = EVENT_KEY_PRESS;
        out->scancode      = sc;
        out->ascii         = 0;
        out->key_extended  = 1;
        out->mouse_x       = 0;
        out->mouse_y       = 0;
        out->mouse_buttons = 0;
        out->mouse_pressed = 0;
        out->window_id     = 0;
        return 1;
    }

    if (sc >= 0x80u) {
        if (sc == 0xAAu || sc == 0xB6u)
            kbd_shift = 0;
        return 0;
    }

    if (sc == 0x2Au || sc == 0x36u) {
        kbd_shift = 1;
        return 0;
    }
    if (sc == 0x3Au || sc == 0x1Du || sc == 0x38u)
        return 0;

    base = (sc < 128u) ? sc_us_normal[sc] : 0;
    sh   = (sc < 128u) ? sc_us_shift[sc]  : 0;

    if (kbd_shift) {
        ch = sh ? sh : base;
        if (!ch && base >= 'a' && base <= 'z')
            ch = (char)(base - 32);
    } else {
        ch = base;
    }

    out->type          = EVENT_KEY_PRESS;
    out->scancode      = sc;
    out->ascii         = ch;
    out->key_extended  = 0;
    out->mouse_x       = 0;
    out->mouse_y       = 0;
    out->mouse_buttons = 0;
    out->mouse_pressed = 0;
    out->window_id     = 0;
    return 1;
}

char keyboard_getchar(void) {
    for (;;) {
        Event ev;
        while (pop_event(&ev)) {
            if (ev.type == EVENT_KEY_PRESS && ev.ascii != 0)
                return ev.ascii;
        }
        __asm__ volatile("hlt");
    }
}
