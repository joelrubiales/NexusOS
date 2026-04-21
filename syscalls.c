#include "syscalls.h"
#include "vfs.h"
#include "shm.h"
#include "ipc.h"
#include "nexus.h"
#include "memory.h"
#include "task.h"
#include "nexus_userland.h"
#include <stddef.h>
#include <stdint.h>

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_FMASK  0xC0000084u

#define EFER_SCE   (1u << 0)

/* STAR: SYSCALL carga CS=[47:32], SS=CS+8; SYSRET 64-bit: SS=[63:48]+8|3, CS=[63:48]+16|3 */
#define STAR_KERNEL_CS 0x18u
#define STAR_USER_BASE 0x20u

/* RFLAGS bits a limpiar en SYSCALL (IF, TF, DF, NT, RF, AC). */
#define SYSCALL_FMASK 0x47700u

#define NX_ENOENT  -2
#define NX_EBADF   -9
#define NX_EFAULT  -14
#define NX_EINVAL  -22
#define NX_EMFILE  -24

typedef enum {
    FD_NONE = 0,
    FD_STDIN,
    FD_STDOUT,
    FD_STDERR,
    FD_VFS_RO,
    FD_VFS_RO_HEAP,
} FdType;

typedef struct {
    FdType           type;
    const uint8_t*   data;
    uint32_t         size;
    uint32_t         pos;
} FdSlot;

#define PCB_MAX_FD 32

typedef struct {
    FdSlot fd[PCB_MAX_FD];
} ProcessControlBlock;

static ProcessControlBlock pcb_table[SCHED_MAX_TASKS];

extern void syscall_entry(void);

static ProcessControlBlock* pcb_cur(void) {
    int t = sched_current_tid();
    if (t < 0 || t >= SCHED_MAX_TASKS)
        return &pcb_table[0];
    return &pcb_table[t];
}

void nexus_pcb_init_for_tid(int tid) {
    int i;
    if (tid < 0 || tid >= SCHED_MAX_TASKS)
        return;
    for (i = 0; i < PCB_MAX_FD; i++) {
        pcb_table[tid].fd[i].type = FD_NONE;
        pcb_table[tid].fd[i].data = NULL;
        pcb_table[tid].fd[i].size = 0;
        pcb_table[tid].fd[i].pos = 0;
    }
    pcb_table[tid].fd[0].type = FD_STDIN;
    pcb_table[tid].fd[1].type = FD_STDOUT;
    pcb_table[tid].fd[2].type = FD_STDERR;
}

static void wrmsr(uint32_t msr, uint32_t lo, uint32_t hi) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static void rdmsr(uint32_t msr, uint32_t* lo, uint32_t* hi) {
    uint32_t a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(msr));
    *lo = a;
    *hi = d;
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

static long sys_read(int fd, void* ubuf, size_t count) {
    FdSlot* s;
    size_t  avail, n;
    if (fd < 0 || fd >= PCB_MAX_FD)
        return NX_EBADF;
    s = &pcb_cur()->fd[fd];
    if (s->type == FD_NONE)
        return NX_EBADF;
    if (s->type == FD_STDIN)
        return 0;
    if (s->type == FD_STDOUT || s->type == FD_STDERR)
        return NX_EINVAL;
    if (s->type != FD_VFS_RO && s->type != FD_VFS_RO_HEAP)
        return NX_EINVAL;
    if (!user_range_ok((uintptr_t)ubuf, count))
        return NX_EFAULT;
    if (count == 0)
        return 0;
    if (s->pos >= s->size)
        return 0;
    avail = (size_t)(s->size - s->pos);
    n = count < avail ? count : avail;
    if (copy_to_user(ubuf, s->data + s->pos, n) != 0)
        return NX_EFAULT;
    s->pos += (uint32_t)n;
    return (long)n;
}

static long sys_write(int fd, const void* ubuf, size_t count) {
    char     kchunk[128];
    size_t   i, chunk;
    char     attr;
    FdSlot*  s;

    if (fd < 0 || fd >= PCB_MAX_FD)
        return NX_EBADF;
    s = &pcb_cur()->fd[fd];
    if (s->type == FD_NONE)
        return NX_EBADF;
    if (s->type != FD_STDOUT && s->type != FD_STDERR)
        return NX_EINVAL;
    if (!user_range_ok((uintptr_t)ubuf, count))
        return NX_EFAULT;
    attr = (s->type == FD_STDERR) ? (char)0x0C : (char)0x07;

    for (i = 0; i < count; i += chunk) {
        chunk = count - i;
        if (chunk > sizeof(kchunk))
            chunk = sizeof(kchunk);
        if (copy_from_user(kchunk, (const uint8_t*)ubuf + i, chunk) != 0)
            return NX_EFAULT;
        {
            size_t j;
            for (j = 0; j < chunk; j++) {
                char t[2] = { kchunk[j], 0 };
                nexus_tty_cursor = kprint_color(t, nexus_tty_cursor, attr);
            }
        }
    }
    return (long)count;
}

static int find_free_fd(void) {
    int i;
    for (i = 3; i < PCB_MAX_FD; i++) {
        if (pcb_cur()->fd[i].type == FD_NONE)
            return i;
    }
    return -1;
}

static long sys_open(const char* upath, int flags, unsigned mode) {
    char              kpath[256];
    size_t            pi;
    int               fd;
    const uint8_t*    data;
    uint32_t          sz;
    int               heap_owned = 0;
    (void)mode;

    if (!user_range_ok((uintptr_t)upath, 1))
        return NX_EFAULT;

    for (pi = 0; pi < sizeof(kpath) - 1u; pi++) {
        uint8_t c;
        if (copy_from_user(&c, (const uint8_t*)upath + pi, 1) != 0)
            return NX_EFAULT;
        kpath[pi] = (char)c;
        if (c == 0)
            break;
    }
    kpath[sizeof(kpath) - 1u] = 0;

    if ((flags & (NX_O_RDONLY | NX_O_WRONLY | NX_O_RDWR)) == NX_O_WRONLY ||
        (flags & (NX_O_RDONLY | NX_O_WRONLY | NX_O_RDWR)) == (NX_O_RDONLY | NX_O_WRONLY))
        return NX_EINVAL;
    if (flags & (NX_O_CREAT | NX_O_TRUNC | NX_O_APPEND))
        return NX_EINVAL;

    data = vfs_find(kpath, &sz, &heap_owned);
    if (!data)
        return NX_ENOENT;

    fd = find_free_fd();
    if (fd < 0) {
        if (heap_owned)
            kfree((void*)data);
        return NX_EMFILE;
    }

    pcb_cur()->fd[fd].type = heap_owned ? FD_VFS_RO_HEAP : FD_VFS_RO;
    pcb_cur()->fd[fd].data = data;
    pcb_cur()->fd[fd].size = sz;
    pcb_cur()->fd[fd].pos = 0;
    return (long)fd;
}

static long sys_close(int fd) {
    if (fd < 0 || fd >= PCB_MAX_FD)
        return NX_EBADF;
    if (fd < 3)
        return NX_EINVAL;
    if (pcb_cur()->fd[fd].type == FD_NONE)
        return NX_EBADF;
    if (pcb_cur()->fd[fd].type == FD_VFS_RO_HEAP && pcb_cur()->fd[fd].data)
        kfree((void*)pcb_cur()->fd[fd].data);
    pcb_cur()->fd[fd].type = FD_NONE;
    pcb_cur()->fd[fd].data = NULL;
    pcb_cur()->fd[fd].size = 0;
    pcb_cur()->fd[fd].pos = 0;
    return 0;
}

long syscall_dispatch_c(uint64_t* frame) {
    uint64_t nr = frame[0];
    uint64_t a1 = frame[1];
    uint64_t a2 = frame[2];
    uint64_t a3 = frame[3];
    uint64_t a4 = frame[4];
    uint64_t a5 = frame[5];
    uint64_t a6 = frame[6];
    (void)a4;
    (void)a5;
    (void)a6;

    switch (nr) {
    case NX_SYS_read:
        return sys_read((int)a1, (void*)a2, (size_t)a3);
    case NX_SYS_write:
        return sys_write((int)a1, (const void*)a2, (size_t)a3);
    case NX_SYS_open:
        return sys_open((const char*)a1, (int)a2, (unsigned)a3);
    case NX_SYS_close:
        return sys_close((int)a1);
    case NX_SYS_shm_create:
        return sys_shm_create((size_t)a1);
    case NX_SYS_shm_attach:
        return sys_shm_attach((int)a1);
    case NX_SYS_send_msg:
        return sys_send_msg((int)a1, (const void*)a2, (size_t)a3);
    case NX_SYS_recv_msg:
        return sys_recv_msg((void*)a1);
    case NX_SYS_wm:
        return nexus_wm_dispatch((int)a1, a2, a3);
    case NX_SYS_spawn: {
        char kpath[256];
        size_t pi;
        if (!user_range_ok((uintptr_t)a1, 1))
            return NX_EFAULT;
        for (pi = 0; pi < sizeof(kpath) - 1u; pi++) {
            uint8_t c;
            if (copy_from_user(&c, (const uint8_t*)a1 + pi, 1) != 0)
                return NX_EFAULT;
            kpath[pi] = (char)c;
            if (c == 0)
                break;
        }
        kpath[sizeof(kpath) - 1u] = 0;
        return nexus_spawn_elf_kpath(kpath);
    }
    default:
        return NX_EINVAL;
    }
}

void syscalls_init(void) {
    int      i;
    uint32_t lo, hi;

    shm_init();
    ipc_init();

    for (i = 0; i < SCHED_MAX_TASKS; i++)
        nexus_pcb_init_for_tid(i);

    rdmsr(MSR_EFER, &lo, &hi);
    lo |= EFER_SCE;
    wrmsr(MSR_EFER, lo, hi);

    {
        uint64_t star = ((uint64_t)STAR_USER_BASE << 48) | ((uint64_t)STAR_KERNEL_CS << 32);
        wrmsr(MSR_STAR, (uint32_t)star, (uint32_t)(star >> 32));
    }

    {
        uintptr_t ent = (uintptr_t)syscall_entry;
        wrmsr(MSR_LSTAR, (uint32_t)ent, (uint32_t)((uint64_t)ent >> 32));
    }

    wrmsr(MSR_FMASK, SYSCALL_FMASK, 0);
}
