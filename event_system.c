#include "event.h"
#include <stdint.h>

/*
 * Ring buffer SPSC para NexusOS.
 *
 * Diseño:
 *   evt_queue[EVT_QUEUE_SIZE]  — slots de datos.
 *   evt_tail  — índice donde el PRODUCTOR escribe el siguiente evento.
 *   evt_head  — índice desde el que el CONSUMIDOR lee el siguiente evento.
 *
 * Invariante: la cola está vacía si head == tail; llena si (tail+1)%N == head.
 *
 * Garantía de orden (x86-64, TSO):
 *   push():  escribe el dato en evt_queue[tail], luego actualiza tail.
 *            Un barrier de compilador entre ambas impide reordenamiento.
 *   pop():   lee tail, luego lee el dato de evt_queue[head].
 *            Un barrier análogo previene que el compilador anticipe la lectura.
 *
 * En x86-64 las stores no se reordenan entre sí (TSO), así que el barrier
 * de compilador es suficiente — no se necesita mfence/lfence.
 */

static volatile Event    evt_queue[EVT_QUEUE_SIZE];
static volatile uint32_t evt_head = 0;   /* consumer index */
static volatile uint32_t evt_tail = 0;   /* producer index */

void push_event(Event e) {
    uint32_t cur_tail = evt_tail;
    uint32_t next     = (cur_tail + 1u) % (uint32_t)EVT_QUEUE_SIZE;

    if (next == evt_head)
        return;  /* buffer lleno: descartar (política lossy, no bloquear ISR) */

    evt_queue[cur_tail] = e;

    /* Barrier de compilador: asegurar que el dato se escribe ANTES de avanzar
     * el índice tail visible al consumidor. */
    __asm__ volatile("" ::: "memory");

    evt_tail = next;
}

int pop_event(Event* e) {
    uint32_t cur_head = evt_head;

    if (cur_head == evt_tail)
        return 0;  /* vacío */

    *e = evt_queue[cur_head];

    /* Barrier: leer el dato ANTES de liberar el slot avanzando head. */
    __asm__ volatile("" ::: "memory");

    evt_head = (cur_head + 1u) % (uint32_t)EVT_QUEUE_SIZE;
    return 1;
}

void flush_events(void) {
    evt_head = evt_tail;
}
