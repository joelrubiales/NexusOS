#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#include "event.h"
#include "event_queue.h"
#include "nexus_status.h"

/* Bits de retorno de mouse_os_event_to_gui() (máscara para event_system). */
#define MOUSE_GUI_MOVE  (1 << 0)
#define MOUSE_GUI_CLICK (1 << 1)

NexusStatus mouse_init(int32_t screen_w, int32_t screen_h);

int32_t  mouse_get_x(void);
int32_t  mouse_get_y(void);
uint8_t  mouse_get_buttons(void);

void mouse_body(void);

int mouse_os_event_to_gui(const os_event_t* o, Event* move_out, Event* click_out);

#endif
