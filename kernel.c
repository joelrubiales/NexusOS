#include "nexus.h"
#include "memory.h"
#include "disk.h"
#include "multitasking.h"
#include "gfx.h"
#include "font8x8.h"
#include "teclado.h"
#include "pci.h"
#include "hda.h"
#include "xhci.h"
#include "nic.h"
#include "gui.h"
#include "mouse.h"
#include <stdint.h>
#include "vesa.h"
#include "boot_info.h"
#include "shell.h"
#include "gui_installer.h"
#include "nexus_userland.h"
#include "vfs.h"
#include "ext2.h"
#include "smp.h"
#include "syscalls.h"
#include "net.h"
#include <stddef.h>

/* 1 = sin startx; 0 = escritorio tras consola VESA. */
#define NEXUS_GUI_DISABLED 0

/* ═══════════════════════════════════════════════════════════════════════════
 *  NexusOS v3.0 Gaming Edition — x86-64 Long Mode kernel
 *  GRUB Multiboot2: kernel_main(magic, mb2_info_phys); tag 8 → BootInfo @ 0x5000.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MB2_INFO_MAGIC              0x36d76289u
#define MB2_TAG_END                 0u
#define MB2_TAG_CMDLINE             1u
#define MULTIBOOT_TAG_TYPE_CMDLINE  1u /* alias spec Multiboot2 */
#define MB2_TAG_MODULE              3u
#define MB2_TAG_FRAMEBUFFER         8u

static int boot_mode = 0;

static char nexus_cmdline[256];

/* strcmp mínimo (sin libc). */
static int nexus_strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Línea de comandos MB2 (tag 1); vacío si GRUB no la envía. */
static void mb2_parse_cmdline(uint32_t mbi_phys, char* buf, size_t cap) {
    const volatile uint32_t* m = (const volatile uint32_t*)(uintptr_t)mbi_phys;
    uint32_t total_size;
    size_t off;

    if (cap > 0)
        buf[0] = 0;
    if (cap <= 1 || mbi_phys == 0 || (mbi_phys & 7u) != 0)
        return;

    total_size = m[0];
    if (total_size < 16u || total_size > (1024u * 1024u))
        return;

    for (off = 8; off + 8 <= (size_t)total_size;) {
        uint16_t type = *(const volatile uint16_t*)(uintptr_t)(mbi_phys + off);
        uint32_t tag_size = *(const volatile uint32_t*)(uintptr_t)(mbi_phys + off + 4);
        size_t next;

        if (tag_size < 8u || off + (size_t)tag_size > (size_t)total_size)
            break;

        next = ((size_t)tag_size + 7u) & ~(size_t)7u;
        if (next == 0)
            break;

        if ((uint32_t)type == MULTIBOOT_TAG_TYPE_CMDLINE && tag_size > 8u) {
            const volatile char* s = (const volatile char*)(uintptr_t)(mbi_phys + off + 8);
            size_t maxc = (size_t)tag_size - 8u;
            size_t i;
            for (i = 0; i + 1 < cap && i < maxc && s[i]; i++)
                buf[i] = s[i];
            buf[i] = 0;
            return;
        }

        if ((uint32_t)type == MB2_TAG_END)
            break;

        off += next;
    }
}

static void mb2_hang(void) {
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

/*
 * Extrae el primer módulo GRUB (tag tipo 3 de la MBI).
 * mod_start / mod_end son direcciones físicas del payload.
 * Devuelve 1 si encontró al menos un módulo, 0 en caso contrario.
 */
static int mb2_parse_module(uint32_t mbi_phys, uint32_t* mod_start, uint32_t* mod_end) {
    const volatile uint32_t* m = (const volatile uint32_t*)(uintptr_t)mbi_phys;
    uint32_t total_size;
    size_t off;

    *mod_start = *mod_end = 0;
    if (mbi_phys == 0 || (mbi_phys & 7u) != 0) return 0;

    total_size = m[0];
    if (total_size < 16u || total_size > (4u * 1024u * 1024u)) return 0;

    for (off = 8; off + 8 <= (size_t)total_size;) {
        const volatile uint32_t* tag = (const volatile uint32_t*)(uintptr_t)(mbi_phys + off);
        uint32_t tag_type = tag[0];
        uint32_t tag_size = tag[1];
        size_t   next;

        if (tag_size < 8u) break;
        next = ((size_t)tag_size + 7u) & ~(size_t)7u;
        if (next == 0) break;

        /* Tag tipo 3: uint32 mod_start, uint32 mod_end, char name[]. */
        if (tag_type == MB2_TAG_MODULE && tag_size >= 16u) {
            *mod_start = tag[2];
            *mod_end   = tag[3];
            return 1;
        }
        if (tag_type == MB2_TAG_END) break;
        off += next;
    }
    return 0;
}

/* Recorre tags (type, size) en la Multiboot2 Information Structure; tag 8 = framebuffer. */
static int mb2_parse_framebuffer(uint32_t mbi_phys, BootInfo* bi) {
    const volatile uint32_t* m = (const volatile uint32_t*)(uintptr_t)mbi_phys;
    uint32_t total_size;
    size_t off;

    bi->magic = 0;
    bi->width = bi->height = bi->pitch = bi->bpp = 0;
    bi->lfb_ptr = 0;

    if (mbi_phys == 0 || (mbi_phys & 7u) != 0)
        return 0;

    total_size = m[0];
    if (total_size < 16u || total_size > (1024u * 1024u))
        return 0;

    for (off = 8; off + 8 <= (size_t)total_size;) {
        const volatile uint32_t* tag = (const volatile uint32_t*)(uintptr_t)(mbi_phys + off);
        uint32_t tag_type = tag[0];
        uint32_t tag_size = tag[1];
        size_t next;

        if (tag_size < 8u || off + (size_t)tag_size > (size_t)total_size)
            break;

        next = ((size_t)tag_size + 7u) & ~(size_t)7u;
        if (next == 0)
            break;

        if (tag_type == MB2_TAG_FRAMEBUFFER && tag_size >= 32u) {
            const volatile uint32_t* u = (const volatile uint32_t*)(uintptr_t)(mbi_phys + off);
            uint64_t framebuffer_addr = (uint64_t)u[2] | ((uint64_t)u[3] << 32);
            uint32_t framebuffer_pitch = u[4];
            uint32_t framebuffer_width = u[5];
            uint32_t framebuffer_height = u[6];
            uint8_t bpp = *(const volatile uint8_t*)(uintptr_t)(mbi_phys + off + 28);

            if (framebuffer_addr != 0ull && framebuffer_pitch >= 4u && framebuffer_width > 0u &&
                framebuffer_height > 0u && bpp == 32u) {
                bi->magic = BOOT_INFO_MAGIC;
                bi->width = framebuffer_width;
                bi->height = framebuffer_height;
                bi->pitch = framebuffer_pitch;
                bi->bpp = 32u;
                bi->lfb_ptr = framebuffer_addr;
                return 1;
            }
        }

        if (tag_type == MB2_TAG_END)
            break;

        off += next;
    }
    return 0;
}

/* ─── RAMFS ──────────────────────────────────────────────────────────────── */
typedef struct { char nombre[32]; int es_dir; char cont[512]; int activo; } Nodo;
static Nodo   fs[128];
static int    fs_n = 0;

static int fs_find(const char* n) {
    for(int i=0;i<fs_n;i++) if(fs[i].activo && comparar_cadenas(fs[i].nombre,(char*)n)) return i;
    return -1;
}

/* ─── History ────────────────────────────────────────────────────────────── */
#define HIST_SZ 32
static char hist[HIST_SZ][256];
static int  hist_cnt=0, hist_nav=-1;

static void hist_add(const char* c) {
    if(!c[0]) return;
    if(hist_cnt>0 && comparar_cadenas(hist[(hist_cnt-1)%HIST_SZ],(char*)c)) return;
    copiar_texto(hist[hist_cnt%HIST_SZ],(char*)c);
    hist_cnt++; hist_nav=-1;
}

/* ─── CWD ────────────────────────────────────────────────────────────────── */
static char cwd[64] = "~";

/* ─── Aliases ────────────────────────────────────────────────────────────── */
#define MAX_ALIAS 16
static char alias_name[MAX_ALIAS][32];
static char alias_val[MAX_ALIAS][128];
static int  alias_cnt = 0;

/* ─── Env vars ───────────────────────────────────────────────────────────── */
#define MAX_EXPORT 16
static char env_key[MAX_EXPORT][32];
static char env_val[MAX_EXPORT][128];
static int  env_cnt = 0;

/* ─── Helpers ────────────────────────────────────────────────────────────── */
static int  klen(const char* s) { int i=0; while(s[i])i++; return i; }
static void hex8(unsigned char v,char* o) {
    const char h[]="0123456789abcdef"; o[0]=h[v>>4]; o[1]=h[v&0xF]; o[2]=0;
}
static void hex16(unsigned short v,char* o) {
    hex8((unsigned char)(v>>8),o); hex8((unsigned char)(v&0xFF),o+2); o[4]=0;
}
static void num_str(int n,char* b) {
    if(n<=0){b[0]='0';b[1]=0;return;}
    char t[12]; int i=0;
    while(n){t[i++]='0'+(n%10);n/=10;}
    int j=0; for(int k=i-1;k>=0;k--) b[j++]=t[k]; b[j]=0;
}

static void cpu_brand(char* out) {
    unsigned int r[12], mx;
    __asm__ volatile("cpuid":"=a"(mx):"a"(0x80000000u):"ebx","ecx","edx");
    if(mx >= 0x80000004u) {
        __asm__ volatile("cpuid":"=a"(r[0]),"=b"(r[1]),"=c"(r[2]),"=d"(r[3]):"a"(0x80000002u));
        __asm__ volatile("cpuid":"=a"(r[4]),"=b"(r[5]),"=c"(r[6]),"=d"(r[7]):"a"(0x80000003u));
        __asm__ volatile("cpuid":"=a"(r[8]),"=b"(r[9]),"=c"(r[10]),"=d"(r[11]):"a"(0x80000004u));
        char* s=(char*)r; int i; for(i=0;i<47;i++) out[i]=s[i]; out[47]=0;
        int b=0; while(out[b]==' ')b++;
        if(b){int j=0;while(out[b+j]){out[j]=out[b+j];j++;}out[j]=0;}
    } else copiar_texto(out,"Unknown");
}

static void do_reboot(void) {
    __asm__ volatile("cli");
    outb(0x64,0xFE);
    for(volatile int i=0;i<500000;i++);
    struct{unsigned short l;unsigned long long b;}__attribute__((packed)) nid={0,0};
    __asm__ volatile("lidt %0\nint $3\n"::"m"(nid));
}
static void do_halt(void) { __asm__ volatile("cli;hlt"); }

static void parse_args(const char* s,char* a1,char* a2) {
    a1[0]=a2[0]=0;
    int i=0,j; while(s[i]&&s[i]!=' ')i++; while(s[i]==' ')i++;
    j=0; while(s[i]&&s[i]!=' '&&j<31) a1[j++]=s[i++]; a1[j]=0;
    while(s[i]==' ')i++;
    j=0; while(s[i]&&j<31) a2[j++]=s[i++]; a2[j]=0;
}

/* ─── Scroll ─────────────────────────────────────────────────────────────── */
static int scroll_if_needed(int cur) {
    volatile char* V=get_text_ptr();
    int cols=get_text_cols();
    int rows=vesa_console_active ? VC_ROWS : 25;
    int stride=cols*2;
    int total=cols*rows*2;
    if(cur >= total) {
        for(int i=0;i<stride*(rows-1);i++) V[i]=V[i+stride];
        for(int i=stride*(rows-1);i<total;i+=2){V[i]=' ';V[i+1]=0x07;}
        cur=stride*(rows-1);
        if(vesa_console_active) vesa_console_flush();
    }
    return cur;
}

/* ─── Prompt ─────────────────────────────────────────────────────────────── */
static int prompt_cursor = 0;
static void cmd_redraw(int* cursor,char* cmd,int* cl,const char* nt) {
    volatile char* V=get_text_ptr();
    int c=prompt_cursor;
    for(int i=0;i<*cl;i++){V[c]=' ';c+=2;}
    int i=0; while(nt[i]&&i<255){cmd[i]=nt[i];i++;} cmd[i]=0; *cl=i;
    c=prompt_cursor;
    for(int k=0;k<*cl;k++){V[c]=cmd[k];V[c+1]=0x0F;c+=2;}
    *cursor=c;
    if(vesa_console_active) vesa_console_flush();
}

/* ─── Tab completion ─────────────────────────────────────────────────────── */
static const char* all_cmds[] = {
    "help","clear","ls","cd","pwd","mkdir","touch","cat","rm","cp","mv",
    "grep","wc","head","tail","sort","uniq","find","du","ln","chmod","chown",
    "echo","date","uname","whoami","hostname","uptime","dmesg","env","export",
    "set","alias","history","man","which","neofetch","lscpu","free","ps","top",
    "df","ip","ifconfig","ping","curl","wget","ss","lspci","lsusb","lsblk",
    "fdisk","mount","umount","lsmod","kill","reboot","halt","shutdown",
    "poweroff","id","groups","startx","htop","apt","systemctl","nano",
    "screenfetch",0
};

static int tab_complete(char* cmd, int cl) {
    if(cl == 0) return cl;
    int matches = 0;
    const char* match = 0;
    for(int i = 0; all_cmds[i]; i++) {
        int ok = 1;
        for(int j = 0; j < cl; j++) {
            if(!all_cmds[i][j] || all_cmds[i][j] != cmd[j]) { ok = 0; break; }
        }
        if(ok) { matches++; match = all_cmds[i]; }
    }
    if(matches == 1 && match) {
        int i = 0;
        while(match[i] && i < 254) { cmd[i] = match[i]; i++; }
        cmd[i] = ' '; cmd[i+1] = 0;
        return i + 1;
    }
    /* Also try file names */
    for(int i = 0; i < fs_n; i++) {
        if(!fs[i].activo) continue;
        int ok = 1;
        for(int j = 0; j < cl; j++) {
            if(!fs[i].nombre[j] || fs[i].nombre[j] != cmd[j]) { ok = 0; break; }
        }
        if(ok) { matches++; match = fs[i].nombre; }
    }
    if(matches == 1 && match) {
        int i = 0;
        while(match[i] && i < 254) { cmd[i] = match[i]; i++; }
        cmd[i] = 0;
        return i;
    }
    return cl;
}

/* ─── dmesg ring buffer ──────────────────────────────────────────────────── */
#define DMESG_SZ 2048
static char dmesg_buf[DMESG_SZ];
static int  dmesg_pos = 0;

static void dmesg_log(const char* msg) {
    for(int i=0;msg[i]&&dmesg_pos<DMESG_SZ-2;i++) dmesg_buf[dmesg_pos++]=msg[i];
    if(dmesg_pos<DMESG_SZ-1) dmesg_buf[dmesg_pos++]='\n';
    dmesg_buf[dmesg_pos]=0;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
void kernel_main(uint32_t magic, uint32_t mb2_info_addr) {
    extern char __bss_start, __bss_end;
    BootInfo* info = (BootInfo*)(uintptr_t)BOOT_INFO_ADDR;

    for (char* p = &__bss_start; p < &__bss_end; p++) *p = 0;

    if (magic != MB2_INFO_MAGIC)
        mb2_hang();

    if (!mb2_parse_framebuffer(mb2_info_addr, info))
        mb2_hang();

    mb2_parse_cmdline(mb2_info_addr, nexus_cmdline, sizeof nexus_cmdline);

    boot_mode = 0;
    {
        const char* p;
        for (p = nexus_cmdline; *p; p++) {
            if (*p == 'm' && nexus_strcmp(p, "mode=cli") == 0) {
                boot_mode = 1;
                break;
            }
        }
    }

    if (info->magic != BOOT_INFO_MAGIC || info->lfb_ptr == 0ull || info->width == 0u ||
        info->height == 0u || info->pitch < 4u || info->bpp != 32u)
        mb2_hang();

    memory_init();

    /* Tags de GRUB: no asignar esas páginas con kmalloc / PMM. */
    pmm_reserve_multiboot_info(mb2_info_addr);

    /* Módulo initrd (tag 3): reservar en el PMM y montar el VFS. */
    {
        uint32_t mod_start = 0, mod_end = 0;
        if (mb2_parse_module(mb2_info_addr, &mod_start, &mod_end) && mod_end > mod_start) {
            uint32_t mod_sz = mod_end - mod_start;
            pmm_reserve_phys_range((uintptr_t)mod_start, (size_t)mod_sz);
            vfs_init(mod_start, mod_end);
        } else {
            vfs_init(0, 0);  /* Sin initrd: VFS vacío, los fallbacks dibujan de forma nativa. */
        }
    }

    /* Inmediatamente tras el handoff gráfico: PML4 + tablas 4 KiB, identidad sobre toda la VRAM. */
    memory_map_framebuffer_identity(info->lfb_ptr, info->pitch, info->height,
                                    VMM_PAGE_PRESENT | VMM_PAGE_RW);

    {
        uint64_t vram_size = (uint64_t)info->pitch * (uint64_t)info->height;
        if (vram_size > 0ull && vram_size <= (uint64_t)((size_t)-1))
            pmm_reserve_phys_range((uintptr_t)info->lfb_ptr, (size_t)vram_size);
    }

    /* GUI: pantalla azul de prueba; rescue/CLI: negro hasta start_shell(). */
    {
        volatile uint32_t* vram = (volatile uint32_t*)(uintptr_t)info->lfb_ptr;
        uint32_t fill = boot_mode ? RGB(0, 0, 0) : RGB(0, 0, 255);
        uint32_t y, x;
        uint32_t line_u32 = info->pitch / 4u;

        for (y = 0; y < info->height; y++) {
            volatile uint32_t* row = vram + (size_t)y * (size_t)line_u32;
            for (x = 0; x < info->width; x++)
                row[x] = fill;
        }
    }

    /* Consola sobre LFB (visible en modo gráfico; Linux hace fbcon similar). */
    vesa_console_init();

    /* ── Initial RAMFS ───────────────────────────────────────────────────── */
    copiar_texto(fs[0].nombre,"readme.txt");
    copiar_texto(fs[0].cont,"NexusOS v3.0 Gaming Edition — Live desktop\nBare-metal x86_64 OS. Shell: type 'help' after exiting GUI (ESC).");
    fs[0].es_dir=0; fs[0].activo=1;
    copiar_texto(fs[1].nombre,"docs"); fs[1].es_dir=1; fs[1].activo=1;
    copiar_texto(fs[2].nombre,"nexus.cfg");
    copiar_texto(fs[2].cont,"hostname=nexusos\nuser=root\nshell=/bin/nexusbash\narch=x86_64\nedition=gaming");
    fs[2].es_dir=0; fs[2].activo=1;
    copiar_texto(fs[3].nombre,"etc"); fs[3].es_dir=1; fs[3].activo=1;
    copiar_texto(fs[4].nombre,"bin"); fs[4].es_dir=1; fs[4].activo=1;
    copiar_texto(fs[5].nombre,"tmp"); fs[5].es_dir=1; fs[5].activo=1;
    copiar_texto(fs[6].nombre,"var"); fs[6].es_dir=1; fs[6].activo=1;
    copiar_texto(fs[7].nombre,"home"); fs[7].es_dir=1; fs[7].activo=1;
    copiar_texto(fs[8].nombre,"proc"); fs[8].es_dir=1; fs[8].activo=1;
    copiar_texto(fs[9].nombre,"dev"); fs[9].es_dir=1; fs[9].activo=1;
    copiar_texto(fs[10].nombre,"usr"); fs[10].es_dir=1; fs[10].activo=1;
    fs_n=11;

    /* ── Network: lazy detect ────────────────────────────────────────────── */
    static NicInfo nic;
    static int     nic_ok = -1;
    static int     vbox   = 0;

    /* ── dmesg ───────────────────────────────────────────────────────────── */
    dmesg_log("[    0.000000] NexusOS 3.0.0-nexus (x86_64) Gaming Edition");
    {
        char line[320];
        int p = 0;
        const char* pre = "[    0.000001] Command line: ";
        int i = 0;
        while (pre[i] && p < (int)sizeof(line) - 2)
            line[p++] = pre[i++];
        for (i = 0; nexus_cmdline[i] && p < (int)sizeof(line) - 2; i++)
            line[p++] = nexus_cmdline[i];
        line[p] = 0;
        dmesg_log(line);
    }
    dmesg_log("[    0.001000] CPU: x86_64 Long Mode (SMP tras IDT)");
    dmesg_log("[    0.002000] Memory: 512MB RAM detected");
    dmesg_log("[    0.003000] IDT: 256 entries installed (16-byte x64 gates)");
    dmesg_log("[    0.004000] PIT: channel 0 configured @ 1000 Hz (~1 ms/tick)");
    dmesg_log("[    0.005000] PIC: remapped IRQ0-7 to INT32-39");
    dmesg_log("[    0.006000] Keyboard: PS/2 ISO-ES layout with AltGr");
    dmesg_log("[    0.007000] RAMFS: 128 inodes, 512B/file, mounted on /");
    dmesg_log("[    0.008000] FB: Multiboot2 tag 8 → BootInfo @ 0x5000 (GRUB)");
    dmesg_log("[    0.009000] PS/2 Mouse: IRQ12 enabled");

    instalar_idt();
    syscalls_init();
    {
        XhciInfo xhci = {0};
        if (xhci_init(mb2_info_addr, &xhci) == 0 && xhci.present) {
            if (xhci_usb_mouse_active)
                dmesg_log("[    0.009150] xHCI: USB HID boot mouse active (polling)");
            else
                dmesg_log("[    0.009150] xHCI: controller OK, no HID mouse (use PS/2)");
        } else
            dmesg_log("[    0.009150] xHCI: absent or init failed (PS/2 mouse)");
    }
    {
        int ha = hda_init();
        if (ha == 0)
            dmesg_log("[    0.009175] HDA: Intel HD Audio init (MMIO, CORB/RIRB, PCM skeleton)");
        else
            dmesg_log("[    0.009175] HDA: no controller or init failed (optional)");
    }
    /* MADT + SIPI: requiere IDT cargada (APs hacen sti;hlt). */
    smp_init(mb2_info_addr);
    multitasking_init();
    net_init();
    if (net_ready())
        dmesg_log("[    0.009250] net: E1000 link up, ARP for 10.0.2.15 (slirp)");
    else
        dmesg_log("[    0.009250] net: E1000 not present or init failed");
    if (disk_init() == 0) {
        dmesg_log("[    0.009500] ATA: primary master PIO (ports 0x1F0-0x1F7)");
        if (ext2_mount(0) == 0)
            dmesg_log("[    0.009520] EXT2: read-only volume (superblock @ 1024, magic 0xEF53)");
        else
            dmesg_log("[    0.009520] EXT2: not present or invalid (optional; initrd/TAR still used)");
    } else
        dmesg_log("[    0.009500] ATA: init timeout (VM sin disco IDE)");
    dmesg_log("[    0.009800] MM: PMM bitmap + kmalloc heap 32 MiB @ 32 MiB");
    dmesg_log("[    0.010000] Interrupts enabled");

    /* Live OS: sin instalador ni pantallas intermedias */
    static char login_user[32];
    static char login_host[32];
    copiar_texto(login_user, "user");
    copiar_texto(login_host, "nexusos");

    const char* user_locale = "en_US.UTF-8";
    const char* user_tz     = "UTC+0";
    const char* user_kb     = "us";

    /* Home directory: /home/<user> */
    static char home_dir[64];
    home_dir[0]='/'; home_dir[1]='h'; home_dir[2]='o'; home_dir[3]='m'; home_dir[4]='e'; home_dir[5]='/';
    { int i=0; while(login_user[i]&&i<55){home_dir[6+i]=login_user[i];i++;} home_dir[6+i]=0; }

    /* ── Default env vars ────────────────────────────────────────────────── */
    copiar_texto(env_key[0],"PATH");     copiar_texto(env_val[0],"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    copiar_texto(env_key[1],"USER");     copiar_texto(env_val[1],login_user);
    copiar_texto(env_key[2],"HOME");     copiar_texto(env_val[2],home_dir);
    copiar_texto(env_key[3],"SHELL");    copiar_texto(env_val[3],"/bin/nexusbash");
    copiar_texto(env_key[4],"TERM");     copiar_texto(env_val[4],"linux");
    copiar_texto(env_key[5],"LANG");     copiar_texto(env_val[5],(char*)user_locale);
    copiar_texto(env_key[6],"DISPLAY");  copiar_texto(env_val[6],":0");
    copiar_texto(env_key[7],"HOSTNAME"); copiar_texto(env_val[7],login_host);
    copiar_texto(env_key[8],"TZ");       copiar_texto(env_val[8],(char*)user_tz);
    copiar_texto(env_key[9],"XKB_LAYOUT"); copiar_texto(env_val[9],(char*)user_kb);
    copiar_texto(env_key[10],"EDITOR");  copiar_texto(env_val[10],"nano");
    copiar_texto(env_key[11],"LOGNAME"); copiar_texto(env_val[11],login_user);
    env_cnt=12;

    /* ── nexus.cfg (valores por defecto Live OS) ─────────────────────── */
    {
        char cb[512]; int p = 0;
        const char* kv[] = {
            "hostname=", login_host, "\n",
            "user=", login_user, "\n",
            "lang=", (char*)user_locale, "\n",
            "timezone=", (char*)user_tz, "\n",
            "keyboard=", (char*)user_kb, "\n",
            "shell=/bin/nexusbash\n",
            "arch=x86_64\n",
            "edition=gaming\n",
            0
        };
        for(int k=0; kv[k]; k++)
            for(int i=0; kv[k][i] && p<510; i++) cb[p++]=kv[k][i];
        cb[p]=0;
        copiar_texto(fs[2].cont, cb);
    }

    /* Create home directory in RAMFS */
    if(fs_n < 127) {
        copiar_texto(fs[fs_n].nombre, login_user);
        fs[fs_n].es_dir = 1; fs[fs_n].activo = 1; fs_n++;
    }

    /* Bifurcación principal: subsistema gráfico vs solo consola (Multiboot2 cmdline). */
    if (boot_mode == 0) {
        init_desktop();
        if (nexus_try_boot_userland() == 0) {
            __asm__ volatile("sti");
            for (;;)
                __asm__ volatile("hlt");
        }
        start_gui_installer();
    } else {
        start_shell();
    }
    limpiar_pantalla();

    /* ── Linux-style login banner ────────────────────────────────────────── */
    int cursor = 0;
    cursor=kprint_color("\n",cursor,0x0F);
    cursor=kprint_color("NexusOS 3.0.0-nexus (tty1)\n",cursor,0x07);
    cursor=kprint_color("\n",cursor,0x07);
    cursor=kprint_color(login_host,cursor,0x0F);
    cursor=kprint_color(" login: ",cursor,0x0F);
    cursor=kprint_color(login_user,cursor,0x0A);
    cursor=kprint_color(" (automatic)\n",cursor,0x08);
    cursor=kprint_color("Last login: Mon Apr 13 00:00:00 on tty1\n",cursor,0x07);
    cursor=kprint_color("Welcome to NexusOS 3.0 Gaming Edition (x86_64)\n",cursor,0x0F);
    cursor=kprint_color("\n",cursor,0x07);
    cursor=kprint_color(" * System:    512 MB RAM, 6 CPUs\n",cursor,0x08);
    cursor=kprint_color(" * Shell:     /bin/nexusbash\n",cursor,0x08);
    cursor=kprint_color(" * GUI:       type 'startx' to return to desktop\n",cursor,0x08);
    cursor=kprint_color("\n",cursor,0x07);

    /* ── Prompt ──────────────────────────────────────────────────────────── */
    cursor=kprint_color(login_user,cursor,0x0A);
    cursor=kprint_color("@",cursor,0x0A);
    cursor=kprint_color(login_host,cursor,0x0A);
    cursor=kprint_color(":",cursor,0x07);
    cursor=kprint_color(cwd,cursor,0x09);
    cursor=kprint_color("$ ",cursor,0x07);
    prompt_cursor=cursor;

    char cmd[256]; int cl=0;
    KbdState kbd; kbd_init(&kbd);

    while(1) {
        if(!tecla_nueva){__asm__ volatile("hlt");continue;}
        unsigned char sc=tecla_nueva; tecla_nueva=0;
        KbdEvent ev=kbd_handle_scancode(&kbd,sc);

        if(ev.type==KBD_EV_UP) {
            if(!hist_cnt) continue;
            if(hist_nav<0) hist_nav=hist_cnt;
            if(hist_nav>0) hist_nav--;
            cmd_redraw(&cursor,cmd,&cl,hist[hist_nav%HIST_SZ]); continue;
        }
        if(ev.type==KBD_EV_DOWN) {
            if(hist_nav<0) continue;
            hist_nav++;
            if(hist_nav>=hist_cnt){hist_nav=-1;cmd_redraw(&cursor,cmd,&cl,"");}
            else cmd_redraw(&cursor,cmd,&cl,hist[hist_nav%HIST_SZ]);
            continue;
        }

        /* Tab completion */
        if(ev.type==KBD_EV_CHAR && ev.ch=='\t') {
            cmd[cl]=0;
            int newcl = tab_complete(cmd, cl);
            if(newcl != cl) {
                cl = newcl;
                int c = prompt_cursor;
                volatile char* V=get_text_ptr();
                for(int k=0;k<cl;k++){V[c]=cmd[k];V[c+1]=0x0F;c+=2;}
                cursor = c;
                if(vesa_console_active) vesa_console_flush();
            }
            continue;
        }

        if(ev.type==KBD_EV_ENTER) {
            cursor=kprint_color("\n",cursor,0x0F);
            cursor=scroll_if_needed(cursor);
            cmd[cl]=0;
            if(cl>0) {
                hist_add(cmd);
                char a1[32],a2[32]; parse_args(cmd,a1,a2);

                for(int i=0;i<alias_cnt;i++) {
                    if(comparar_cadenas(cmd,alias_name[i])) {
                        copiar_texto(cmd,alias_val[i]);
                        parse_args(cmd,a1,a2);
                        break;
                    }
                }

/* ══════ STARTX ═══════════════════════════════════════════════════════════ */
#if !NEXUS_GUI_DISABLED
if(comparar_cadenas(cmd,"startx")) {
    cursor=kprint_color("Starting NexusOS Desktop Environment...\n",cursor,0x0A);
    retraso(200);
    gui_run();
    cursor=0;
    cursor=kprint_color("Desktop session ended.\n",cursor,0x07);
}
#else
if(comparar_cadenas(cmd,"startx")) {
    cursor=kprint_color("startx: GUI disabled in this kernel build (LFB handoff test).\n",cursor,0x0C);
}
#endif

/* ══════ HELP ═════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"help")) {
    cursor=kprint_color("NexusOS Commands:\n\n",cursor,0x0B);
    cursor=kprint_color(" Files     ",cursor,0x0A);
    cursor=kprint_color("ls cd pwd mkdir touch cat rm cp mv grep wc\n",cursor,0x07);
    cursor=kprint_color("           head tail sort uniq find du ln chmod chown\n",cursor,0x07);
    cursor=kprint_color(" System    ",cursor,0x0A);
    cursor=kprint_color("clear date uname whoami hostname uptime dmesg\n",cursor,0x07);
    cursor=kprint_color(" Info      ",cursor,0x0A);
    cursor=kprint_color("neofetch screenfetch lscpu free ps top htop env\n",cursor,0x07);
    cursor=kprint_color("           export set alias id groups df\n",cursor,0x07);
    cursor=kprint_color(" Network   ",cursor,0x0A);
    cursor=kprint_color("ip ifconfig ping curl wget ss\n",cursor,0x07);
    cursor=kprint_color(" Hardware  ",cursor,0x0A);
    cursor=kprint_color("lspci lsusb lsblk fdisk mount umount lsmod\n",cursor,0x07);
    cursor=kprint_color(" Power     ",cursor,0x0A);
    cursor=kprint_color("reboot halt shutdown poweroff\n",cursor,0x07);
    cursor=kprint_color(" Desktop   ",cursor,0x0A);
    cursor=kprint_color("startx\n",cursor,0x07);
    cursor=kprint_color(" Other     ",cursor,0x0A);
    cursor=kprint_color("echo man which history nano apt systemctl\n",cursor,0x07);
    cursor=kprint_color("\n Tab key for auto-completion.\n",cursor,0x08);
}

/* ══════ CLEAR ════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"clear")){limpiar_pantalla();cursor=0;}

/* ══════ CD ═══════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"cd")||comparar_cadenas(cmd,"cd /"))
    copiar_texto(cwd,"/");
else if(comparar_cadenas(cmd,"cd ~"))
    copiar_texto(cwd,"~");
else if(empieza_con(cmd,"cd ")) {
    char* d=cmd+3;
    if(comparar_cadenas(d,"..")) copiar_texto(cwd,"~");
    else if(comparar_cadenas(d,"~")||comparar_cadenas(d,"/")) copiar_texto(cwd,d);
    else {
        int id=fs_find(d);
        if(id>=0&&fs[id].es_dir){cwd[0]='/';int ci=1;for(int k=0;d[k]&&ci<62;k++)cwd[ci++]=d[k];cwd[ci]=0;}
        else cursor=kprint_color("bash: cd: no such file or directory\n",cursor,0x0C);
    }
}

/* ══════ PWD ══════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"pwd"))
    {cursor=kprint_color(cwd[0]=='~'?"/root":cwd,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);}

/* ══════ LS ═══════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"ls")||empieza_con(cmd,"ls ")) {
    int ext=empieza_con(cmd,"ls -l")||empieza_con(cmd,"ls -la")||empieza_con(cmd,"ls -al");
    int hay=0;
    if(ext){
        cursor=kprint_color("total ",cursor,0x07);
        char b[12]; num_str(fs_n,b);
        cursor=kprint_color(b,cursor,0x07);cursor=kprint_color("\n",cursor,0x07);
    }
    for(int i=0;i<fs_n;i++){
        if(!fs[i].activo) continue;
        hay=1;
        if(ext){
            cursor=kprint_color(fs[i].es_dir?"drwxr-xr-x  2 root root  4096  ":"-rw-r--r--  1 root root   512  ",cursor,0x07);
        }
        cursor=kprint_color(fs[i].nombre,cursor,fs[i].es_dir?0x09:0x0F);
        if(fs[i].es_dir) cursor=kprint_color("/",cursor,0x09);
        cursor=kprint_color(ext?"\n":"  ",cursor,0x0F);
        cursor=scroll_if_needed(cursor);
    }
    if(!ext && hay) cursor=kprint_color("\n",cursor,0x0F);
    if(!hay) cursor=kprint_color("(empty)\n",cursor,0x08);
}

/* ══════ MKDIR ════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"mkdir ")&&a1[0]) {
    if(fs_n<128){copiar_texto(fs[fs_n].nombre,a1);fs[fs_n].es_dir=1;fs[fs_n].activo=1;fs_n++;}
}

/* ══════ TOUCH ════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"touch ")&&a1[0]) {
    if(fs_n<128){copiar_texto(fs[fs_n].nombre,a1);fs[fs_n].cont[0]=0;fs[fs_n].es_dir=0;fs[fs_n].activo=1;fs_n++;}
}

/* ══════ CAT ══════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"cat ")) {
    int id=fs_find(a1);
    if(id<0) cursor=kprint_color("cat: No such file or directory\n",cursor,0x0C);
    else if(fs[id].es_dir) cursor=kprint_color("cat: Is a directory\n",cursor,0x0C);
    else{cursor=kprint_color(fs[id].cont,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);}
}

/* ══════ RM ═══════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"rm ")&&a1[0]) {
    int id=fs_find(a1);
    if(id<0) cursor=kprint_color("rm: cannot remove: No such file\n",cursor,0x0C);
    else fs[id].activo=0;
}

/* ══════ CP ═══════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"cp ")) {
    if(!a1[0]||!a2[0]) cursor=kprint_color("Usage: cp <src> <dst>\n",cursor,0x0C);
    else{int id=fs_find(a1);
    if(id<0) cursor=kprint_color("cp: cannot stat: No such file\n",cursor,0x0C);
    else if(fs_n<128){copiar_texto(fs[fs_n].nombre,a2);copiar_texto(fs[fs_n].cont,fs[id].cont);fs[fs_n].es_dir=fs[id].es_dir;fs[fs_n].activo=1;fs_n++;}}
}

/* ══════ MV ═══════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"mv ")) {
    if(!a1[0]||!a2[0]) cursor=kprint_color("Usage: mv <src> <dst>\n",cursor,0x0C);
    else{int id=fs_find(a1);if(id<0)cursor=kprint_color("mv: cannot stat: No such file\n",cursor,0x0C);else copiar_texto(fs[id].nombre,a2);}
}

/* ══════ LN ═══════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"ln ")) {
    if(!a1[0]||!a2[0]) cursor=kprint_color("Usage: ln <target> <link>\n",cursor,0x0C);
    else{int id=fs_find(a1);
    if(id<0) cursor=kprint_color("ln: failed to create link\n",cursor,0x0C);
    else if(fs_n<128){copiar_texto(fs[fs_n].nombre,a2);copiar_texto(fs[fs_n].cont,fs[id].cont);fs[fs_n].es_dir=0;fs[fs_n].activo=1;fs_n++;}}
}

/* ══════ CHMOD / CHOWN ════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"chmod ")||empieza_con(cmd,"chown ")) {
    if(!a1[0]||!a2[0]) cursor=kprint_color("Usage: chmod/chown <mode> <file>\n",cursor,0x0C);
    else{int id=fs_find(a2); if(id<0)cursor=kprint_color("No such file\n",cursor,0x0C);}
}

/* ══════ HEAD / TAIL ══════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"head ")||empieza_con(cmd,"tail ")) {
    int id=fs_find(a1);
    if(id<0) cursor=kprint_color("No such file\n",cursor,0x0C);
    else{
        char* ct=fs[id].cont; int lines=0;
        if(empieza_con(cmd,"head")){for(int i=0;ct[i]&&lines<10;i++){char t[2]={ct[i],0};cursor=kprint_color(t,cursor,0x0F);if(ct[i]=='\n')lines++;}}
        else{int len=klen(ct),start=len; int cnt=0; for(int i=len-1;i>=0&&cnt<10;i--){if(ct[i]=='\n')cnt++;if(cnt<10)start=i;}
        for(int i=start;i<len;i++){char t[2]={ct[i],0};cursor=kprint_color(t,cursor,0x0F);}}
        cursor=kprint_color("\n",cursor,0x0F);
    }
}

/* ══════ SORT / UNIQ ══════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"sort ")||empieza_con(cmd,"uniq ")) {
    int id=fs_find(a1);
    if(id<0) cursor=kprint_color("No such file\n",cursor,0x0C);
    else{cursor=kprint_color(fs[id].cont,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);}
}

/* ══════ FIND ═════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"find ")) {
    int found=0;
    for(int i=0;i<fs_n;i++){if(!fs[i].activo)continue;
        int pl=klen(a1),ok=0;
        for(int j=0;fs[i].nombre[j];j++){int m=1;for(int k=0;k<pl;k++){if(fs[i].nombre[j+k]!=a1[k]){m=0;break;}}if(m){ok=1;break;}}
        if(ok){cursor=kprint_color("./",cursor,0x08);cursor=kprint_color(fs[i].nombre,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);found=1;cursor=scroll_if_needed(cursor);}
    }
    if(!found) cursor=kprint_color("(no results)\n",cursor,0x08);
}

/* ══════ DU ═══════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"du")||empieza_con(cmd,"du ")) {
    for(int i=0;i<fs_n;i++){if(!fs[i].activo)continue;
        char buf[12]; num_str(klen(fs[i].cont),buf);
        cursor=kprint_color(buf,cursor,0x0F);cursor=kprint_color("\t./",cursor,0x08);
        cursor=kprint_color(fs[i].nombre,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);
        cursor=scroll_if_needed(cursor);
    }
}

/* ══════ GREP ═════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"grep ")) {
    if(!a1[0]||!a2[0]) cursor=kprint_color("Usage: grep <pattern> <file>\n",cursor,0x0C);
    else{int id=fs_find(a2);
    if(id<0) cursor=kprint_color("grep: No such file\n",cursor,0x0C);
    else{char* ct=fs[id].cont;int pl=klen(a1),found=0;
    for(int i=0;ct[i];i++){int ok=1;for(int j=0;j<pl;j++)if(ct[i+j]!=a1[j]){ok=0;break;}if(ok){found=1;break;}}
    if(found){cursor=kprint_color(ct,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);}
    else cursor=kprint_color("(no match)\n",cursor,0x08);}}
}

/* ══════ WC ═══════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"wc ")) {
    int id=fs_find(a1);
    if(id<0) cursor=kprint_color("wc: No such file\n",cursor,0x0C);
    else{char* ct=fs[id].cont;int ch=0,w=0,li=1,iw=0;
    for(int i=0;ct[i];i++){ch++;if(ct[i]=='\n'){li++;iw=0;}else if(ct[i]==' ')iw=0;else{if(!iw){w++;iw=1;}}}
    char b[12];
    cursor=kprint_color("  ",cursor,0x07);num_str(li,b);cursor=kprint_color(b,cursor,0x0F);
    cursor=kprint_color("  ",cursor,0x07);num_str(w,b);cursor=kprint_color(b,cursor,0x0F);
    cursor=kprint_color("  ",cursor,0x07);num_str(ch,b);cursor=kprint_color(b,cursor,0x0F);
    cursor=kprint_color(" ",cursor,0x07);cursor=kprint_color(a1,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);}
}

/* ══════ ECHO ═════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"echo ")) {
    char* t=cmd+5;
    if(t[0]=='$') {
        char vn[32]; int i=0; t++;
        while(t[i]&&t[i]!=' '&&i<31){vn[i]=t[i];i++;} vn[i]=0;
        for(int j=0;j<env_cnt;j++){if(comparar_cadenas(env_key[j],vn)){cursor=kprint_color(env_val[j],cursor,0x0F);break;}}
    } else cursor=kprint_color(t,cursor,0x0F);
    cursor=kprint_color("\n",cursor,0x0F);
}

/* ══════ DATE ═════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"date")) {
    char h[12]; obtener_hora(h);
    cursor=kprint_color("Mon Apr 13 ",cursor,0x0F);
    cursor=kprint_color(h,cursor,0x0F);cursor=kprint_color(" UTC 2026\n",cursor,0x0F);
}

/* ══════ UNAME ════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"uname"))
    cursor=kprint_color("NexusOS 3.0.0-nexus x86_64 GNU/NexusOS\n",cursor,0x0F);

/* ══════ WHOAMI ═══════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"whoami")) {cursor=kprint_color(login_user,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);}

/* ══════ ID ═══════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"id")) {
    cursor=kprint_color("uid=1000(",cursor,0x0F);cursor=kprint_color(login_user,cursor,0x0F);
    cursor=kprint_color(") gid=1000(",cursor,0x0F);cursor=kprint_color(login_user,cursor,0x0F);
    cursor=kprint_color(") groups=1000(",cursor,0x0F);cursor=kprint_color(login_user,cursor,0x0F);
    cursor=kprint_color("),27(sudo),29(audio),44(video)\n",cursor,0x0F);
}

/* ══════ GROUPS ═══════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"groups")) cursor=kprint_color("root wheel sudo audio video\n",cursor,0x0F);

/* ══════ HOSTNAME ═════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"hostname")) {cursor=kprint_color(login_host,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);}

/* ══════ UPTIME ═══════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"uptime")) {
    unsigned int sec=ticks/1000, min=sec/60, hr=min/60;
    sec%=60; min%=60;
    char b[12]; char h[12]; obtener_hora(h);
    cursor=kprint_color(" ",cursor,0x07);cursor=kprint_color(h,cursor,0x0F);
    cursor=kprint_color(" up ",cursor,0x07);
    num_str((int)hr,b);cursor=kprint_color(b,cursor,0x0F);cursor=kprint_color(":",cursor,0x07);
    num_str((int)min,b);if(min<10)cursor=kprint_color("0",cursor,0x0F);cursor=kprint_color(b,cursor,0x0F);
    cursor=kprint_color(",  1 user,  load average: 0.00, 0.00, 0.00\n",cursor,0x07);
}

/* ══════ DMESG ════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"dmesg")) {
    cursor=kprint_color(dmesg_buf,cursor,0x07);
    cursor=scroll_if_needed(cursor);
}

/* ══════ MEMINFO / ATA / YIELD DEMO (subsistema tipo Linux) ═══════════════ */
else if(comparar_cadenas(cmd,"meminfo")) {
    char b[16];
    unsigned pf = pmm_free_pages();
    unsigned pt = pmm_total_pages();
    cursor=kprint_color("PMM free pages: ",cursor,0x07);
    num_str((int)pf,b); cursor=kprint_color(b,cursor,0x0F);
    cursor=kprint_color(" / ",cursor,0x07);
    num_str((int)pt,b); cursor=kprint_color(b,cursor,0x0F);
    cursor=kprint_color(" (4 KiB pages, 128 MiB tracked)\n",cursor,0x07);
    cursor=scroll_if_needed(cursor);
}
else if(comparar_cadenas(cmd,"atatest")) {
    unsigned char secbuf[512];
    int r = ata_read_sector(0, secbuf);
    if(r != 0) cursor=kprint_color("ATA: read LBA0 failed (normal sin disco IDE)\n",cursor,0x0E);
    else {
        cursor=kprint_color("ATA: sector 0 read OK. ",cursor,0x0A);
        if(secbuf[510]==0x55 && secbuf[511]==0xAA) cursor=kprint_color("MBR signature 0x55AA\n",cursor,0x0A);
        else cursor=kprint_color("No MBR signature (RAM/virt)\n",cursor,0x0E);
    }
    cursor=scroll_if_needed(cursor);
}
else if(comparar_cadenas(cmd,"yielddemo")) {
    char b[16];
    cursor=kprint_color("Cooperative RR (20 yields)...\n",cursor,0x07);
    multitasking_selftest();
    cursor=kprint_color("count_a=",cursor,0x0F);
    num_str((int)multitasking_count_a(), b); cursor=kprint_color(b,cursor,0x0A);
    cursor=kprint_color(" count_b=",cursor,0x0F);
    num_str((int)multitasking_count_b(), b); cursor=kprint_color(b,cursor,0x0A);
    cursor=kprint_color("\n",cursor,0x07);
    cursor=scroll_if_needed(cursor);
}

/* ══════ ENV / SET ════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"env")||comparar_cadenas(cmd,"set")) {
    for(int i=0;i<env_cnt;i++){
        cursor=kprint_color(env_key[i],cursor,0x0F);cursor=kprint_color("=",cursor,0x07);
        cursor=kprint_color(env_val[i],cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);
        cursor=scroll_if_needed(cursor);
    }
}

/* ══════ EXPORT ═══════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"export ")) {
    char* t=cmd+7; int eq=-1;
    for(int i=0;t[i];i++) if(t[i]=='='){eq=i;break;}
    if(eq>0&&env_cnt<MAX_EXPORT){
        int i=0;for(;i<eq&&i<31;i++) env_key[env_cnt][i]=t[i]; env_key[env_cnt][i]=0;
        i=eq+1;int j=0;for(;t[i]&&j<127;i++,j++) env_val[env_cnt][j]=t[i]; env_val[env_cnt][j]=0;
        env_cnt++;
    } else cursor=kprint_color("Usage: export KEY=VALUE\n",cursor,0x0C);
}

/* ══════ ALIAS ════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"alias")) {
    for(int i=0;i<alias_cnt;i++){
        cursor=kprint_color("alias ",cursor,0x07);cursor=kprint_color(alias_name[i],cursor,0x0F);cursor=kprint_color("='",cursor,0x07);
        cursor=kprint_color(alias_val[i],cursor,0x0F);cursor=kprint_color("'\n",cursor,0x07);
    }
    if(!alias_cnt) cursor=kprint_color("(no aliases defined)\n",cursor,0x08);
}
else if(empieza_con(cmd,"alias ")) {
    char* t=cmd+6; int eq=-1;
    for(int i=0;t[i];i++) if(t[i]=='='){eq=i;break;}
    if(eq>0&&alias_cnt<MAX_ALIAS){
        int i=0;for(;i<eq&&i<31;i++) alias_name[alias_cnt][i]=t[i]; alias_name[alias_cnt][i]=0;
        i=eq+1;int j=0;for(;t[i]&&j<127;i++,j++) alias_val[alias_cnt][j]=t[i]; alias_val[alias_cnt][j]=0;
        alias_cnt++;
    }
}

/* ══════ HISTORY ══════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"history")) {
    int s=hist_cnt>20?hist_cnt-20:0;
    for(int i=s;i<hist_cnt;i++){
        char b[8]; num_str(i+1,b);
        cursor=kprint_color("  ",cursor,0x08); cursor=kprint_color(b,cursor,0x08);
        cursor=kprint_color("  ",cursor,0x07); cursor=kprint_color(hist[i%HIST_SZ],cursor,0x0F);
        cursor=kprint_color("\n",cursor,0x0F); cursor=scroll_if_needed(cursor);
    }
}

/* ══════ MAN ══════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"man ")) {
    char* t=a1;
    if(comparar_cadenas(t,"ls")) cursor=kprint_color("LS(1)\n\nNAME\n    ls - list directory contents\n\nSYNOPSIS\n    ls [-la]\n",cursor,0x0F);
    else if(comparar_cadenas(t,"cat")) cursor=kprint_color("CAT(1)\n\nNAME\n    cat - concatenate files to stdout\n\nSYNOPSIS\n    cat <file>\n",cursor,0x0F);
    else if(comparar_cadenas(t,"ip")) cursor=kprint_color("IP(8)\n\nNAME\n    ip - show network interfaces\n\nSYNOPSIS\n    ip a | ip addr\n",cursor,0x0F);
    else if(comparar_cadenas(t,"startx")) cursor=kprint_color("STARTX(1)\n\nNAME\n    startx - launch GUI desktop environment\n",cursor,0x0F);
    else{cursor=kprint_color("No manual entry for ",cursor,0x0C);cursor=kprint_color(t,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);}
}

/* ══════ NEOFETCH / SCREENFETCH ═══════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"neofetch")||comparar_cadenas(cmd,"screenfetch")) {
    char cpu[64]; cpu_brand(cpu);
    unsigned int sec=ticks/1000;
    char ub[12]; num_str((int)sec,ub);
    cursor=kprint_color("\n",cursor,0x0F);
    cursor=kprint_color("    \xDB\xDB\xDB    \xDB\xDB    ",cursor,0x09);cursor=kprint_color(login_user,cursor,0x0A);cursor=kprint_color("@",cursor,0x08);cursor=kprint_color(login_host,cursor,0x0A);cursor=kprint_color("\n",cursor,0x0A);
    cursor=kprint_color("    \xDB\xDB\xDB\xDB   \xDB\xDB    ",cursor,0x09);cursor=kprint_color("------------------\n",cursor,0x08);
    cursor=kprint_color("    \xDB\xDB \xDB\xDB  \xDB\xDB    ",cursor,0x0B);cursor=kprint_color("OS:       ",cursor,0x07);cursor=kprint_color("NexusOS 3.0 Gaming x86_64\n",cursor,0x0F);
    cursor=kprint_color("    \xDB\xDB  \xDB\xDB \xDB\xDB    ",cursor,0x0B);cursor=kprint_color("Kernel:   ",cursor,0x07);cursor=kprint_color("3.0.0-nexus\n",cursor,0x0F);
    cursor=kprint_color("    \xDB\xDB   \xDB\xDB\xDB\xDB    ",cursor,0x09);cursor=kprint_color("Uptime:   ",cursor,0x07);cursor=kprint_color(ub,cursor,0x0F);cursor=kprint_color(" secs\n",cursor,0x0F);
    cursor=kprint_color("    \xDB\xDB    \xDB\xDB\xDB    ",cursor,0x09);cursor=kprint_color("Shell:    ",cursor,0x07);cursor=kprint_color("/bin/nexusbash\n",cursor,0x0F);
    cursor=kprint_color("                   ",cursor,0x00);cursor=kprint_color("DE:       ",cursor,0x07);cursor=kprint_color("NexusDE (VESA dynamic)\n",cursor,0x0F);
    cursor=kprint_color("                   ",cursor,0x00);cursor=kprint_color("CPU:      ",cursor,0x07);cursor=kprint_color(cpu,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);
    cursor=kprint_color("                   ",cursor,0x00);cursor=kprint_color("Memory:   ",cursor,0x07);cursor=kprint_color("~1MB / 512MB\n",cursor,0x0F);
    cursor=kprint_color("\n",cursor,0x0F);
}

/* ══════ LSCPU ════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"lscpu")) {
    char cpu[64]; cpu_brand(cpu);
    cursor=kprint_color("Architecture:          x86_64\n",cursor,0x0F);
    cursor=kprint_color("CPU op-mode(s):        32-bit, 64-bit\n",cursor,0x0F);
    cursor=kprint_color("Byte Order:            Little Endian\n",cursor,0x0F);
    cursor=kprint_color("CPU(s):                6\n",cursor,0x0F);
    cursor=kprint_color("Model name:            ",cursor,0x0F);cursor=kprint_color(cpu,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);
    cursor=kprint_color("Virtualization:        VT-x\n",cursor,0x0F);
}

/* ══════ FREE ═════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"free")||comparar_cadenas(cmd,"free -h")) {
    cursor=kprint_color("               total       used       free     shared    available\n",cursor,0x0B);
    cursor=kprint_color("Mem:           512Mi       ~1Mi      511Mi       0Mi       511Mi\n",cursor,0x0F);
    cursor=kprint_color("Swap:            0Mi        0Mi        0Mi\n",cursor,0x0F);
}

/* ══════ PS ═══════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"ps")||comparar_cadenas(cmd,"ps aux")) {
    cursor=kprint_color("USER       PID %CPU %MEM    VSZ   RSS TTY   STAT  COMMAND\n",cursor,0x0B);
    cursor=kprint_color("root         1  0.0  0.1   1024   128 ?     Ss    /sbin/init\n",cursor,0x0F);
    cursor=kprint_color("root         2  0.0  0.0      0     0 ?     S     [kworker/0:0-timer]\n",cursor,0x0F);
    cursor=kprint_color("root         3  0.0  0.0      0     0 ?     S     [kworker/1:0-kbd]\n",cursor,0x0F);
    cursor=kprint_color("root         4  0.1  0.0   2048   256 tty1  Ss+   /bin/nexusbash\n",cursor,0x0F);
}

/* ══════ TOP / HTOP ═══════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"top")||comparar_cadenas(cmd,"htop")) {
    unsigned int sec=ticks/1000;
    char b[12]; num_str((int)sec,b);
    cursor=kprint_color("top - up ",cursor,0x0B);cursor=kprint_color(b,cursor,0x0F);cursor=kprint_color("s,  1 user,  load average: 0.00, 0.00, 0.00\n",cursor,0x07);
    cursor=kprint_color("Tasks:   4 total,   1 running,   3 sleeping\n",cursor,0x07);
    cursor=kprint_color("%Cpu(s):  0.1 us,  0.0 sy,  0.0 ni, 99.9 id\n",cursor,0x07);
    cursor=kprint_color("MiB Mem:  512.0 total,  511.0 free,    1.0 used\n",cursor,0x07);
    cursor=kprint_color("\n",cursor,0x07);
    cursor=kprint_color("  PID USER      PR  NI    VIRT    RES  %CPU  COMMAND\n",cursor,0x0B);
    cursor=kprint_color("    4 root      20   0    2048    256   0.1  nexusbash\n",cursor,0x0F);
    cursor=kprint_color("    1 root      20   0    1024    128   0.0  init\n",cursor,0x0F);
}

/* ══════ DF ═══════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"df")||comparar_cadenas(cmd,"df -h")) {
    cursor=kprint_color("Filesystem      Size  Used Avail Use% Mounted on\n",cursor,0x0B);
    cursor=kprint_color("ramfs            64K    8K   56K  13% /\n",cursor,0x0F);
    cursor=kprint_color("devfs              0     0     0   0% /dev\n",cursor,0x08);
    cursor=kprint_color("procfs             0     0     0   0% /proc\n",cursor,0x08);
}

/* ══════ IP A / IFCONFIG ══════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"ip a")||comparar_cadenas(cmd,"ip addr")||comparar_cadenas(cmd,"ifconfig")) {
    if(nic_ok==-1){
        nic=nic_detect(); nic_ok=nic.valid;
        vbox=pci_detect_virtualbox();
        dmesg_log(nic_ok?"[   10.000000] eth0: NIC detected":"[   10.000000] eth0: no NIC found");
    }
    cursor=kprint_color("1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 state UP\n",cursor,0x0F);
    cursor=kprint_color("    inet 127.0.0.1/8 scope host lo\n",cursor,0x07);
    if(nic_ok) {
        char mac[18]; nic_mac_str(&nic,mac);
        cursor=kprint_color("2: eth0: <BROADCAST,MULTICAST,UP> mtu 1500 (",cursor,0x0F);
        cursor=kprint_color(nic.name,cursor,0x0E);cursor=kprint_color(")\n",cursor,0x0F);
        cursor=kprint_color("    link/ether ",cursor,0x07);cursor=kprint_color(mac,cursor,0x0F);cursor=kprint_color("\n",cursor,0x07);
        if(vbox){
            cursor=kprint_color("    inet 10.0.2.15/24 brd 10.0.2.255 scope global eth0\n",cursor,0x0F);
            cursor=kprint_color("    gateway 10.0.2.2\n",cursor,0x08);
        } else cursor=kprint_color("    inet (DHCP pending)\n",cursor,0x08);
    } else cursor=kprint_color("2: eth0: <NO-CARRIER,BROADCAST,MULTICAST> state DOWN\n",cursor,0x08);
}

/* ══════ PING ═════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"ping ")) {
    cursor=kprint_color("PING ",cursor,0x0F);cursor=kprint_color(a1,cursor,0x0F);
    cursor=kprint_color(" (",cursor,0x07);cursor=kprint_color(a1,cursor,0x0F);cursor=kprint_color(") 56(84) bytes of data.\n",cursor,0x07);
    if (net_ready())
        cursor=kprint_color("ICMP not implemented (kernel has ARP + raw Ethernet only)\n",cursor,0x08);
    else
        cursor=kprint_color("Request timeout (no NIC stack)\n",cursor,0x08);
}

/* ══════ CURL / WGET ══════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"curl ")||empieza_con(cmd,"wget ")) {
    cursor=kprint_color("curl: (7) Failed to connect: TCP/IP stack not available\n",cursor,0x0C);
}

/* ══════ SS ═══════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"ss")||empieza_con(cmd,"ss ")) {
    cursor=kprint_color("Netid State  Recv-Q Send-Q  Local Address:Port  Peer Address:Port\n",cursor,0x0B);
    cursor=kprint_color("(no sockets - TCP/IP stack pending)\n",cursor,0x08);
}

/* ══════ LSPCI ════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"lspci")) {
    char bx[8], vx[8], dx[8];
    int found=0;
    for(int bus=0;bus<8;bus++){
        for(int slot=0;slot<32;slot++){
            unsigned int id=pci_read32((unsigned char)bus,(unsigned char)slot,0,0x00);
            if((id&0xFFFF)==0xFFFF) continue;
            unsigned short vid=(unsigned short)(id&0xFFFF);
            unsigned short did=(unsigned short)(id>>16);
            unsigned int cls=pci_read32((unsigned char)bus,(unsigned char)slot,0,0x08);
            unsigned char cc=(unsigned char)(cls>>24);
            found=1;
            hex8((unsigned char)bus,bx); hex16(vid,vx); hex16(did,dx);
            cursor=kprint_color(bx,cursor,0x08);cursor=kprint_color(":",cursor,0x08);
            hex8((unsigned char)slot,bx);cursor=kprint_color(bx,cursor,0x08);
            cursor=kprint_color(".0 ",cursor,0x08);
            const char* cn="Unknown";
            if(cc==0x01) cn="Mass storage controller"; else if(cc==0x02) cn="Network controller";
            else if(cc==0x03) cn="Display controller"; else if(cc==0x04) cn="Multimedia controller";
            else if(cc==0x06) cn="Bridge"; else if(cc==0x0C) cn="Serial bus controller";
            cursor=kprint_color((char*)cn,cursor,0x0F);
            cursor=kprint_color(" [",cursor,0x08);cursor=kprint_color(vx,cursor,0x0A);
            cursor=kprint_color(":",cursor,0x08);cursor=kprint_color(dx,cursor,0x0A);
            cursor=kprint_color("]\n",cursor,0x08);
            cursor=scroll_if_needed(cursor);
        }
    }
    if(!found) cursor=kprint_color("(no PCI devices found)\n",cursor,0x08);
}

/* ══════ LSUSB ════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"lsusb"))
    cursor=kprint_color("Bus 001 Device 001: ID 80ee:0021 VirtualBox USB Tablet\n",cursor,0x0F);

/* ══════ LSBLK ════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"lsblk")) {
    cursor=kprint_color("NAME   MAJ:MIN RM  SIZE RO TYPE MOUNTPOINT\n",cursor,0x0B);
    cursor=kprint_color("fd0      2:0    1  1.4M  0 disk /boot\n",cursor,0x0F);
    cursor=kprint_color("ram0     1:0    0   64K  0 disk /\n",cursor,0x0F);
}

/* ══════ FDISK ════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"fdisk")||empieza_con(cmd,"fdisk ")) {
    cursor=kprint_color("Disk /dev/fd0: 1.44 MiB, 1474560 bytes, 2880 sectors\n",cursor,0x0F);
    cursor=kprint_color("Units: sectors of 1 * 512 = 512 bytes\n",cursor,0x07);
    cursor=kprint_color("Sector size: 512 bytes\n",cursor,0x07);
}

/* ══════ MOUNT / UMOUNT ═══════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"mount")) {
    cursor=kprint_color("ramfs on / type ramfs (rw,relatime)\n",cursor,0x0F);
    cursor=kprint_color("devfs on /dev type devfs (rw,nosuid)\n",cursor,0x07);
    cursor=kprint_color("proc on /proc type proc (rw,nosuid,nodev,noexec)\n",cursor,0x07);
}
else if(empieza_con(cmd,"umount ")) cursor=kprint_color("umount: permission denied\n",cursor,0x0C);

/* ══════ LSMOD ════════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"lsmod")) {
    cursor=kprint_color("Module                  Size  Used by\n",cursor,0x0B);
    cursor=kprint_color("pit_timer              128    1\n",cursor,0x0F);
    cursor=kprint_color("ps2_keyboard           256    1\n",cursor,0x0F);
    cursor=kprint_color("vga_fb                 384    1\n",cursor,0x0F);
    cursor=kprint_color("pci_bus                384    1\n",cursor,0x0F);
    cursor=kprint_color("ramfs                  256    1\n",cursor,0x0F);
    if(nic_ok>0){cursor=kprint_color("e1000                  512    1\n",cursor,0x0F);}
}

/* ══════ APT ══════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"apt")) {
    cursor=kprint_color("apt: NexusOS uses nexuspkg (not yet implemented)\n",cursor,0x08);
    cursor=kprint_color("Try: nexuspkg install <package>\n",cursor,0x08);
}

/* ══════ SYSTEMCTL ════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"systemctl")) {
    cursor=kprint_color("  UNIT                    LOAD   ACTIVE SUB     DESCRIPTION\n",cursor,0x0B);
    cursor=kprint_color("  pit-timer.service       loaded active running PIT Timer IRQ0\n",cursor,0x0F);
    cursor=kprint_color("  ps2-kbd.service         loaded active running PS/2 Keyboard\n",cursor,0x0F);
    cursor=kprint_color("  vga-console.service     loaded active running VGA Console\n",cursor,0x0F);
    cursor=kprint_color("  ramfs.service           loaded active running RAM Filesystem\n",cursor,0x0F);
    cursor=kprint_color("\n4 loaded units listed.\n",cursor,0x08);
}

/* ══════ NANO ═════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"nano ")) {
    int id=fs_find(a1);
    int created = 0;
    if(id<0 && fs_n<128) {
        id=fs_n; copiar_texto(fs[id].nombre,a1);
        fs[id].cont[0]=0; fs[id].es_dir=0; fs[id].activo=1; fs_n++;
        created=1;
    }
    if(id>=0 && !fs[id].es_dir) {
        limpiar_pantalla();
        int nc=0;
        int ncols=get_text_cols();
        int nrows=vesa_console_active?VC_ROWS:25;
        int nstride=ncols*2;
        nc=kprint_color("  GNU nano 7.2                 ",nc,0x70);
        nc=kprint_color(a1,nc,0x70);
        {volatile char*V=get_text_ptr();for(int p=nc;p<nstride;p+=2){V[p]=' ';V[p+1]=0x70;}}
        nc=nstride;
        nc=kprint_color(fs[id].cont,nc,0x0F);
        nc=kprint_color("\n",nc,0x0F);
        {int sbar=(nrows-1)*nstride;
        volatile char*V=get_text_ptr();
        const char* hlp="^X Exit  ^O Save  (ESC to return)";
        for(int p=0;hlp[p];p++){V[sbar+p*2]=hlp[p];V[sbar+p*2+1]=0x70;}
        for(int p=klen(hlp)*2;p<nstride;p+=2){V[sbar+p]=' ';V[sbar+p+1]=0x70;}
        if(vesa_console_active) vesa_console_flush();}

        /* Wait for ESC */
        while(1) {
            if(!tecla_nueva){__asm__ volatile("hlt");continue;}
            unsigned char nsc=tecla_nueva; tecla_nueva=0;
            if(nsc==0x01) break;
        }
        limpiar_pantalla();
        cursor=0;
        if(created) {cursor=kprint_color("Created new file: ",cursor,0x08);cursor=kprint_color(a1,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);}
    }
}
else if(comparar_cadenas(cmd,"nano")) cursor=kprint_color("Usage: nano <filename>\n",cursor,0x0C);

/* ══════ KILL ═════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"kill "))
    cursor=kprint_color("kill: Operation not permitted\n",cursor,0x0C);

/* ══════ WHICH ════════════════════════════════════════════════════════════ */
else if(empieza_con(cmd,"which ")) {
    cursor=kprint_color("/usr/bin/",cursor,0x0F);cursor=kprint_color(a1,cursor,0x0F);cursor=kprint_color("\n",cursor,0x0F);
}

/* ══════ REBOOT ═══════════════════════════════════════════════════════════ */
else if(comparar_cadenas(cmd,"reboot")) {
    limpiar_pantalla(); kprint_color("Rebooting...\n",0,0x07);
    retraso(200); do_reboot();
}

/* ══════ HALT / SHUTDOWN / POWEROFF ═══════════════════════════════════════ */
else if(comparar_cadenas(cmd,"halt")||comparar_cadenas(cmd,"shutdown")||comparar_cadenas(cmd,"poweroff")) {
    limpiar_pantalla();
    kprint_color("System halted. It is safe to power off.\n",0,0x07);
    do_halt();
}

/* ══════ NOT FOUND ════════════════════════════════════════════════════════ */
else {
    cursor=kprint_color("bash: ",cursor,0x07);
    cursor=kprint_color(cmd,cursor,0x0F);
    cursor=kprint_color(": command not found\n",cursor,0x07);
}

            } /* if cl>0 */

            hist_nav=-1; cl=0;
            cursor=scroll_if_needed(cursor);
            cursor=kprint_color(login_user,cursor,0x0A);
            cursor=kprint_color("@",cursor,0x0A);
            cursor=kprint_color(login_host,cursor,0x0A);
            cursor=kprint_color(":",cursor,0x07);
            cursor=kprint_color(cwd,cursor,0x09);
            cursor=kprint_color("$ ",cursor,0x07);
            prompt_cursor=cursor;
        }

        else if(ev.type==KBD_EV_BACKSPACE && cl>0) {
            cl--; cursor-=2;
            volatile char* V=get_text_ptr();
            V[cursor]=' ';
            if(vesa_console_active) vesa_console_flush();
        }

        else if(ev.type==KBD_EV_CHAR && cl<255) {
            cmd[cl++]=(char)ev.ch;
            char t[2]={(char)ev.ch,0};
            cursor=kprint_color(t,cursor,0x0F);
        }
    }
}
