#ifndef NEXUS_IPC_H
#define NEXUS_IPC_H

#include <stddef.h>

/*
 * Buzón por tarea (índice del scheduler): ring buffer en kernel.
 * sys_send_msg(dst_tid, ...) encola; sys_recv_msg() bloquea hasta un mensaje.
 */

#define IPC_MSG_MAX 256u
#define IPC_RING_CAP 64u

void ipc_init(void);

long sys_send_msg(int dest_tid, const void* umsg, size_t size);
long sys_recv_msg(void* umsg);

#endif
