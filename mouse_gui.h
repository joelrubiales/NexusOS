#ifndef NEXUS_MOUSE_GUI_H
#define NEXUS_MOUSE_GUI_H

#include "window.h"

int  nwm_hit_titlebar(const NWM_Window* w, int px, int py, int title_h);
int  nwm_hit_close_button(const NWM_Window* w, int px, int py, int title_h);
void nwm_raise_window(int* zorder, int zcount, int id);
void nwm_apply_window_drag(NWM_Window* w, int mx, int my, int min_y, int max_y);

#endif
