#ifndef SMP_H
#define SMP_H

#include <stdint.h>

#define SMP_MAX_CPUS 16u

typedef struct {
    volatile uint32_t v;
} spinlock_t;

#define SPINLOCK_INIT ((spinlock_t){0})

void spinlock_init(spinlock_t* lk);
void spinlock_lock(spinlock_t* lk);
void spinlock_unlock(spinlock_t* lk);

/* Tras smp_init(): número de CPUs locales en MADT (incl. BSP). */
extern unsigned smp_cpu_count;

/* Spinlocks globales (inicializados en smp_init). */
extern spinlock_t smp_kmalloc_lock;
extern spinlock_t smp_console_lock;

/* 1 si hay más de un núcleo según MADT (bloqueos en kmalloc / consola). */
static inline int smp_needs_locks(void) { return smp_cpu_count > 1u; }

/*
 * ACPI + INIT-SIPI-SIPI. Llama tras memory_init y mapa LFB (identidad < 4 GiB).
 * Despierta APs; cada uno ejecuta smp_ap_idle() (hlt + pause).
 */
void smp_init(uint32_t mb2_info_phys);

uint32_t smp_bsp_apic_id(void);
uint32_t smp_lapic_read_id(void);

/* Primera I/O APIC de la MADT (MMIO físico); 0 si no hay. Próximo paso: redirigir IRQ. */
extern uint32_t smp_ioapic_phys;

#endif
