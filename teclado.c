#include "teclado.h"
#include "nexus.h"

// ─── Teclado Español ISO – sin modificador ────────────────────────────────────
static const unsigned char teclado_normal[128] = {
/*00*/  0,   27,  '1', '2', '3', '4', '5', '6',
/*08*/  '7', '8', '9', '0','\'','\xAD','\b','\t',
/*10*/  'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
/*18*/  'o', 'p', '`', '+', '\n', 0,  'a', 's',
/*20*/  'd', 'f', 'g', 'h', 'j', 'k', 'l','\xA4',
/*28*/  '\'','\xA7', 0,  '<', 'z', 'x', 'c', 'v',
/*30*/  'b', 'n', 'm', ',', '.', '-',  0,  '*',
/*38*/   0,  ' ',  0,   0,   0,   0,   0,   0,
/*40*/   0,   0,   0,   0,   0,   0,   0,   0,
/*48*/   0,   0,   0,   0,   0,   0,   0,   0,
/*50*/   0,   0,   0,   0,   0,   0,   0,   0,
/*58*/   0,   0,   0,   0,   0,   0,   0,   0,
/*60*/   0,   0,   0,   0,   0,   0,   0,   0,
/*68*/   0,   0,   0,   0,   0,   0,   0,   0,
/*70*/   0,   0,   0,   0,   0,   0,   0,   0,
/*78*/   0,   0,   0,   0,   0,   0,   0,   0,
};

// ─── Con Shift ────────────────────────────────────────────────────────────────
static const unsigned char teclado_shift[128] = {
/*00*/  0,   27,  '!', '"', '#', '$', '%', '&',
/*08*/  '/', '(', ')', '=', '?','\xA8','\b','\t',
/*10*/  'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
/*18*/  'O', 'P', '^', '*', '\n', 0,  'A', 'S',
/*20*/  'D', 'F', 'G', 'H', 'J', 'K', 'L','\xA5',
/*28*/  '"', '\xA6', 0,  '>',  'Z', 'X', 'C', 'V',
/*30*/  'B', 'N', 'M', ';', ':', '_',  0,  '*',
/*38*/   0,  ' ',  0,   0,   0,   0,   0,   0,
/*40-7F: 0 */
          0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
};

// ─── Con AltGr (Right Alt) ────────────────────────────────────────────────────
static const unsigned char teclado_altgr[128] = {
/*00*/  0,   0,   '|', '@', '#', '~',  0,   0,
/*08*/  0,   0,   0,   0,   0,   0,   0,   0,
/*10*/  0,   0,   0,   0,   0,   0,   0,   0,
/*18*/  0,   0,  '[', ']', '\n', 0,   0,   0,
/*20*/  0,   0,   0,   0,   0,   0,   0,   0,
/*28*/ '{', '\\', 0,  '}',  0,   0,   0,   0,
/*30*/  0,   0,   0,   0,   0,   0,   0,   0,
/*38*/  0,   0,   0,   0,   0,   0,   0,   0,
/*40-7F: 0 */
          0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
};

void kbd_init(KbdState* st) { st->shift = 0; st->caps = 0; st->altgr = 0; }

KbdEvent kbd_handle_scancode(KbdState* st, unsigned char sc) {
    KbdEvent ev; ev.type = KBD_EV_NONE; ev.ch = 0;

    // ── Teclas extendidas (prefijo E0) ────────────────────────────────────
    if(tecla_extended) {
        if(sc == 0x48) { ev.type = KBD_EV_UP;    return ev; }
        if(sc == 0x50) { ev.type = KBD_EV_DOWN;  return ev; }
        if(sc == 0x4B) { ev.type = KBD_EV_LEFT;  return ev; }
        if(sc == 0x4D) { ev.type = KBD_EV_RIGHT; return ev; }
        if(sc == 0x38) { st->altgr = 1; return ev; }  // AltGr press
        if(sc == 0xB8) { st->altgr = 0; return ev; }  // AltGr release
        return ev;
    }

    // ── Modificadores ─────────────────────────────────────────────────────
    if(sc == 0x2A || sc == 0x36) { st->shift = 1; return ev; } // Shift press
    if(sc == 0xAA || sc == 0xB6) { st->shift = 0; return ev; } // Shift release
    if(sc == 0x3A) { st->caps = !st->caps;  return ev; }       // Caps Lock
    if(sc == 0x38) { return ev; }                               // Left Alt (ignorar)
    if(sc >= 0x80) return ev;                                   // key-release
    if(sc == 0x01) { ev.type = KBD_EV_ESC; return ev; }

    // ── AltGr ─────────────────────────────────────────────────────────────
    if(st->altgr) {
        unsigned char ch = teclado_altgr[sc];
        if(!ch) return ev;
        ev.type = KBD_EV_CHAR; ev.ch = ch;
        return ev;
    }

    // ── Carácter normal ───────────────────────────────────────────────────
    unsigned char base = teclado_normal[sc];
    int es_letra = (base >= 'a' && base <= 'z') ||
                   (base == (unsigned char)'\xA4');  // incluye ñ
    int usar_shift = st->shift;
    if(es_letra && st->caps) usar_shift = !usar_shift;

    unsigned char ch = usar_shift ? teclado_shift[sc] : teclado_normal[sc];
    if(!ch) return ev;

    if(ch == '\n') { ev.type = KBD_EV_ENTER;     return ev; }
    if(ch == '\b') { ev.type = KBD_EV_BACKSPACE;  return ev; }
    ev.type = KBD_EV_CHAR; ev.ch = ch;
    return ev;
}
