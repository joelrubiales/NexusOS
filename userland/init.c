/*
 * Init — primer proceso en Ring 3 (NexusOS).
 * Sin libc: solo nexus_ulib.h + syscalls.
 */
#include "nexus_ulib.h"

static void msg(const char* s) {
    const char* p = s;
    unsigned long n = 0;
    while (p[n])
        n++;
    (void)nexus_write(1, s, n);
}

void _start(void) {
    /* 1) Fondo de escritorio vía Window Manager (compositor en kernel). */
    (void)nexus_wm(1, 0, 0);

    /* 2) Equivalente fork+exec: nuevos binarios PIE en direcciones distintas. */
    if (nexus_spawn("/bin/dock.elf") < 0)
        msg("init: spawn dock failed\n");
    if (nexus_spawn("/bin/installer.elf") < 0)
        msg("init: spawn installer failed\n");

    msg("init: userland session (Ring 3) — WM + dock + installer spawned\n");

    for (;;)
        __asm__ volatile("pause");
}
