#ifndef TECLADO_H
#define TECLADO_H

typedef struct { int shift; int caps; int altgr; } KbdState;

typedef enum {
    KBD_EV_NONE = 0,
    KBD_EV_CHAR,
    KBD_EV_ENTER,
    KBD_EV_BACKSPACE,
    KBD_EV_ESC,
    KBD_EV_UP,
    KBD_EV_DOWN,
    KBD_EV_LEFT,
    KBD_EV_RIGHT
} KbdEventType;

typedef struct { KbdEventType type; unsigned char ch; } KbdEvent;

void     kbd_init(KbdState* st);
KbdEvent kbd_handle_scancode(KbdState* st, unsigned char scancode);

#endif
