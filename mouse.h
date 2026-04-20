#ifndef MOUSE_H
#define MOUSE_H

void mouse_init(int screen_w, int screen_h);

extern volatile int mouse_x;
extern volatile int mouse_y;
extern volatile unsigned char mouse_buttons;

void mouse_body(void);

#endif
