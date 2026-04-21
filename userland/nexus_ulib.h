#ifndef NEXUS_ULIB_H
#define NEXUS_ULIB_H

#include <stdint.h>

/* Números alineados con syscalls.h del kernel. */
#define NX_SYS_write  1
#define NX_SYS_wm     25
#define NX_SYS_spawn  32

static inline long nexus_syscall3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline long nexus_write(int fd, const void* buf, unsigned long len) {
    return nexus_syscall3(NX_SYS_write, (long)fd, (long)buf, (long)len);
}

static inline long nexus_wm(int cmd, unsigned long a1, unsigned long a2) {
    return nexus_syscall3(NX_SYS_wm, (long)cmd, (long)a1, (long)a2);
}

static inline long nexus_spawn(const char* path) {
    return nexus_syscall3(NX_SYS_spawn, (long)path, 0, 0);
}

#endif
