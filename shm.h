#ifndef NEXUS_SHM_H
#define NEXUS_SHM_H

#include <stddef.h>

/*
 * Memoria compartida (POSIX-like mínimo).
 * Las páginas físicas se asignan en shm_create; cada tarea que llama a
 * shm_attach obtiene un rango de VA distinto en el mismo espacio lineal
 * (un solo CR3 hoy) — misma RAM, distintas direcciones virtuales por tarea.
 */

void shm_init(void);

long sys_shm_create(size_t size);
long sys_shm_attach(int shm_id);

#endif
