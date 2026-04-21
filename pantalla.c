#include "nexus.h"
#include "gfx.h"
#include "boot_info.h"
#include "font8x8.h"
#include "pci.h"
#include "smp.h"

int vesa_console_active = 0;
int nexus_tty_cursor = 0;

#define MAX_COLS 160
#define MAX_ROWS 120
#define MAX_TBUF (MAX_COLS * MAX_ROWS * 2)

static int dyn_cols = 100;
static int dyn_rows = 75;

int vc_get_cols(void) { return dyn_cols; }
int vc_get_rows(void) { return dyn_rows; }

#define TBUF_SZ (dyn_cols * dyn_rows * 2)

unsigned char vesa_text_buf[MAX_TBUF];
static unsigned char prev_buf[MAX_TBUF];

static const unsigned int vga16[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

/* ── VMSVGA driver ─────────────────────────────────────────────────── */
static unsigned short svga_io_base;
static volatile unsigned int *svga_fifo;
static volatile unsigned int *svga_fb;
static int svga_enabled = 0;

static void svga_wreg(unsigned int idx, unsigned int val) {
    outl(svga_io_base, idx);
    outl(svga_io_base + 1, val);
}
static unsigned int svga_rreg(unsigned int idx) {
    outl(svga_io_base, idx);
    return inl(svga_io_base + 1);
}

/* Ring buffer free space (VMware SVGA semantics; one slot reserved when next<stop). */
static unsigned int svga_fifo_free_bytes(void) {
    volatile unsigned int *f = svga_fifo;
    unsigned int min  = f[0];
    unsigned int max  = f[1];
    unsigned int next = f[2];
    unsigned int stop = f[3];
    if (next >= stop)
        return max - next + stop - min;
    return stop - next - 4u;
}

static void svga_fifo_wait_room(unsigned int need) {
    volatile unsigned int *f = svga_fifo;
    while (svga_fifo_free_bytes() < need)
        (void)f[3]; /* STOP — host advances when it consumes commands */
}

static void svga_fifo_write(unsigned int val) {
    volatile unsigned int *f = svga_fifo;
    unsigned int next = f[2];
    unsigned int max  = f[1];
    unsigned int min  = f[0];
    svga_fifo_wait_room(4u);
    ((volatile unsigned int*)((unsigned long long)f + next))[0] = val;
    next += 4;
    if (next >= max) next = min;
    f[2] = next;
}

/* SVGA_CMD_UPDATE is 5 dwords; must not wrap mid-command. Pad with NOP (0). */
static void svga_update(unsigned int x, unsigned int y, unsigned int w, unsigned int h) {
    volatile unsigned int *f = svga_fifo;
    unsigned int max = f[1];
    unsigned int next;
    if (!svga_enabled) return;
    for (;;) {
        next = f[2];
        if (max - next >= 20u || next == f[0]) break;
        svga_fifo_write(0); /* NOP until 5 dwords fit contiguously before max */
    }
    svga_fifo_write(1); /* SVGA_CMD_UPDATE */
    svga_fifo_write(x);
    svga_fifo_write(y);
    svga_fifo_write(w);
    svga_fifo_write(h);
}

/* One huge UPDATE can fail to refresh the whole guest FB in VirtualBox; use strips. */
#define SVGA_REFRESH_STRIP 64

static void svga_refresh_full(void) {
    unsigned int w = *(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 4);
    unsigned int h = *(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 8);
    unsigned int y;
    for (y = 0; y < h; y += SVGA_REFRESH_STRIP) {
        unsigned int hh = h - y;
        if (hh > SVGA_REFRESH_STRIP) hh = SVGA_REFRESH_STRIP;
        svga_update(0, y, w, hh);
    }
}

/* VBE ya fijó modo+LFB en el bootloader: enganchar FIFO para SVGA_CMD_UPDATE.
 * Hay que programar WIDTH/HEIGHT/BPP/PITCH igual que VBE *antes* de ENABLE; si no,
 * el chip queda en estado inválido y VirtualBox muestra todo negro.
 * No sobrescribimos NEXUS_BOOT_INFO_PHYS.. (fuente de verdad boot2 / SVGA). */
static int svga_attach_fifo_for_vbe(void) {
    PciDevice dev = pci_find(0x15AD, 0x0405);
    if (!dev.valid) return 0;

    unsigned int bar0 = pci_read32(dev.bus, dev.slot, 0, 0x10);
    svga_io_base = (unsigned short)(bar0 & ~3u);

    unsigned int bar1 = pci_read32(dev.bus, dev.slot, 0, 0x14);
    unsigned int fb_base = bar1 & ~0xFu;

    unsigned int bar2 = pci_read32(dev.bus, dev.slot, 0, 0x18);
    unsigned int fifo_base = bar2 & ~0xFu;

    uint32_t vbe_lo = *(volatile uint32_t*)(uintptr_t)(NEXUS_BOOT_INFO_PHYS + 20);
    uint32_t vbe_hi = *(volatile uint32_t*)(uintptr_t)(NEXUS_BOOT_INFO_PHYS + 24);
    uint64_t vbe_lfb = (uint64_t)vbe_lo | ((uint64_t)vbe_hi << 32);
    if (((uint64_t)(fb_base & ~0xFu)) != (vbe_lfb & ~0xFULL))
        return 0;

    svga_fb   = (volatile unsigned int *)(unsigned long long)fb_base;
    svga_fifo = (volatile unsigned int *)(unsigned long long)fifo_base;

    svga_wreg(0, 0x90000002);
    if ((svga_rreg(0) & 0xFF) < 2) return 0;

    {
        unsigned int w = *(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 4);
        unsigned int h = *(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 8);
        unsigned int bpp = *(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 16);
        unsigned int pitch = *(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 12);
        if (w == 0 || h == 0 || bpp == 0 || pitch == 0) return 0;
        /* Mismos índices que svga_init: 2=W 3=H 7=BPP 12=bytes por línea */
        svga_wreg(2, w);
        svga_wreg(3, h);
        svga_wreg(7, bpp);
        svga_wreg(12, pitch);

        dyn_cols = (int)(w / 8u);
        dyn_rows = (int)(h / 8u);
        if (dyn_cols > MAX_COLS) dyn_cols = MAX_COLS;
        if (dyn_rows > MAX_ROWS) dyn_rows = MAX_ROWS;
    }

    {
        unsigned int fifo_min = 16;
        unsigned int fifo_max = 65536;
        svga_fifo[0] = fifo_min;
        svga_fifo[1] = fifo_max;
        svga_fifo[2] = fifo_min;
        svga_fifo[3] = fifo_min;
    }

    svga_wreg(20, 1);
    svga_wreg(1, 1);

    svga_enabled = 1;
    return 1;
}

static int svga_init(void) {
    /* Find VMSVGA PCI device (15ad:0405) */
    PciDevice dev = pci_find(0x15AD, 0x0405);
    if (!dev.valid) return 0;

    /* BAR0 = I/O port base */
    unsigned int bar0 = pci_read32(dev.bus, dev.slot, 0, 0x10);
    svga_io_base = (unsigned short)(bar0 & ~3u);

    /* BAR1 = framebuffer physical address */
    unsigned int bar1 = pci_read32(dev.bus, dev.slot, 0, 0x14);
    unsigned int fb_base = bar1 & ~0xFu;

    /* BAR2 = FIFO physical address */
    unsigned int bar2 = pci_read32(dev.bus, dev.slot, 0, 0x18);
    unsigned int fifo_base = bar2 & ~0xFu;

    svga_fb   = (volatile unsigned int *)(unsigned long long)fb_base;
    svga_fifo = (volatile unsigned int *)(unsigned long long)fifo_base;

    /* Version negotiation */
    svga_wreg(0, 0x90000002); /* SVGA_REG_ID = SVGA_ID_2 */
    if ((svga_rreg(0) & 0xFF) < 2) return 0;

    /* Read max supported dimensions */
    unsigned int max_w = svga_rreg(16); /* SVGA_REG_MAX_WIDTH  */
    unsigned int max_h = svga_rreg(17); /* SVGA_REG_MAX_HEIGHT */

    /* Máxima resolución que anuncia el dispositivo (acotada por seguridad). */
    unsigned int w = max_w;
    unsigned int h = max_h;
    if (w > 2560u) w = 2560u;
    if (h > 1600u) h = 1600u;
    if (w < 640u || h < 480u) { w = 640u; h = 480u; }

    /* Program the chosen mode */
    svga_wreg(2, w);   /* WIDTH */
    svga_wreg(3, h);   /* HEIGHT */
    svga_wreg(7, 32);  /* BPP */

    /* Init FIFO */
    unsigned int fifo_min = 16;
    unsigned int fifo_max = 65536;
    svga_fifo[0] = fifo_min;
    svga_fifo[1] = fifo_max;
    svga_fifo[2] = fifo_min;
    svga_fifo[3] = fifo_min;

    svga_wreg(20, 1);  /* CONFIG_DONE */
    svga_wreg(1, 1);   /* ENABLE */

    /* Read back actual values the device accepted */
    w = svga_rreg(2);
    h = svga_rreg(3);
    unsigned int pitch = svga_rreg(12);

    /* Set dynamic cols/rows */
    dyn_cols = (int)(w / 8);
    dyn_rows = (int)(h / 8);
    if (dyn_cols > MAX_COLS) dyn_cols = MAX_COLS;
    if (dyn_rows > MAX_ROWS) dyn_rows = MAX_ROWS;

    /* Store FB info for the rest of the OS */
    *(volatile uint32_t*)(unsigned long long)NEXUS_BOOT_INFO_PHYS = NEXUS_BOOT_HANDOFF_MAGIC;
    *(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 4) = w;
    *(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 8) = h;
    *(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 12) = pitch;
    *(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 16) = 32u;
    *(volatile uint32_t*)(uintptr_t)(NEXUS_BOOT_INFO_PHYS + 20) = fb_base;
    *(volatile uint32_t*)(uintptr_t)(NEXUS_BOOT_INFO_PHYS + 24) = 0u;

    svga_enabled = 1;
    return 1;
}

/* ── End VMSVGA ───────────────────────────────────────────────────── */

static inline uint64_t vc_lfb_phys(void) {
    uint32_t lo = *(volatile uint32_t*)(uintptr_t)(NEXUS_BOOT_INFO_PHYS + 20);
    uint32_t hi = *(volatile uint32_t*)(uintptr_t)(NEXUS_BOOT_INFO_PHYS + 24);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
static inline volatile unsigned int* vc_lfb(void) {
    return (volatile unsigned int*)(uintptr_t)vc_lfb_phys();
}
static inline int vc_width(void)  { return (int)*(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 4); }
static inline int vc_height(void) { return (int)*(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 8); }
static inline int vc_pitch(void)  { return (int)*(volatile uint32_t*)(unsigned long long)(NEXUS_BOOT_INFO_PHYS + 12); }
static inline int vc_stride(void) { return vc_pitch() / 4; }

void vesa_force_refresh(void) {
    if (svga_enabled) {
        svga_refresh_full();
        return;
    }
    /* No tocar registros VGA (GR/AR) con VBE+LFB: en VirtualBox/QEMU puede romper
     * el modo lineal y dejar franjas negras; el host ya muestra el LFB mapeado. */
    __asm__ volatile("" ::: "memory");
}

void vesa_console_init(void) {
    /* BootInfo @ 0x5000: no sobrescribir si ya hay handoff VBE del stage2. */
    if (*(volatile uint32_t*)(unsigned long long)NEXUS_BOOT_INFO_PHYS == NEXUS_BOOT_HANDOFF_MAGIC) {
        vesa_console_active = 1;
        (void)svga_attach_fifo_for_vbe();
    } else if (svga_init()) {
        vesa_console_active = 1;
    } else {
        VesaBootInfo vbi;
        if (!gfx_vesa_detect(&vbi)) return;
        vesa_console_active = 1;
    }

    volatile unsigned int* fb = vc_lfb();
    int w = vc_width();
    int h = vc_height();
    int s = vc_stride();

    for(int y = 0; y < h; y++)
        for(int x = 0; x < w; x++)
            fb[y * s + x] = 0;

    vesa_force_refresh();

    for(int i = 0; i < TBUF_SZ; i += 2) {
        vesa_text_buf[i] = ' ';
        vesa_text_buf[i+1] = 0x07;
        prev_buf[i] = 0xFF;
        prev_buf[i+1] = 0xFF;
    }
}

static inline void vc_draw_cell(int r, int c, unsigned char ch, unsigned char attr) {
    unsigned int fg = vga16[attr & 0x0F];
    unsigned int bg = vga16[(attr >> 4) & 0x0F];

    volatile unsigned int* fb = vc_lfb();
    int w  = vc_width();
    int h  = vc_height();
    int s  = vc_stride();

    int px = c * 8;
    int py = r * 8;

    if(px + 8 > w || py + 8 > h) return;

    const unsigned char* gl = font8x8_get(ch);

    for(int y = 0; y < 8; y++) {
        unsigned char bits = (ch != 0x00 && ch != 0x20 && ch != 0x7F) ? gl[y] : 0;
        volatile unsigned int* row = fb + (py + y) * s + px;

        row[0] = (bits & 0x01) ? fg : bg;
        row[1] = (bits & 0x02) ? fg : bg;
        row[2] = (bits & 0x04) ? fg : bg;
        row[3] = (bits & 0x08) ? fg : bg;
        row[4] = (bits & 0x10) ? fg : bg;
        row[5] = (bits & 0x20) ? fg : bg;
        row[6] = (bits & 0x40) ? fg : bg;
        row[7] = (bits & 0x80) ? fg : bg;
    }
}

void vesa_console_flush(void) {
    if(!vesa_console_active) return;

    for(int r = 0; r < dyn_rows; r++) {
        for(int c = 0; c < dyn_cols; c++) {
            int idx = (r * dyn_cols + c) * 2;
            if(vesa_text_buf[idx] != prev_buf[idx] || vesa_text_buf[idx+1] != prev_buf[idx+1]) {
                vc_draw_cell(r, c, vesa_text_buf[idx], vesa_text_buf[idx+1]);
                prev_buf[idx] = vesa_text_buf[idx];
                prev_buf[idx+1] = vesa_text_buf[idx+1];
            }
        }
    }
    vesa_force_refresh();
}

static void vc_flush_all(void) {
    if(!vesa_console_active) return;
    for(int i = 0; i < TBUF_SZ; i++) prev_buf[i] = ~vesa_text_buf[i];
    vesa_console_flush();
}

static volatile char* const VGA = (volatile char*)0xB8000;

void limpiar_pantalla(void) {
    if(vesa_console_active) {
        for(int i = 0; i < TBUF_SZ; i += 2) {
            vesa_text_buf[i] = ' ';
            vesa_text_buf[i+1] = 0x07;
        }
        vc_flush_all();
    } else {
        for(int i = 0; i < 4000; i += 2) { VGA[i] = ' '; VGA[i+1] = 0x07; }
    }
}

int kprint_color(char* texto, int cursor, char color) {
    int vcols = vesa_console_active ? dyn_cols : 80;
    int stride = vcols * 2;

    if (smp_needs_locks())
        spinlock_lock(&smp_console_lock);

    for(int i = 0; texto[i]; i++) {
        if(texto[i] == '\n') {
            cursor = (cursor / stride + 1) * stride;
        } else {
            if(vesa_console_active) {
                if(cursor >= 0 && cursor < TBUF_SZ - 1) {
                    vesa_text_buf[cursor] = (unsigned char)texto[i];
                    vesa_text_buf[cursor+1] = (unsigned char)color;
                }
            } else {
                VGA[cursor] = texto[i];
                VGA[cursor+1] = color;
            }
            cursor += 2;
        }
    }
    if(vesa_console_active) vesa_console_flush();
    if (smp_needs_locks())
        spinlock_unlock(&smp_console_lock);
    nexus_tty_cursor = cursor;
    return cursor;
}

int imprimir_centrado(char* t, int c, char color) {
    int vcols = vesa_console_active ? dyn_cols : 80;
    int l = 0; while(t[l] && t[l] != '\n') l++;
    int e = (vcols - l) / 2;
    for(int i = 0; i < e; i++) {
        if(vesa_console_active) {
            if(c >= 0 && c < TBUF_SZ - 1) {
                vesa_text_buf[c] = ' ';
                vesa_text_buf[c+1] = (unsigned char)color;
            }
        } else {
            VGA[c] = ' '; VGA[c+1] = color;
        }
        c += 2;
    }
    return kprint_color(t, c, color);
}

void obtener_hora(char* b) {
    outb(0x70, 0x04); unsigned char h = inb(0x71);
    outb(0x70, 0x02); unsigned char m = inb(0x71);
    outb(0x70, 0x00); unsigned char s = inb(0x71);
    b[0]=(h>>4)+'0'; b[1]=(h&0xF)+'0'; b[2]=':';
    b[3]=(m>>4)+'0'; b[4]=(m&0xF)+'0'; b[5]=':';
    b[6]=(s>>4)+'0'; b[7]=(s&0xF)+'0'; b[8]='\0';
}

void obtener_fecha(char* b) {
    unsigned char day, mo, yr;
    outb(0x70, 0x07); day = inb(0x71);
    outb(0x70, 0x08); mo = inb(0x71);
    outb(0x70, 0x09); yr = inb(0x71);
    b[0] = (day >> 4) + '0';
    b[1] = (day & 0x0F) + '0';
    b[2] = '/';
    b[3] = (mo >> 4) + '0';
    b[4] = (mo & 0x0F) + '0';
    b[5] = '/';
    b[6] = '2';
    b[7] = '0';
    b[8] = (yr >> 4) + '0';
    b[9] = (yr & 0x0F) + '0';
    b[10] = '\0';
}

void retraso(int ms) {
    uint64_t start = ticks;
    /* PIT ~1000 Hz: ~1 tick por ms */
    unsigned int need = (unsigned int)ms;
    if (need < 1u) need = 1u;
    volatile int safety = 0;
    while ((ticks - start) < (uint64_t)need) {
        __asm__ volatile("hlt");
        if(++safety > 500000) break;
    }
}

int  comparar_cadenas(char* s1, char* s2) {
    int i=0; while(s1[i]&&s2[i]){if(s1[i]!=s2[i])return 0;i++;} return s1[i]==s2[i];
}
void copiar_texto(char* d, char* o) {
    int i=0; while(o[i]&&i<255){d[i]=o[i];i++;} d[i]='\0';
}
int  empieza_con(char* c, char* p) {
    int i=0; while(p[i]){if(c[i]!=p[i])return 0;i++;} return 1;
}

void mostrar_intro(void) {
    int vcols = vesa_console_active ? dyn_cols : 80;
    int stride = vcols * 2;
    int maxcells = vcols * (vesa_console_active ? dyn_rows : 25);
    int bufsz = maxcells * 2;

    if(vesa_console_active) {
        for(int i = 0; i < bufsz; i += 2) { vesa_text_buf[i]=' '; vesa_text_buf[i+1]=0x00; }
    } else {
        for(int i = 0; i < 4000; i += 2) { VGA[i]=' '; VGA[i+1]=0x00; }
    }

    int cy = vesa_console_active ? dyn_rows/2 - 8 : 7;

    const char* logo[] = {
        " \xDB\xDB\xDB    \xDB\xDB ",
        " \xDB\xDB\xDB\xDB   \xDB\xDB ",
        " \xDB\xDB \xDB\xDB  \xDB\xDB ",
        " \xDB\xDB  \xDB\xDB \xDB\xDB ",
        " \xDB\xDB   \xDB\xDB\xDB\xDB ",
        " \xDB\xDB    \xDB\xDB\xDB ",
    };
    const unsigned char lcol[] = { 0x01, 0x01, 0x09, 0x09, 0x0B, 0x0B };

    for(int i = 0; i < 6; i++) {
        int row = (cy + i) * stride + (vcols/2 - 6) * 2;
        const char* s = logo[i];
        for(int j = 0; s[j]; j++) {
            int off = row + j*2;
            if(off >= 0 && off < bufsz - 1) {
                if(vesa_console_active) {
                    vesa_text_buf[off] = (unsigned char)s[j];
                    vesa_text_buf[off+1] = (s[j] != ' ') ? lcol[i] : 0x00;
                } else {
                    VGA[off] = s[j];
                    VGA[off+1] = (s[j] != ' ') ? lcol[i] : 0x00;
                }
            }
        }
        if(vesa_console_active) vesa_console_flush();
        retraso(30);
    }

    const char* title = "N E X U S  O S";
    int trow = cy + 8;
    int tcur = trow * stride + (vcols/2 - 7) * 2;
    for(int i = 0; title[i]; i++) {
        int off = tcur + i*2;
        if(off >= 0 && off < bufsz - 1) {
            if(vesa_console_active) {
                vesa_text_buf[off] = (unsigned char)title[i];
                vesa_text_buf[off+1] = (title[i] != ' ') ? 0x0F : 0x00;
            } else {
                VGA[off] = title[i];
                VGA[off+1] = (title[i] != ' ') ? 0x0F : 0x00;
            }
        }
        if(title[i] != ' ') {
            if(vesa_console_active) vesa_console_flush();
            retraso(35);
        }
    }

    const char* sub = "Gaming Edition x86_64";
    int srow = trow + 1;
    int scur = srow * stride + (vcols/2 - 10) * 2;
    for(int i = 0; sub[i]; i++) {
        int off = scur + i*2;
        if(off >= 0 && off < bufsz - 1) {
            if(vesa_console_active) {
                vesa_text_buf[off] = (unsigned char)sub[i];
                vesa_text_buf[off+1] = 0x08;
            } else {
                VGA[off] = sub[i];
                VGA[off+1] = 0x08;
            }
        }
    }
    if(vesa_console_active) vesa_console_flush();

    int drow = srow + 3;
    int dots[5];
    for(int i = 0; i < 5; i++) {
        dots[i] = drow * stride + (vcols/2 - 4 + i*2) * 2;
    }

    for(int frame = 0; frame < 12; frame++) {
        int act = frame % 5;
        for(int i = 0; i < 5; i++) {
            int off = dots[i];
            if(off >= 0 && off < bufsz - 1) {
                int dist = (i - act + 5) % 5;
                unsigned char ch = (dist < 2) ? '\xFE' : '\xFA';
                unsigned char attr = (dist == 0) ? 0x0B : (dist == 1) ? 0x09 : 0x08;
                if(vesa_console_active) {
                    vesa_text_buf[off] = ch;
                    vesa_text_buf[off+1] = attr;
                } else {
                    VGA[off] = ch;
                    VGA[off+1] = attr;
                }
            }
        }
        if(vesa_console_active) vesa_console_flush();
        retraso(60);
    }

    retraso(80);
}
