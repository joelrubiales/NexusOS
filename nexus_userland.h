#ifndef NEXUS_USERLAND_H
#define NEXUS_USERLAND_H

#include <stdint.h>

/* Inicializa capa compositor + wallpaper (syscall WM background). */
void nexus_userland_graphics_prepare(void);

/*
 * Comandos WM (syscall NX_SYS_wm, RDI=cmd).
 * 1 = preparar fondo de escritorio / wallpaper en compositor
 * 2 = un frame del instalador gráfico (kernel pinta)
 * 3 = no-op reservado (dock / top panel)
 */
long nexus_wm_dispatch(int cmd, uint64_t a1, uint64_t a2);

/* Carga ELF desde ruta kernel (null-term); devuelve tid o neg errno. */
long nexus_spawn_elf_kpath(const char* kpath);

/* Carga /bin/init.elf, crea tarea usuario, habilita scheduler. 0=ok, -1=fallo. */
int nexus_try_boot_userland(void);

#endif
