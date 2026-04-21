/*
 * Ratón PS/2 (8042 aux, IRQ12).
 *
 * Inicialización agresiva del 8042 + máquina de estados con resincronización
 * (bit 0x08 en el primer byte). Estado: mouse_get_*().
 */
#include "mouse.h"
#include "event_queue.h"
#include "nexus.h"
#include "xhci.h"

#include <stdint.h>

extern int screen_width;
extern int screen_height;

static volatile struct {
    int32_t  pos_x;
    int32_t  pos_y;
    uint8_t  buttons;
} g_mouse;

static int32_t g_logic_w = 1;
static int32_t g_logic_h = 1;

#define MOUSE_PACKET_CAP 4u

static uint8_t mouse_packet[MOUSE_PACKET_CAP];
static int     mouse_cycle        = 0;
static int     mouse_packet_len   = 3;

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

/* Puerto 0x64: bit0 salida lista (leer 0x60); bit1 entrada llena (no escribir). */
#define ST_OUT_FULL 0x01u
#define ST_IN_FULL  0x02u

#define PS2_CMD_DISABLE_KBD 0xADu
#define PS2_CMD_DISABLE_AUX 0xA7u
#define PS2_CMD_ENABLE_AUX  0xA8u
#define PS2_CMD_ENABLE_KBD  0xAEu
#define PS2_CMD_WRITE_AUX   0xD4u
#define PS2_CMD_READ_CB     0x20u
#define PS2_CMD_WRITE_CB    0x60u

#define PS2_MOUSE_ACK   0xFAu
#define PS2_MOUSE_RESEND 0xFEu

#define PIC_MASTER_CMD 0x20u
#define PIC_SLAVE_CMD  0xA0u
#define PIC_EOI        0x20u

#define PS2_SPIN_MAX_READ  500000u
#define PS2_SPIN_MAX_WRITE 256000u
#define PS2_DRAIN_MAX_OPS  65536u

int32_t mouse_get_x(void) {
    return g_mouse.pos_x;
}

int32_t mouse_get_y(void) {
    return g_mouse.pos_y;
}

uint8_t mouse_get_buttons(void) {
    return g_mouse.buttons;
}

static void ps2_drain_output(void) {
    uint32_t n = 0;
    while ((inb(PS2_STATUS) & ST_OUT_FULL) && n < PS2_DRAIN_MAX_OPS) {
        (void)inb(PS2_DATA);
        n++;
    }
}

/*
 * Espera a que el buffer de entrada del controlador esté vacío (bit1 == 0)
 * antes de escribir en 0x64 / 0x60.
 */
static void ps2_wait_input_empty(void) {
    uint32_t i;
    for (i = 0; i < PS2_SPIN_MAX_WRITE; i++) {
        if (!(inb(PS2_STATUS) & ST_IN_FULL))
            return;
    }
}

static int ps2_wait_output_ready(void) {
    uint32_t i;
    for (i = 0; i < PS2_SPIN_MAX_READ; i++) {
        if (inb(PS2_STATUS) & ST_OUT_FULL)
            return 0;
    }
    return -1;
}

static void mouse_send_pic_eoi_both(void) {
    outb(PIC_SLAVE_CMD, PIC_EOI);
    outb(PIC_MASTER_CMD, PIC_EOI);
}

static void ps2_controller_cmd(uint8_t cmd) {
    ps2_wait_input_empty();
    outb(PS2_CMD, cmd);
}

/*
 * Reinicio agresivo: vaciar salida, desactivar ambos puertos, vaciar de nuevo,
 * activar solo el auxiliar (el teclado se rehabilita al final de mouse_init).
 */
static void ps2_aggressive_aux_prep(void) {
    ps2_drain_output();

    ps2_controller_cmd(PS2_CMD_DISABLE_KBD);
    ps2_drain_output();

    ps2_controller_cmd(PS2_CMD_DISABLE_AUX);
    ps2_drain_output();

    ps2_controller_cmd(PS2_CMD_ENABLE_AUX);
    ps2_drain_output();
}

/* Escribe un byte al ratón (puerto aux); espera ACK 0xFA o reintenta RESEND. */
static int mouse_dev_write(unsigned char cmd) {
    int attempt;
    for (attempt = 0; attempt < 8; attempt++) {
        ps2_wait_input_empty();
        outb(PS2_CMD, PS2_CMD_WRITE_AUX);
        ps2_wait_input_empty();
        outb(PS2_DATA, cmd);
        if (ps2_wait_output_ready() != 0)
            continue;
        {
            unsigned char ack = inb(PS2_DATA);
            if (ack == PS2_MOUSE_ACK)
                return 0;
            if (ack != PS2_MOUSE_RESEND)
                return -1;
        }
    }
    return -1;
}

static int mouse_read_data_byte(unsigned char* out) {
    if (!out)
        return -1;
    if (ps2_wait_output_ready() != 0)
        return -1;
    *out = inb(PS2_DATA);
    return 0;
}

static int mouse_get_device_id(unsigned char* id_out) {
    if (!id_out)
        return -1;
    if (mouse_dev_write(0xF2) != 0)
        return -1;
    return mouse_read_data_byte(id_out);
}

static void mouse_probe_intellimouse(void) {
    unsigned char id = 0;

    mouse_packet_len = 3;

    (void)mouse_dev_write(0xF3);
    (void)mouse_dev_write(200u);
    (void)mouse_dev_write(0xF3);
    (void)mouse_dev_write(100u);
    (void)mouse_dev_write(0xF3);
    (void)mouse_dev_write(80u);

    ps2_drain_output();

    if (mouse_get_device_id(&id) == 0 && (id == 3u || id == 4u))
        mouse_packet_len = 4;
}

static int mouse_init_plain_streaming(void) {
    ps2_drain_output();
    (void)mouse_dev_write(0xF5);
    ps2_drain_output();
    if (mouse_dev_write(0xF6) != 0)
        return -1;
    ps2_drain_output();
    mouse_packet_len = 3;
    return mouse_dev_write(0xF4);
}

static void mouse_apply_limits(void) {
    int32_t lim_w = screen_width > 0 ? (int32_t)screen_width : g_logic_w;
    int32_t lim_h = screen_height > 0 ? (int32_t)screen_height : g_logic_h;
    if (lim_w < 1)
        lim_w = 1;
    if (lim_h < 1)
        lim_h = 1;
    if (g_mouse.pos_x < 0)
        g_mouse.pos_x = 0;
    if (g_mouse.pos_y < 0)
        g_mouse.pos_y = 0;
    if (g_mouse.pos_x >= lim_w)
        g_mouse.pos_x = lim_w - 1;
    if (g_mouse.pos_y >= lim_h)
        g_mouse.pos_y = lim_h - 1;
}

static void mouse_enqueue_ps2_packet(int use_scroll_byte) {
    unsigned char b0 = mouse_packet[0];
    int           dx = (int)mouse_packet[1];
    int           dy = (int)mouse_packet[2];
    int           dz = 0;
    os_event_t    o;

    if (b0 & 0x10)
        dx |= 0xFFFFFF00;
    if (b0 & 0x20)
        dy |= 0xFFFFFF00;

    if (use_scroll_byte && mouse_packet_len >= 4)
        dz = (int)(int8_t)mouse_packet[3];

    o.type     = MOUSE_EVENT;
    o.mouse_x  = dx;
    o.mouse_y  = dy;
    o.key_code = (unsigned)b0 | (((unsigned)dz & 0xFFu) << 8);
    event_queue_push(&o);
}

/*
 * IRQ12 habilitado en el byte de configuración del 8042: bit1, y reloj ratón activo (~bit5).
 */
static void mouse_ps2_program_command_byte_irq(void) {
    ps2_drain_output();

    ps2_controller_cmd(PS2_CMD_READ_CB);
    if (ps2_wait_output_ready() != 0)
        return;
    {
        unsigned char cb = inb(PS2_DATA);
        cb |= 0x02u;
        cb &= (unsigned char)~0x20u;
        ps2_wait_input_empty();
        outb(PS2_CMD, PS2_CMD_WRITE_CB);
        ps2_wait_input_empty();
        outb(PS2_DATA, cb);
    }
    ps2_drain_output();
}

NexusStatus mouse_init(int32_t sw, int32_t sh) {
    if (sw < 1)
        sw = 1;
    if (sh < 1)
        sh = 1;
    g_logic_w = sw;
    g_logic_h = sh;
    g_mouse.pos_x    = sw / 2;
    g_mouse.pos_y    = sh / 2;
    mouse_cycle      = 0;
    mouse_packet_len = 3;

    if (xhci_usb_mouse_active) {
        xhci_set_screen_dims((int)sw, (int)sh);
        return NEXUS_OK;
    }

    __asm__ volatile("cli");

    ps2_aggressive_aux_prep();

    /* Valores por defecto (F6) y ACK; luego sondeo rueda; streaming (F4) y ACK. */
    if (mouse_dev_write(0xF6) != 0) {
        ps2_drain_output();
        (void)mouse_dev_write(0xF6);
    }
    ps2_drain_output();

    mouse_probe_intellimouse();

    ps2_drain_output();

    if (mouse_dev_write(0xF4) != 0) {
        (void)mouse_init_plain_streaming();
    }

    mouse_ps2_program_command_byte_irq();

    /* Rehabilitar teclado en el primer puerto (0xAD lo desactivó al inicio). */
    ps2_controller_cmd(PS2_CMD_ENABLE_KBD);
    ps2_drain_output();

    __asm__ volatile("sti");

    mouse_apply_limits();
    return NEXUS_OK;
}

/*
 * Máquina de estados: en cycle 0 solo acepta byte con bit 0x08 (sync).
 * Cabecera en mitad de frame → resincronizar sin bloquear el drenaje del 8042.
 */
static int mouse_ps2_feed_byte(unsigned char data) {
    if (mouse_cycle == 0) {
        if (!(data & 0x08u))
            return 1;
        mouse_packet[0] = data;
        mouse_cycle     = 1;
        return 1;
    }

    if (mouse_cycle == 3) {
        if ((data & 0x08u) != 0) {
            /* Íbamos a por scroll pero llegó nueva cabecera: cerrar como 3-byte. */
            mouse_enqueue_ps2_packet(0);
            mouse_packet[0] = data;
            mouse_cycle     = 1;
            return 1;
        }
        mouse_packet[3] = data;
        mouse_cycle     = 0;
        mouse_enqueue_ps2_packet(1);
        return 1;
    }

    if ((data & 0x08u) != 0 && (mouse_cycle == 1 || mouse_cycle == 2)) {
        mouse_packet[0] = data;
        mouse_cycle     = 1;
        return 1;
    }

    if (mouse_cycle == 1) {
        mouse_packet[1] = data;
        mouse_cycle     = 2;
        return 1;
    }

    if (mouse_cycle == 2) {
        mouse_packet[2] = data;
        if (mouse_packet_len >= 4) {
            mouse_cycle = 3;
            return 1;
        }
        mouse_cycle = 0;
        mouse_enqueue_ps2_packet(0);
        return 1;
    }

    return 1;
}

int mouse_os_event_to_gui(const os_event_t* o, Event* move_out, Event* click_out) {
    unsigned char b0, old_buttons, new_buttons;
    unsigned char changed;
    int           dz;
    int           ret = 0;

    if (!o || !move_out || !click_out || o->type != MOUSE_EVENT)
        return 0;

    b0          = (unsigned char)(o->key_code & 0xFFu);
    dz          = (int)(int8_t)((o->key_code >> 8) & 0xFFu);
    old_buttons = g_mouse.buttons;
    new_buttons = b0 & 0x07u;

    g_mouse.buttons = new_buttons;

    if (!(b0 & 0xC0)) {
        int dx = o->mouse_x;
        int dy = o->mouse_y;
        g_mouse.pos_x += dx;
        g_mouse.pos_y -= dy;
        if (dz != 0)
            g_mouse.pos_y -= dz;
        mouse_apply_limits();

        move_out->type          = EVENT_MOUSE_MOVE;
        move_out->mouse_x       = (int)g_mouse.pos_x;
        move_out->mouse_y       = (int)g_mouse.pos_y;
        move_out->mouse_buttons = g_mouse.buttons;
        move_out->mouse_pressed = 0;
        move_out->scancode      = 0;
        move_out->ascii         = 0;
        move_out->key_extended  = 0;
        move_out->window_id     = 0;
        ret |= MOUSE_GUI_MOVE;
    }

    if (new_buttons != old_buttons) {
        changed = new_buttons ^ old_buttons;
        click_out->type          = EVENT_MOUSE_CLICK;
        click_out->mouse_x       = (int)g_mouse.pos_x;
        click_out->mouse_y       = (int)g_mouse.pos_y;
        click_out->mouse_buttons = g_mouse.buttons;
        click_out->mouse_pressed = (new_buttons & changed) ? 1 : 0;
        click_out->scancode      = 0;
        click_out->ascii         = 0;
        click_out->key_extended  = 0;
        click_out->window_id     = 0;
        ret |= MOUSE_GUI_CLICK;
    }

    return ret;
}

void mouse_body(void) {
    if (xhci_usb_mouse_active)
        goto eoi;

    for (;;) {
        unsigned char st = inb(PS2_STATUS);
        if (!(st & ST_OUT_FULL))
            break;
        (void)mouse_ps2_feed_byte(inb(PS2_DATA));
    }

eoi:
    mouse_send_pic_eoi_both();
}
