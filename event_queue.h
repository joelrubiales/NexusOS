#ifndef NEXUS_EVENT_QUEUE_H
#define NEXUS_EVENT_QUEUE_H

#include <stdint.h>

/*
 * Cola hardware SPSC: solo drivers (IRQ / xhci_poll) escriben;
 * el consumidor único es event_system (vía pop_event → Event).
 */
#define OS_EVENT_QUEUE_SIZE 256

typedef enum {
    OS_EV_NONE      = 0,
    MOUSE_EVENT     = 1,
    KEY_EVENT       = 2,
} os_event_type_t;

typedef struct {
    int      type;     /* MOUSE_EVENT | KEY_EVENT */
    int      mouse_x;  /* ratón: dx; teclado: 1 si prefijo extendido */
    int      mouse_y;  /* ratón: dy */
    unsigned key_code; /* ratón: byte0 PS/2 (o 0x08|botones USB) + wheel en bits 8–15;
                        * teclado: scancode crudo */
} os_event_t;

void event_queue_push(const os_event_t* e);
int  event_queue_pop(os_event_t* e);
void event_queue_flush(void);

#endif
