#include "shm.h"
#include "task.h"
#include "memory.h"
#include <stdint.h>

#define SHM_MAX_OBJECTS 16
#define SHM_MAX_TOTAL_PAGES 4096u /* ~16 MiB por segmento máx. razonable */
#define SHM_TASK_STRIDE 0x01000000u /* 16 MiB de VA reservados por slot de tarea */

/* Por encima del típico ET_DYN en 0x400000, por debajo de pila ~0x7FE00000 */
#define SHM_ARENA_BASE 0x058000000ULL

typedef struct {
    int         in_use;
    size_t      size;
    size_t      npages;
    uintptr_t*  phys;
} ShmObject;

static ShmObject shm_tab[SHM_MAX_OBJECTS];
static uint64_t  shm_next_va[SCHED_MAX_TASKS];

void shm_init(void) {
    int i;
    for (i = 0; i < SHM_MAX_OBJECTS; i++) {
        shm_tab[i].in_use = 0;
        shm_tab[i].size = 0;
        shm_tab[i].npages = 0;
        shm_tab[i].phys = NULL;
    }
    for (i = 0; i < SCHED_MAX_TASKS; i++)
        shm_next_va[i] = SHM_ARENA_BASE + (uint64_t)i * (uint64_t)SHM_TASK_STRIDE;
}

static int shm_region_end_va(int tid, uint64_t* out_end) {
    if (tid < 0 || tid >= SCHED_MAX_TASKS)
        return 0;
    *out_end = SHM_ARENA_BASE + (uint64_t)(tid + 1) * (uint64_t)SHM_TASK_STRIDE;
    return 1;
}

long sys_shm_create(size_t size) {
    size_t npages;
    uintptr_t* phys;
    int i, j;

    if (size == 0)
        return -22; /* EINVAL */
    npages = (size + (size_t)PAGE_SIZE - 1u) / (size_t)PAGE_SIZE;
    if (npages > SHM_MAX_TOTAL_PAGES)
        return -22;

    for (i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (!shm_tab[i].in_use)
            break;
    }
    if (i >= SHM_MAX_OBJECTS)
        return -24; /* EMFILE-ish */

    phys = (uintptr_t*)kmalloc((uint64_t)npages * sizeof(uintptr_t));
    if (!phys)
        return -12; /* ENOMEM */

    for (j = 0; j < (int)npages; j++) {
        uintptr_t p = pmm_alloc_page();
        if (!p) {
            while (--j >= 0)
                pmm_free_page(phys[j]);
            kfree(phys);
            return -12;
        }
        phys[j] = p;
    }

    shm_tab[i].in_use = 1;
    shm_tab[i].size = npages * (size_t)PAGE_SIZE;
    shm_tab[i].npages = npages;
    shm_tab[i].phys = phys;
    return (long)i;
}

long sys_shm_attach(int shm_id) {
    ShmObject* s;
    int tid;
    uint64_t va, region_end;
    size_t k;
    uint64_t need;

    if (shm_id < 0 || shm_id >= SHM_MAX_OBJECTS)
        return -22;
    s = &shm_tab[shm_id];
    if (!s->in_use || !s->phys || s->npages == 0)
        return -22;

    tid = sched_current_tid();
    if (tid < 0 || tid >= SCHED_MAX_TASKS)
        return -22;
    if (!shm_region_end_va(tid, &region_end))
        return -22;

    need = (uint64_t)s->npages * PAGE_SIZE;
    va = shm_next_va[tid];
    if (va + need < va || va + need > region_end)
        return -12;

    for (k = 0; k < s->npages; k++) {
        uint64_t v = va + (uint64_t)k * PAGE_SIZE;
        vmm_map_page(v, (uint64_t)s->phys[k],
                     VMM_PAGE_PRESENT | VMM_PAGE_RW | VMM_PAGE_USER | VMM_PAGE_NX);
    }

    shm_next_va[tid] = va + need;
    return (long)va;
}
