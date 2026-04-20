#include "pit.h"
#include "nexus.h"

/*
 * PIT canal 0, modo 3 (cuadrado), acceso lobyte/hibyte.
 * Puerto 0x43: comando. Puerto 0x40: divisor (CH0).
 * Frecuencia base ~1.193182 MHz; divisor = 1193182 / hz.
 */
volatile uint64_t ticks = 0;

void pit_init(void) {
    uint32_t hz = (uint32_t)PIT_TICKS_PER_SEC;
    uint32_t divisor = 1193182u / hz;
    if (divisor > 0xFFFFu)
        divisor = 0xFFFFu;
    if (divisor < 1u)
        divisor = 1u;

    outb(0x43, 0x36);
    outb(0x40, (unsigned char)(divisor & 0xFFu));
    outb(0x40, (unsigned char)((divisor >> 8) & 0xFFu));
}

void pit_irq_tick(void) {
    ticks++;
}
