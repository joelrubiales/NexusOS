#ifndef MULTITASKING_H
#define MULTITASKING_H

/* Cooperativo round-robin (x86-64): yield() con cpu_switch.
 * Preemptivo real desde IRQ0 requiere guardar marco IRQ completo + IST. */

void multitasking_init(void);

void cpu_switch(unsigned long long *old_sp_out, unsigned long long new_sp);

void yield(void);

/* Ejecuta ~20 cambios de contexto entre dos hilos de prueba; vuelve al llamador */
void coop_run(void);

unsigned multitasking_selftest(void);
unsigned long long multitasking_count_a(void);
unsigned long long multitasking_count_b(void);

#endif
