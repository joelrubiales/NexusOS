#include "ipc.h"
#include "task.h"
#include "memory.h"
#include <stdint.h>

typedef struct {
    uint16_t len;
    uint8_t  data[IPC_MSG_MAX];
} IpcSlot;

typedef struct {
    volatile uint32_t head;
    volatile uint32_t tail;
    IpcSlot           slots[IPC_RING_CAP];
} IpcRing;

static IpcRing ipc_rings[SCHED_MAX_TASKS];
static int     ipc_send_block_dst[SCHED_MAX_TASKS];

void ipc_init(void) {
    int t;
    for (t = 0; t < SCHED_MAX_TASKS; t++) {
        ipc_rings[t].head = 0;
        ipc_rings[t].tail = 0;
        ipc_send_block_dst[t] = -1;
    }
}

static int ipc_ring_push(int dest_tid, const void* kmsg, size_t size) {
    IpcRing* r;
    uint32_t h, t, next;

    if (dest_tid < 0 || dest_tid >= SCHED_MAX_TASKS)
        return 0;
    if (size == 0 || size > IPC_MSG_MAX)
        return 0;

    r = &ipc_rings[dest_tid];
    __asm__ volatile("cli");
    h = r->head;
    t = r->tail;
    next = (t + 1u) % IPC_RING_CAP;
    if (next == h) {
        __asm__ volatile("sti");
        return 0; /* lleno */
    }
    r->slots[t].len = (uint16_t)size;
    {
        size_t i;
        const uint8_t* s = (const uint8_t*)kmsg;
        for (i = 0; i < size; i++)
            r->slots[t].data[i] = s[i];
    }
    r->tail = next;
    __asm__ volatile("sti");
    return 1;
}

static int ipc_ring_pop(int my_tid, void* kbuf, uint16_t* out_len) {
    IpcRing* r;
    uint32_t h, t, next;

    if (my_tid < 0 || my_tid >= SCHED_MAX_TASKS)
        return 0;

    r = &ipc_rings[my_tid];
    __asm__ volatile("cli");
    h = r->head;
    t = r->tail;
    if (h == t) {
        __asm__ volatile("sti");
        return 0;
    }
    next = (h + 1u) % IPC_RING_CAP;
    *out_len = r->slots[h].len;
    {
        uint16_t i;
        uint8_t* d = (uint8_t*)kbuf;
        for (i = 0; i < *out_len; i++)
            d[i] = r->slots[h].data[i];
    }
    r->head = next;
    {
        int i;
        for (i = 0; i < sched_task_count; i++) {
            if (ipc_send_block_dst[i] == my_tid && sched_tasks[i].state == TASK_SLEEP)
                sched_tasks[i].state = TASK_READY;
        }
    }
    __asm__ volatile("sti");
    return 1;
}

static int user_range_ok(uintptr_t uaddr, size_t len) {
    uintptr_t end;
    if (len == 0)
        return 1;
    if (uaddr < (uintptr_t)PAGE_SIZE)
        return 0;
    if (uaddr >= 0x100000000ULL)
        return 0;
    end = uaddr + len;
    if (end < uaddr || end > 0x100000000ULL)
        return 0;
    return 1;
}

static int copy_from_user(void* kdst, const void* usrc, size_t n) {
    const uint8_t* s;
    uint8_t*       d;
    size_t         i;
    if (!user_range_ok((uintptr_t)usrc, n))
        return -1;
    s = (const uint8_t*)usrc;
    d = (uint8_t*)kdst;
    for (i = 0; i < n; i++)
        d[i] = s[i];
    return 0;
}

static int copy_to_user(void* udst, const void* ksrc, size_t n) {
    uint8_t*       d;
    const uint8_t* s;
    size_t         i;
    if (!user_range_ok((uintptr_t)udst, n))
        return -1;
    d = (uint8_t*)udst;
    s = (const uint8_t*)ksrc;
    for (i = 0; i < n; i++)
        d[i] = s[i];
    return 0;
}

long sys_send_msg(int dest_tid, const void* umsg, size_t size) {
    uint8_t kbuf[IPC_MSG_MAX];

    if (dest_tid < 0 || dest_tid >= sched_task_count)
        return -22;
    if (sched_tasks[dest_tid].state == TASK_DEAD)
        return -3; /* ESRCH-like */
    if (size == 0 || size > IPC_MSG_MAX)
        return -22;
    if (copy_from_user(kbuf, umsg, size) != 0)
        return -14;

    for (;;) {
        if (ipc_ring_push(dest_tid, kbuf, size))
            break;
        {
            int self = sched_current_tid();
            if (self < 0 || self >= SCHED_MAX_TASKS)
                return -11;
            ipc_send_block_dst[self] = dest_tid;
            sched_tasks[self].state = TASK_SLEEP;
            __asm__ volatile("sti; hlt");
            ipc_send_block_dst[self] = -1;
            if (sched_tasks[self].state == TASK_SLEEP)
                sched_tasks[self].state = TASK_RUNNING;
        }
    }

    if (sched_tasks[dest_tid].state == TASK_SLEEP)
        sched_tasks[dest_tid].state = TASK_READY;
    return (long)size;
}

long sys_recv_msg(void* umsg) {
    uint8_t  kbuf[IPC_MSG_MAX];
    uint16_t len = 0;
    int      my;

    if (!user_range_ok((uintptr_t)umsg, IPC_MSG_MAX))
        return -14;

    my = sched_current_tid();
    if (my < 0 || my >= SCHED_MAX_TASKS)
        return -22;

    for (;;) {
        if (ipc_ring_pop(my, kbuf, &len)) {
            if (copy_to_user(umsg, kbuf, (size_t)len) != 0)
                return -14;
            return (long)len;
        }
        sched_tasks[my].state = TASK_SLEEP;
        __asm__ volatile("sti; hlt");
        if (sched_tasks[my].state == TASK_SLEEP)
            sched_tasks[my].state = TASK_RUNNING;
    }
}
