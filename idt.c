#include "nexus.h"
#include "pit.h"
#include "keyboard.h"
#include "task.h"

struct idt_entry {
    unsigned short base_low;
    unsigned short sel;
    unsigned char  ist;
    unsigned char  flags;
    unsigned short base_mid;
    unsigned int   base_high;
    unsigned int   reserved;
} __attribute__((packed));

struct idt_ptr {
    unsigned short     limit;
    unsigned long long base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

volatile unsigned char tecla_nueva    = 0;
volatile unsigned char tecla_extended = 0;
static   volatile unsigned char _ext_pending = 0;

/*
 * timer_body: solo se usa mientras sched_timer_handler NO está en la IDT
 * (es decir, nunca en producción; se conserva para pruebas unitarias aisladas).
 * En el sistema normal, sched_timer_handler → sched_tick hace el tick + EOI.
 */
void timer_body(void) {
    pit_irq_tick();
    outb(0x20, 0x20);
}

void irq_stub_body(void) {
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void keyboard_body(void) {
    unsigned char sc = inb(0x60);
    outb(0x20, 0x20);

    if (sc == 0xE0) { _ext_pending = 1; return; }
    if (_ext_pending) { _ext_pending = 0; tecla_extended = 1; }
    else              { tecla_extended = 0; }

    /* Encola KEY_EVENT crudo; pop_event / keyboard_getchar traducen. */
    keyboard_irq(sc, tecla_extended);

    /* Ruta legacy: gui_run() todavía procesa tecla_nueva con KbdState. */
    tecla_nueva = sc;
}

extern void timer_handler(void);
extern void keyboard_handler(void);
extern void mouse_handler(void);
extern void irq_stub(void);
extern void isr_halt(void);
extern void sched_timer_handler(void);  /* IRQ0 preventivo (scheduler.asm) */

static void set_idt_gate(int n, void (*handler)(void)) {
    unsigned long long addr = (unsigned long long)(__UINTPTR_TYPE__)handler;
    idt[n].base_low  = (unsigned short)( addr        & 0xFFFF);
    idt[n].sel       = 0x18;
    idt[n].ist       = 0;
    idt[n].flags     = 0x8E;
    idt[n].base_mid  = (unsigned short)((addr >> 16) & 0xFFFF);
    idt[n].base_high = (unsigned int)  ((addr >> 32) & 0xFFFFFFFF);
    idt[n].reserved  = 0;
}

void instalar_idt(void) {
    idtp.limit = (unsigned short)(sizeof(struct idt_entry) * 256 - 1);
    idtp.base  = (unsigned long long)(__UINTPTR_TYPE__)idt;

    for(int i =  0; i < 32; i++) set_idt_gate(i, isr_halt);
    for(int i = 32; i < 48; i++) set_idt_gate(i, irq_stub);

    pit_init();

    /* PIC remap: IRQ0-7 -> INT 32-39, IRQ8-15 -> INT 40-47 */
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);

    /* Unmask IRQ0 (timer), IRQ1 (keyboard) on PIC1 */
    outb(0x21, 0xFC);
    /* Unmask IRQ12 (mouse) on PIC2: bit4 = IRQ12 */
    outb(0xA1, 0xEF);

    /*
     * IRQ0: usar el handler preventivo del scheduler.
     * sched_tick() gestiona ticks++, EOI y el cambio de contexto.
     * Mientras sched_enabled == 0, solo hace tick+EOI sin switch.
     */
    set_idt_gate(32, sched_timer_handler); /* IRQ0  -> INT 32 */
    set_idt_gate(33, keyboard_handler);    /* IRQ1  -> INT 33 */
    set_idt_gate(44, mouse_handler);       /* IRQ12 -> INT 44 */

    keyboard_init();
    sched_init();

    __asm__ volatile("lidt %0" : : "m"(idtp));
    __asm__ volatile("sti");
}

void idt_irq_install(uint8_t irq, void (*handler_asm)(void)) {
    if (irq >= 16u || !handler_asm)
        return;
    set_idt_gate(32 + (int)irq, handler_asm);
}

void pic_irq_unmask(uint8_t irq) {
    if (irq >= 16u)
        return;
    if (irq < 8u)
        outb(0x21, (unsigned char)(inb(0x21) & (unsigned char)~(1u << irq)));
    else
        outb(0xA1, (unsigned char)(inb(0xA1) & (unsigned char)~(1u << (irq - 8u))));
}
