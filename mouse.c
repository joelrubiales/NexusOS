/*
 * Ratón PS/2 (8042 aux port, IRQ12).
 *
 * IRQ12 entra por el PIC esclavo: al final del handler hay que enviar EOI al
 * esclavo (0xA0) y al maestro (0x20). Si falta uno, el PIC deja de entregar
 * IRQs y el ratón parece congelado.
 *
 * Un movimiento = 3 interrupciones: en cada una solo se lee UN byte de 0x60
 * (nunca un bucle que drene el paquete entero en una sola IRQ).
 */
#include "mouse.h"
#include "gfx.h"
#include "nexus.h"
#include "event.h"

volatile int mouse_x = 0;
volatile int mouse_y = 0;
volatile unsigned char mouse_buttons = 0;

static int scr_w = 1;
static int scr_h = 1;

/* Máquina de estados: un byte por IRQ12 (0 → 1 → 2 → aplicar y volver a 0). */
static int mouse_cycle = 0;
static unsigned char mouse_byte[3];

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define ST_OUT_FULL 0x01
#define ST_IN_FULL  0x02

#define PIC1_CMD 0x20
#define PIC2_CMD 0xA0
#define PIC_EOI  0x20

static void ps2_flush_out(void) {
    while (inb(PS2_STATUS) & ST_OUT_FULL)
        (void)inb(PS2_DATA);
}

static void ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & ST_IN_FULL))
            return;
    }
}

/* Espera dato en salida (ACK 0xFA, etc.). 0 = OK, -1 = timeout */
static int ps2_wait_read(void) {
    for (int i = 0; i < 500000; i++) {
        if (inb(PS2_STATUS) & ST_OUT_FULL)
            return 0;
    }
    return -1;
}

static void mouse_pic_eoi(void) {
    /* Obligatorio para IRQ12: primero esclavo, luego maestro. */
    outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

/* Escribe un byte al dispositivo auxiliar (prefijo 0xD4). Comprueba 0xFA / 0xFE. */
static int mouse_dev_write(unsigned char cmd) {
    for (int attempt = 0; attempt < 5; attempt++) {
        ps2_wait_write();
        outb(PS2_CMD, 0xD4);
        ps2_wait_write();
        outb(PS2_DATA, cmd);
        if (ps2_wait_read() != 0)
            continue;
        unsigned char ack = inb(PS2_DATA);
        if (ack == 0xFA)
            return 0;
        if (ack != 0xFE)
            return -1;
    }
    return -1;
}

static void mouse_apply_limits(void) {
    int lim_w = screen_width > 0 ? screen_width : scr_w;
    int lim_h = screen_height > 0 ? screen_height : scr_h;
    if (lim_w < 1)
        lim_w = 1;
    if (lim_h < 1)
        lim_h = 1;
    if (mouse_x < 0)
        mouse_x = 0;
    if (mouse_y < 0)
        mouse_y = 0;
    if (mouse_x >= lim_w)
        mouse_x = lim_w - 1;
    if (mouse_y >= lim_h)
        mouse_y = lim_h - 1;
}

void mouse_init(int sw, int sh) {
    if (sw < 1)
        sw = 1;
    if (sh < 1)
        sh = 1;
    scr_w = sw;
    scr_h = sh;
    mouse_x = sw / 2;
    mouse_y = sh / 2;
    mouse_cycle = 0;

    __asm__ volatile("cli");

    ps2_flush_out();

    /* 0xA8 — habilitar puerto auxiliar (segundo canal). */
    ps2_wait_write();
    outb(PS2_CMD, 0xA8);

    /* Leer Command Byte (0x20), activar IRQ del ratón (bit 1), aux habilitado. */
    ps2_wait_write();
    outb(PS2_CMD, 0x20);
    if (ps2_wait_read() == 0) {
        unsigned char cb = inb(PS2_DATA);
        cb |= 0x02;
        cb &= (unsigned char)~0x20;
        ps2_wait_write();
        outb(PS2_CMD, 0x60);
        ps2_wait_write();
        outb(PS2_DATA, cb);
    }

    ps2_flush_out();

    (void)mouse_dev_write(0xF6);

    /*
     * Antes de 0xF4: vaciar de nuevo 0x60 (restos de BIOS, ACKs o basura)
     * para no arrancar el streaming con el parser desincronizado.
     */
    ps2_flush_out();
    ps2_flush_out();

    (void)mouse_dev_write(0xF4);

    __asm__ volatile("sti");

    mouse_apply_limits();
}

void mouse_body(void) {
    unsigned char st = inb(PS2_STATUS);

    /*
     * IRQ espuria o dato aún no listo: no leer 0x60. Reiniciar ciclo para no
     * mezclar bytes de dos paquetes distintos (síntoma típico de “ratón pillado”).
     */
    if (!(st & ST_OUT_FULL)) {
        mouse_cycle = 0;
        goto eoi;
    }

    unsigned char data = inb(PS2_DATA);

    if (mouse_cycle == 0) {
        if (!(data & 0x08)) {
            mouse_cycle = 0;
            goto eoi;
        }
        mouse_byte[0] = data;
        mouse_cycle   = 1;
    } else if (mouse_cycle == 1) {
        mouse_byte[1] = data;
        mouse_cycle   = 2;
    } else {
        mouse_byte[2] = data;
        mouse_cycle   = 0;

        unsigned char old_buttons = mouse_buttons;
        mouse_buttons = mouse_byte[0] & 0x07;

        int dx = (int)mouse_byte[1];
        if (mouse_byte[0] & 0x10)
            dx |= 0xFFFFFF00;
        int dy = (int)mouse_byte[2];
        if (mouse_byte[0] & 0x20)
            dy |= 0xFFFFFF00;

        if (!(mouse_byte[0] & 0xC0)) {
            mouse_x += dx;
            mouse_y -= dy;
            mouse_apply_limits();

            /* ── EVENT_MOUSE_MOVE ─────────────────────────────────── */
            {
                Event ev;
                ev.type          = EVENT_MOUSE_MOVE;
                ev.mouse_x       = mouse_x;
                ev.mouse_y       = mouse_y;
                ev.mouse_buttons = mouse_buttons;
                ev.mouse_pressed = 0;
                ev.scancode      = 0;
                ev.ascii         = 0;
                ev.key_extended  = 0;
                ev.window_id     = 0;
                push_event(ev);
            }
        }

        /* ── EVENT_MOUSE_CLICK (solo si cambiaron los botones) ───── */
        if (mouse_buttons != old_buttons) {
            Event ev;
            unsigned char changed = mouse_buttons ^ old_buttons;
            ev.type          = EVENT_MOUSE_CLICK;
            ev.mouse_x       = mouse_x;
            ev.mouse_y       = mouse_y;
            ev.mouse_buttons = mouse_buttons;
            /* pressed=1 si algún bit pasó de 0→1; 0 si fue 1→0 (release) */
            ev.mouse_pressed = (mouse_buttons & changed) ? 1 : 0;
            ev.scancode      = 0;
            ev.ascii         = 0;
            ev.key_extended  = 0;
            ev.window_id     = 0;
            push_event(ev);
        }
    }

eoi:
    mouse_pic_eoi();
}
