#ifndef NEXUS_PIT_H
#define NEXUS_PIT_H

#include <stdint.h>

/* IRQ0: incrementado en cada interrupción del PIT (ver timer_body → pit_irq_tick). */
extern volatile uint64_t ticks;

void pit_init(void);
void pit_irq_tick(void);

#endif
