#include "nexus_ulib.h"

void _start(void) {
    (void)nexus_write(1, "dock: Ring 3 (top panel session)\n", 34);
    for (;;) {
        (void)nexus_wm(3, 0, 0);
        __asm__ volatile("pause");
    }
}
