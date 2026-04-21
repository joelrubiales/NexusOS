#include "event_queue.h"

/*
 * Ring atómico en el sentido del kernel actual (un núcleo, IF=0 en ISR):
 * barrera de compilador entre dato e índice, igual que event_system.c.
 */
static volatile os_event_t ring[OS_EVENT_QUEUE_SIZE];
static volatile uint32_t   q_head = 0;
static volatile uint32_t   q_tail = 0;

void event_queue_push(const os_event_t* e) {
    uint32_t t, n;

    if (!e)
        return;
    t = q_tail;
    n = (t + 1u) % (uint32_t)OS_EVENT_QUEUE_SIZE;
    if (n == q_head)
        return;

    ring[t] = *e;
    __asm__ volatile("" ::: "memory");
    q_tail = n;
}

int event_queue_pop(os_event_t* e) {
    uint32_t h;

    if (!e)
        return 0;
    h = q_head;
    if (h == q_tail)
        return 0;
    *e = ring[h];
    __asm__ volatile("" ::: "memory");
    q_head = (h + 1u) % (uint32_t)OS_EVENT_QUEUE_SIZE;
    return 1;
}

void event_queue_flush(void) {
    q_head = q_tail;
}
