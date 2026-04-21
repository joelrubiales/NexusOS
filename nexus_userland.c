#include "nexus_userland.h"
#include "syscalls.h"
#include "vfs.h"
#include "elf_loader.h"
#include "task.h"
#include "memory.h"
#include "compositor.h"
#include "gfx.h"
#include "desktop.h"
#include "gui_installer.h"
#include "event.h"
#include <stddef.h>

#define NX_ENOENT  -2
#define NX_EMFILE  -24
#define NX_EINVAL  -22

extern volatile int sched_enabled;

static int  g_spawn_slot = 1;
static int  g_wm_bg_done;

static const uint64_t SPAWN_MAP_BASE[] = {
    0x0400000ULL,
    0x2000000ULL,
    0x4000000ULL,
    0x6000000ULL,
};
static const uint64_t SPAWN_STACK_TOP[] = {
    0x80000000ULL,
    0x7F800000ULL,
    0x7F000000ULL,
    0x7E800000ULL,
};

void nexus_userland_graphics_prepare(void) {
    int sw = gfx_width();
    int sh = gfx_height();
    int st = gfx_backbuffer_stride_u32();
    if (sw < 1 || sh < 1 || st < sw)
        return;
    compositor_init(sw, sh, st);
    desktop_wm_init();
    compositor_bake_wallpaper_layer();
    compositor_mark_full();
}

long nexus_wm_dispatch(int cmd, uint64_t a1, uint64_t a2) {
    (void)a1;
    (void)a2;
    switch (cmd) {
    case 1:
        if (!g_wm_bg_done) {
            nexus_userland_graphics_prepare();
            g_wm_bg_done = 1;
        } else {
            compositor_bake_wallpaper_layer();
            compositor_mark_full();
        }
        return 0;
    case 2:
        installer_desktop_step();
        return 0;
    case 3:
        return 0;
    default:
        return NX_EINVAL;
    }
}

long nexus_spawn_elf_kpath(const char* kpath) {
    const uint8_t* data;
    uint32_t       sz;
    int            heap_owned = 0;
    ElfLoadResult  res;
    int            tid;
    int            slot;
    uint64_t       map_b, stack_t;

    if (!kpath)
        return NX_EINVAL;
    if (g_spawn_slot >= 4)
        return NX_EMFILE;

    data = vfs_find(kpath, &sz, &heap_owned);
    if (!data)
        return NX_ENOENT;

    slot  = g_spawn_slot++;
    map_b = SPAWN_MAP_BASE[slot];
    stack_t = SPAWN_STACK_TOP[slot];

    if (elf_load_binary_ex(data, sz, &res, map_b, stack_t) != ELF_LOAD_OK) {
        if (heap_owned)
            kfree((void*)data);
        return NX_EINVAL;
    }
    if (heap_owned)
        kfree((void*)data);

    tid = sched_new_user_task(res.entry, res.user_rsp, "spawn");
    if (tid < 0)
        return NX_EMFILE;
    nexus_pcb_init_for_tid(tid);
    return (long)tid;
}

int nexus_try_boot_userland(void) {
    const uint8_t* data;
    uint32_t       sz;
    int            heap_owned = 0;
    ElfLoadResult  res;
    int            tid;

    flush_events();

    data = vfs_find("/bin/init.elf", &sz, &heap_owned);
    if (!data)
        data = vfs_find("/init.elf", &sz, &heap_owned);
    if (!data)
        return -1;

    if (elf_load_binary_ex(data, sz, &res, 0x0400000ULL, 0x80000000ULL) != ELF_LOAD_OK) {
        if (heap_owned)
            kfree((void*)data);
        return -1;
    }
    if (heap_owned)
        kfree((void*)data);

    tid = sched_new_user_task(res.entry, res.user_rsp, "init");
    if (tid < 0)
        return -1;
    nexus_pcb_init_for_tid(tid);
    sched_enabled = 1;
    return 0;
}
