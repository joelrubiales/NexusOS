#include "event.h"
#include "event_queue.h"
#include "keyboard.h"
#include "mouse.h"
#include "compositor.h"
#include <stdint.h>

/*
 * Dos anillos:
 *   - ring hardware (os_event_t): solo productores drivers → event_queue_*.
 *   - ring software (Event): push_event() desde el kernel (p. ej. WM).
 *
 * pop_event() drena primero el clic diferido, luego el hardware (traduciendo
 * a Event) y por último el ring software.
 */

static volatile Event    evt_queue[EVT_QUEUE_SIZE];
static volatile uint32_t evt_head = 0;
static volatile uint32_t evt_tail = 0;

static Event pending_click;
static int   pending_click_valid;

void push_event(Event e) {
    uint32_t cur_tail = evt_tail;
    uint32_t next     = (cur_tail + 1u) % (uint32_t)EVT_QUEUE_SIZE;

    if (next == evt_head)
        return;

    evt_queue[cur_tail] = e;
    __asm__ volatile("" ::: "memory");
    evt_tail = next;

    compositor_event_bridge(&e);
}

int pop_event(Event* e) {
    uint32_t cur_head;

    if (!e)
        return 0;

    if (pending_click_valid) {
        *e                 = pending_click;
        pending_click_valid = 0;
        compositor_event_bridge(e);
        return 1;
    }

    for (;;) {
        os_event_t o;

        if (!event_queue_pop(&o))
            break;

        if (o.type == KEY_EVENT) {
            Event ke;
            if (!keyboard_os_event_to_gui(&o, &ke))
                continue;
            *e = ke;
            compositor_event_bridge(e);
            return 1;
        }

        if (o.type == MOUSE_EVENT) {
            Event move, click;
            int   m = mouse_os_event_to_gui(&o, &move, &click);

            if (m & MOUSE_GUI_MOVE) {
                *e = move;
                if (m & MOUSE_GUI_CLICK) {
                    pending_click       = click;
                    pending_click_valid = 1;
                }
                compositor_event_bridge(e);
                return 1;
            }
            if (m & MOUSE_GUI_CLICK) {
                *e = click;
                compositor_event_bridge(e);
                return 1;
            }
            continue;
        }
    }

    cur_head = evt_head;
    if (cur_head == evt_tail)
        return 0;

    *e = evt_queue[cur_head];
    __asm__ volatile("" ::: "memory");
    evt_head = (cur_head + 1u) % (uint32_t)EVT_QUEUE_SIZE;
    compositor_event_bridge(e);
    return 1;
}

void flush_events(void) {
    evt_head             = evt_tail;
    pending_click_valid  = 0;
    event_queue_flush();
    keyboard_event_flush_state();
}
