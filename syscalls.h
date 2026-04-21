#ifndef NEXUS_SYSCALLS_H
#define NEXUS_SYSCALLS_H

#include <stdint.h>

/*
 * Syscalls estilo Linux x86-64 (instrucción syscall, MSR LSTAR).
 * Números alineados con Linux para portar libc mínima.
 */
#define NX_SYS_read    0
#define NX_SYS_write   1
#define NX_SYS_open    2
#define NX_SYS_close   3
#define NX_SYS_shm_create 16
#define NX_SYS_shm_attach 17
#define NX_SYS_send_msg   18
#define NX_SYS_recv_msg   19
#define NX_SYS_wm         25
#define NX_SYS_spawn      32

#define NX_O_RDONLY    0
#define NX_O_WRONLY    1
#define NX_O_RDWR      2
#define NX_O_CREAT     0100
#define NX_O_TRUNC     01000
#define NX_O_APPEND    02000

void syscalls_init(void);

/* Tabla de FDs por tid (tareas usuario). */
void nexus_pcb_init_for_tid(int tid);

#endif
