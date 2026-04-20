#include "setup.h"
#include "nexus.h"
#include "teclado.h"

extern volatile unsigned char tecla_nueva;
extern volatile unsigned char tecla_extended;
extern volatile uint64_t ticks;

#define V get_text_ptr()

static void s_flush(void) { if(vesa_console_active) vesa_console_flush(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  NexusOS Installer — Modern Setup Wizard (TUI)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Drawing primitives ──────────────────────────────────────────────────── */

static inline int s_cols(void) { return get_text_cols(); }
static inline int s_rows(void) { return vesa_console_active ? VC_ROWS : 25; }
static inline int s_stride(void) { return get_text_stride(); }
static inline int s_maxbuf(void) { return s_cols() * s_rows() * 2; }

static void s_fill_row(int row, unsigned char attr) {
    int base = row * s_stride();
    int cols = s_cols();
    for(int i = 0; i < cols; i++) { V[base + i*2] = ' '; V[base + i*2 + 1] = attr; }
}

static void s_putstr(int row, int col, const char* s, unsigned char attr) {
    int pos = row * s_stride() + col * 2;
    int mx = s_maxbuf();
    for(int i = 0; s[i]; i++) {
        if(pos >= mx) break;
        V[pos] = s[i]; V[pos+1] = attr;
        pos += 2;
    }
}

static void s_putchar(int row, int col, char ch, unsigned char attr) {
    int pos = row * s_stride() + col * 2;
    int mx = s_maxbuf();
    if(pos >= 0 && pos < mx) { V[pos] = ch; V[pos+1] = attr; }
}

static void s_center(int row, const char* s, unsigned char attr) {
    int len = 0; while(s[len]) len++;
    int col = (s_cols() - len) / 2; if(col < 0) col = 0;
    s_putstr(row, col, s, attr);
}

static int s_strlen(const char* s) { int i = 0; while(s[i]) i++; return i; }

static void s_hline(int row, int col, int w, unsigned char attr) {
    for(int i = 0; i < w; i++) s_putchar(row, col + i, '\xC4', attr);
}

static void s_fill_area(int r, int c, int w, int h, unsigned char attr) {
    for(int y = 0; y < h; y++)
        for(int x = 0; x < w; x++)
            s_putchar(r+y, c+x, ' ', attr);
}

/* Modern rounded-ish box using double-line chars */
static void s_box(int r, int c, int w, int h, unsigned char battr, unsigned char fill) {
    s_fill_area(r, c, w, h, fill);
    s_putchar(r, c, '\xC9', battr);
    s_putchar(r, c+w-1, '\xBB', battr);
    s_putchar(r+h-1, c, '\xC8', battr);
    s_putchar(r+h-1, c+w-1, '\xBC', battr);
    for(int i = 1; i < w-1; i++) {
        s_putchar(r, c+i, '\xCD', battr);
        s_putchar(r+h-1, c+i, '\xCD', battr);
    }
    for(int i = 1; i < h-1; i++) {
        s_putchar(r+i, c, '\xBA', battr);
        s_putchar(r+i, c+w-1, '\xBA', battr);
    }
}

/* ── Color scheme — dark modern ──────────────────────────────────────────── */
#define C_HEADER   0x70   /* black on light gray - sleek bar */
#define C_FOOTER   0x30   /* black on cyan */
#define C_BG       0x07   /* light gray on black - main background */
#define C_PANEL    0x0F   /* white on black - panel interior */
#define C_BORDER   0x0B   /* cyan on black - box borders */
#define C_TITLE    0x0F   /* bright white on black */
#define C_SUBTITLE 0x0B   /* cyan on black */
#define C_DIM      0x08   /* dark gray on black */
#define C_NORMAL   0x07   /* gray on black */
#define C_SEL_BG   0x9F   /* white on bright blue */
#define C_SEL_TXT  0x9F
#define C_INPUT    0x1F   /* white on blue */
#define C_OK       0x0A   /* green on black */
#define C_ERR      0x0C   /* red on black */
#define C_WARN     0x0E   /* yellow on black */
#define C_ACCENT   0x0B   /* cyan on black */
#define C_BAR_FULL 0xA0   /* black on green bg */
#define C_BAR_EMPT 0x80   /* black on dark gray bg */
#define C_LOGO1    0x09   /* blue on black */
#define C_LOGO2    0x0B   /* cyan on black */
#define C_LOGO3    0x0F   /* white on black */

static int  cur_step = 0;
static int  max_steps = 8;

/* ── Screen scaffolding ──────────────────────────────────────────────────── */

static int foot_row(void) { return s_rows() - 1; }

static void draw_screen(const char* step_name, const char* foot) {
    int cols = s_cols();
    int rows = s_rows();

    for(int r = 0; r < rows; r++) s_fill_row(r, C_BG);

    /* Header bar */
    s_fill_row(0, C_HEADER);
    s_putstr(0, 2, "\xFE NexusOS", C_HEADER);
    s_putstr(0, 14, "Installer", 0x78);
    int sl = s_strlen(step_name);
    s_putstr(0, cols - 2 - sl, step_name, C_HEADER);

    /* Progress bar in row 1 */
    s_fill_row(1, 0x00);
    int bar_start = cols / 8;
    int bar_total = cols * 3 / 4;
    for(int i = 0; i < max_steps; i++) {
        int x = bar_start + (i * bar_total) / max_steps;
        int xn = bar_start + ((i+1) * bar_total) / max_steps;
        unsigned char seg_attr;
        if(i < cur_step)       seg_attr = 0x0A;
        else if(i == cur_step) seg_attr = 0x0B;
        else                   seg_attr = 0x08;

        char marker = (i < cur_step) ? '\xFE' : (i == cur_step) ? '\xF9' : '\xFA';
        s_putchar(1, x, marker, seg_attr);
        for(int j = x+1; j < xn && j < bar_start + bar_total; j++)
            s_putchar(1, j, '\xC4', (i < cur_step) ? 0x02 : 0x08);
    }

    /* Footer */
    int fr = foot_row();
    s_fill_row(fr, C_FOOTER);
    s_putstr(fr, 2, foot, C_FOOTER);
    char sb[8]; sb[0]='['; sb[1]='0'+(char)(cur_step+1); sb[2]='/'; sb[3]='0'+(char)max_steps; sb[4]=']'; sb[5]=0;
    s_putstr(fr, cols - 7, sb, C_FOOTER);
    s_flush();
}

/* ── Input helpers ───────────────────────────────────────────────────────── */
static KbdState s_kbd;

static KbdEvent s_wait_key(void) {
    while(1) {
        if(!tecla_nueva) { __asm__ volatile("hlt"); continue; }
        unsigned char sc = tecla_nueva; tecla_nueva = 0;
        KbdEvent ev = kbd_handle_scancode(&s_kbd, sc);
        if(ev.type != KBD_EV_NONE) return ev;
    }
}

static int s_menu(const char** items, int count, int sel, int start_row, int col, int width) {
    while(1) {
        for(int i = 0; i < count; i++) {
            if(i == sel) {
                s_fill_area(start_row + i, col, width, 1, 0x9F);
                s_putstr(start_row + i, col + 1, "\x10", C_SEL_TXT);
                s_putstr(start_row + i, col + 3, items[i], C_SEL_TXT);
            } else {
                s_fill_area(start_row + i, col, width, 1, C_BG);
                s_putstr(start_row + i, col + 1, " ", C_DIM);
                s_putstr(start_row + i, col + 3, items[i], C_NORMAL);
            }
        }
        s_flush();
        KbdEvent ev = s_wait_key();
        if(ev.type == KBD_EV_UP)    sel = (sel - 1 + count) % count;
        if(ev.type == KBD_EV_DOWN)  sel = (sel + 1) % count;
        if(ev.type == KBD_EV_ENTER) return sel;
        if(ev.type == KBD_EV_ESC)   return -1;
    }
}

static int s_input(int row, int col, int maxlen, char* buf, int is_pw) {
    int len = s_strlen(buf);
    while(1) {
        s_fill_area(row, col, maxlen + 2, 1, C_INPUT);
        for(int i = 0; i < len; i++)
            s_putchar(row, col + 1 + i, is_pw ? '\xFE' : buf[i], 0x1F);
        s_putchar(row, col + 1 + len, '_', 0x1E);
        s_flush();
        KbdEvent ev = s_wait_key();
        if(ev.type == KBD_EV_ENTER) { buf[len] = 0; return len; }
        if(ev.type == KBD_EV_ESC)   return -1;
        if(ev.type == KBD_EV_BACKSPACE && len > 0) { len--; buf[len] = 0; }
        if(ev.type == KBD_EV_CHAR && len < maxlen - 1) {
            buf[len++] = (char)ev.ch; buf[len] = 0;
        }
    }
}

static void num_to_str(int n, char* b) {
    if(n <= 0) { b[0]='0'; b[1]=0; return; }
    char t[12]; int i=0;
    while(n) { t[i++] = '0' + (n%10); n /= 10; }
    int j=0; for(int k=i-1; k>=0; k--) b[j++]=t[k]; b[j]=0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STEP 0: Welcome
 * ═══════════════════════════════════════════════════════════════════════════ */
static int step_welcome(void) {
    cur_step = 0;
    draw_screen("Welcome", "ENTER = Begin installation  |  ESC = Cancel");

    int cols = s_cols();
    int rows = s_rows();
    int mid = rows / 2 - 8;
    if (mid < 3) mid = 3;

    s_center(mid,    "\xDB\xDB\xDB\xDB    \xDB\xDB\xDB", C_LOGO1);
    s_center(mid+1,  "\xDB\xDB\xDB\xDB\xDB   \xDB\xDB\xDB", C_LOGO1);
    s_center(mid+2,  "\xDB\xDB \xDB\xDB\xDB  \xDB\xDB\xDB", C_LOGO2);
    s_center(mid+3,  "\xDB\xDB  \xDB\xDB\xDB \xDB\xDB\xDB", C_LOGO2);
    s_center(mid+4,  "\xDB\xDB   \xDB\xDB\xDB\xDB\xDB\xDB", C_LOGO3);
    s_center(mid+5,  "\xDB\xDB    \xDB\xDB\xDB\xDB\xDB", C_LOGO3);

    s_center(mid+7,  "N E X U S  O S", C_TITLE);
    s_center(mid+8,  "Gaming Edition  \xFE  v3.0  \xFE  x86_64", C_ACCENT);

    int hl = cols / 4;
    s_hline(mid+10, (cols - hl) / 2, hl, C_DIM);

    s_center(mid+12, "Welcome to the NexusOS installer.", C_NORMAL);
    s_center(mid+13, "This wizard will configure your system", C_NORMAL);
    s_center(mid+14, "and install all required components.", C_NORMAL);

    s_center(mid+16, "\xDB\xDB  Press ENTER to start  \xDB\xDB", C_ACCENT);
    s_flush();

    while(1) {
        KbdEvent ev = s_wait_key();
        if(ev.type == KBD_EV_ENTER) return 1;
        if(ev.type == KBD_EV_ESC)   return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STEP 1: Language
 * ═══════════════════════════════════════════════════════════════════════════ */
static int step_language(SetupConfig* cfg) {
    cur_step = 1;
    draw_screen("Language", "UP/DOWN = Select  |  ENTER = Confirm  |  ESC = Back");

    int cols = s_cols();
    int bw = 40; if (bw > cols - 4) bw = cols - 4;
    int bx = (cols - bw) / 2;
    int by = 3;

    s_box(by, bx, bw, 16, C_BORDER, C_BG);
    s_center(by+1, "\xFE Select Language \xFE", C_TITLE);
    s_hline(by+2, bx+1, bw-2, C_DIM);

    s_putstr(by+3, bx+2, "Your system language determines the", C_DIM);
    s_putstr(by+4, bx+2, "interface text and locale settings.", C_DIM);

    static const char* langs[] = {
        "English",          "Espanol",
        "Portugues (BR)",   "Francais",
        "Deutsch",          "Italiano",
        "Nederlands",       "Polski"
    };

    int sel = s_menu(langs, 8, cfg->language, by+6, bx+4, bw-8);
    if(sel < 0) return -1;
    cfg->language = sel;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STEP 2: Keyboard Layout
 * ═══════════════════════════════════════════════════════════════════════════ */
static int step_keyboard(SetupConfig* cfg) {
    cur_step = 2;
    draw_screen("Keyboard", "UP/DOWN = Select  |  ENTER = Confirm  |  ESC = Back");

    int cols = s_cols();
    int bw = 44; if (bw > cols - 4) bw = cols - 4;
    int bx = (cols - bw) / 2;
    int by = 3;

    s_box(by, bx, bw, 16, C_BORDER, C_BG);
    s_center(by+1, "\xFE Keyboard Layout \xFE", C_TITLE);
    s_hline(by+2, bx+1, bw-2, C_DIM);

    int def = cfg->kb_layout;
    if(cfg->language == 1 && def == 0) def = 1;
    if(cfg->language == 2 && def == 0) def = 2;
    if(cfg->language == 3 && def == 0) def = 3;
    if(cfg->language == 4 && def == 0) def = 4;
    if(cfg->language == 5 && def == 0) def = 5;

    s_putstr(by+3, bx+2, "Choose your keyboard layout.", C_DIM);

    static const char* kbds[] = {
        "US English (QWERTY)",
        "Spanish (ISO-ES)",
        "Portuguese (BR-ABNT2)",
        "French (AZERTY)",
        "German (QWERTZ)",
        "Italian (QWERTY)",
        "UK English (QWERTY)",
        "Latin American"
    };

    int sel = s_menu(kbds, 8, def, by+5, bx+4, bw-8);
    if(sel < 0) return -1;
    cfg->kb_layout = sel;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STEP 3: Timezone
 * ═══════════════════════════════════════════════════════════════════════════ */
static int step_timezone(SetupConfig* cfg) {
    cur_step = 3;
    draw_screen("Timezone", "UP/DOWN = Select  |  ENTER = Confirm  |  ESC = Back");

    int cols = s_cols();
    int bw = 56; if (bw > cols - 4) bw = cols - 4;
    int bx = (cols - bw) / 2;
    int by = 3;

    s_box(by, bx, bw, 18, C_BORDER, C_BG);
    s_center(by+1, "\xFE Select Timezone \xFE", C_TITLE);
    s_hline(by+2, bx+1, bw-2, C_DIM);

    int def = cfg->timezone;
    if(cfg->language == 1 && def == 0) def = 1;
    if(cfg->language == 2 && def == 0) def = 4;

    static const char* tzs[] = {
        "UTC+0   London, Lisbon, Reykjavik",
        "UTC+1   Madrid, Paris, Berlin, Rome",
        "UTC+2   Helsinki, Athens, Cairo",
        "UTC+3   Moscow, Istanbul, Riyadh",
        "UTC-3   Buenos Aires, Sao Paulo",
        "UTC-4   Santiago, Caracas, Halifax",
        "UTC-5   New York, Bogota, Lima",
        "UTC-6   Mexico City, Chicago",
        "UTC-7   Denver, Phoenix",
        "UTC-8   Los Angeles, Vancouver",
        "UTC+9   Tokyo, Seoul",
        "UTC+10  Sydney, Melbourne"
    };

    int sel = s_menu(tzs, 12, def, by+3, bx+3, bw-6);
    if(sel < 0) return -1;
    cfg->timezone = sel;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STEP 4: Disk Setup
 * ═══════════════════════════════════════════════════════════════════════════ */
static int step_disk(SetupConfig* cfg) {
    cur_step = 4;
    draw_screen("Disk Setup", "UP/DOWN = Select  |  ENTER = Confirm  |  ESC = Back");

    int cols = s_cols();
    int bw = 64; if (bw > cols - 4) bw = cols - 4;
    int bx = (cols - bw) / 2;
    int by = 3;

    s_box(by, bx, bw, 9, C_BORDER, C_BG);
    s_center(by+1, "\xFE Storage Devices Detected \xFE", C_TITLE);
    s_hline(by+2, bx+1, bw-2, C_DIM);
    s_putstr(by+3, bx+3, "NAME        SIZE     TYPE     LABEL", C_ACCENT);
    s_putstr(by+4, bx+3, "/dev/sda    1.44 MB  Floppy   NexusOS Boot", C_NORMAL);
    s_putstr(by+5, bx+3, "/dev/ram0   64 KB    RAMDisk  System RAM", C_NORMAL);
    s_putstr(by+6, bx+3, "/dev/sdb    256 MB   VirtIO   Virtual HDD", C_DIM);

    int b2y = by + 10;
    s_box(b2y, bx, bw, 7, C_BORDER, C_BG);
    s_center(b2y+1, "\xFE Install Target \xFE", C_TITLE);

    static const char* tgts[] = {
        "/dev/sda  (Erase disk - recommended)",
        "/dev/ram0 (RAM only - volatile)",
        "/dev/sdb  (VirtIO disk)"
    };
    int sel = s_menu(tgts, 3, cfg->disk_target, b2y+3, bx+4, bw-8);
    if(sel < 0) return -1;
    cfg->disk_target = sel;

    cur_step = 4;
    draw_screen("Filesystem", "UP/DOWN = Select  |  ENTER = Confirm  |  ESC = Back");

    int fw = 48; if (fw > cols - 4) fw = cols - 4;
    int fx = (cols - fw) / 2;

    s_box(4, fx, fw, 13, C_BORDER, C_BG);
    s_center(5, "\xFE Filesystem Format \xFE", C_TITLE);
    s_hline(6, fx+1, fw-2, C_DIM);
    s_putstr(7, fx+2, "Disk will be formatted. All data lost.", C_WARN);

    static const char* fss[] = {
        "ext4   \xFA Recommended, stable",
        "btrfs  \xFA Snapshots, CoW, modern",
        "xfs    \xFA High performance",
        "f2fs   \xFA Flash-optimized, SSD"
    };
    sel = s_menu(fss, 4, cfg->disk_format, 9, fx+4, fw-8);
    if(sel < 0) return -1;
    cfg->disk_format = sel;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STEP 5: User Account
 * ═══════════════════════════════════════════════════════════════════════════ */
static int step_user(SetupConfig* cfg) {
    cur_step = 5;
    int cols = s_cols();
    int bw = 52; if (bw > cols - 4) bw = cols - 4;
    int bx = (cols - bw) / 2;
    int by = 3;
retry_u:
    draw_screen("User Account", "Type info  |  ENTER = Confirm  |  ESC = Back");

    s_box(by, bx, bw, 17, C_BORDER, C_BG);
    s_center(by+1, "\xFE Create Your Account \xFE", C_TITLE);
    s_hline(by+2, bx+1, bw-2, C_DIM);

    s_putstr(by+3, bx+3, "Your account will have sudo privileges.", C_DIM);
    s_putstr(by+4, bx+3, "Root is always available via su.", C_DIM);

    s_putstr(by+6, bx+3, "Username", C_ACCENT);
    int r = s_input(by+7, bx+3, 26, cfg->username, 0);
    if(r < 0) return -1;
    if(r == 0) {
        s_putstr(by+15, bx+3, "\x10 Username cannot be empty!", C_ERR);
        retraso(700); cfg->username[0] = 0; goto retry_u;
    }
    for(int i = 0; cfg->username[i]; i++) {
        if(cfg->username[i] == ' ') {
            s_putstr(by+15, bx+3, "\x10 No spaces allowed in username!", C_ERR);
            retraso(700); cfg->username[0] = 0; goto retry_u;
        }
    }

    s_putstr(by+9, bx+3, "Password", C_ACCENT);
    cfg->password[0] = 0;
    r = s_input(by+10, bx+3, 26, cfg->password, 1);
    if(r < 0) return -1;
    if(r < 4) {
        s_putstr(by+15, bx+3, "\x10 Minimum 4 characters!", C_ERR);
        retraso(700); cfg->password[0] = 0; goto retry_u;
    }

    static char conf[32]; conf[0] = 0;
    s_putstr(by+12, bx+3, "Confirm password", C_ACCENT);
    r = s_input(by+13, bx+3, 26, conf, 1);
    if(r < 0) return -1;
    int ok = 1;
    for(int i = 0; ; i++) { if(cfg->password[i] != conf[i]) { ok=0; break; } if(!cfg->password[i]) break; }
    if(!ok) {
        s_putstr(by+15, bx+3, "\x10 Passwords don't match!", C_ERR);
        retraso(700); cfg->password[0] = 0; goto retry_u;
    }

    s_putstr(by+15, bx+3, "\xFB Account ready!", C_OK);
    retraso(300);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STEP 6: Hostname
 * ═══════════════════════════════════════════════════════════════════════════ */
static int step_hostname(SetupConfig* cfg) {
    cur_step = 6;
    draw_screen("Hostname", "Type hostname  |  ENTER = Confirm  |  ESC = Back");

    int cols = s_cols();
    int bw = 48; if (bw > cols - 4) bw = cols - 4;
    int bx = (cols - bw) / 2;
    int by = 5;

    s_box(by, bx, bw, 10, C_BORDER, C_BG);
    s_center(by+1, "\xFE Machine Name \xFE", C_TITLE);
    s_hline(by+2, bx+1, bw-2, C_DIM);

    s_putstr(by+3, bx+2, "Identifies your PC on the network", C_DIM);
    s_putstr(by+4, bx+2, "and in the shell prompt.", C_DIM);

    s_putstr(by+6, bx+2, "Hostname", C_ACCENT);
    int r = s_input(by+7, bx+2, 24, cfg->hostname, 0);
    if(r < 0) return -1;
    if(r == 0) copiar_texto(cfg->hostname, "nexusos");

    s_putstr(by+9, bx+2, "\xFB", C_OK);
    s_putstr(by+9, bx+4, cfg->hostname, C_ACCENT);
    retraso(300);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STEP 7: Desktop Type
 * ═══════════════════════════════════════════════════════════════════════════ */
static int step_desktop(SetupConfig* cfg) {
    cur_step = 7;
    draw_screen("Installation", "UP/DOWN = Select  |  ENTER = Confirm  |  ESC = Back");

    int cols = s_cols();
    int bw = 56; if (bw > cols - 4) bw = cols - 4;
    int bx = (cols - bw) / 2;
    int by = 3;

    s_box(by, bx, bw, 16, C_BORDER, C_BG);
    s_center(by+1, "\xFE Choose Installation Type \xFE", C_TITLE);
    s_hline(by+2, bx+1, bw-2, C_DIM);

    static const char* opts[] = {
        "Gaming Edition (Full DE + gaming)",
        "Full Desktop   (NexusDE + apps)",
        "Minimal        (NexusDE only)",
        "Server         (CLI, no GUI)"
    };

    static const char* descs[] = {
        "Optimized for gaming. GPU drivers,",
        "Complete desktop experience with",
        "Lightweight desktop with essential",
        "Command-line only server install."
    };
    static const char* descs2[] = {
        "Steam, Vulkan, ProtonGE included.",
        "all applications and utilities.",
        "tools. Fast and lightweight.",
        "No graphical environment."
    };

    int sel = cfg->install_desktop;
    while(1) {
        for(int i = 0; i < 4; i++) {
            int row = by + 4 + i * 2;
            if(i == sel) {
                s_fill_area(row, bx+2, bw-4, 1, 0x9F);
                s_putstr(row, bx+3, "\x10", C_SEL_TXT);
                s_putstr(row, bx+5, opts[i], C_SEL_TXT);
            } else {
                s_fill_area(row, bx+2, bw-4, 1, C_BG);
                s_putstr(row, bx+3, " ", C_DIM);
                s_putstr(row, bx+5, opts[i], C_NORMAL);
            }
        }
        int dr = by + 13;
        s_fill_area(dr, bx+2, bw-4, 2, C_BG);
        s_putstr(dr, bx+4, descs[sel], C_DIM);
        s_putstr(dr+1, bx+4, descs2[sel], C_DIM);

        s_flush();
        KbdEvent ev = s_wait_key();
        if(ev.type == KBD_EV_UP)    sel = (sel - 1 + 4) % 4;
        if(ev.type == KBD_EV_DOWN)  sel = (sel + 1) % 4;
        if(ev.type == KBD_EV_ENTER) { cfg->install_desktop = sel; return 1; }
        if(ev.type == KBD_EV_ESC)   return -1;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  STEP 8: Summary
 * ═══════════════════════════════════════════════════════════════════════════ */
static const char* lang_names[]  = {"English","Espanol","Portugues","Francais",
                                     "Deutsch","Italiano","Nederlands","Polski"};
static const char* kb_names[]    = {"us","es","pt-br","fr","de","it","gb","latam"};
static const char* tz_short[]    = {"UTC+0","UTC+1","UTC+2","UTC+3","UTC-3",
                                     "UTC-4","UTC-5","UTC-6","UTC-7","UTC-8",
                                     "UTC+9","UTC+10"};
static const char* locale_codes[] = {"en_US.UTF-8","es_ES.UTF-8","pt_BR.UTF-8",
                                     "fr_FR.UTF-8","de_DE.UTF-8","it_IT.UTF-8",
                                     "nl_NL.UTF-8","pl_PL.UTF-8"};
static const char* disk_names[]  = {"/dev/sda","/dev/ram0","/dev/sdb"};
static const char* fs_names[]    = {"ext4","btrfs","xfs","f2fs"};
static const char* desk_names[]  = {"Gaming Edition","Full Desktop",
                                     "Minimal","Server (CLI)"};

static int step_summary(SetupConfig* cfg) {
    cur_step = 7;
    draw_screen("Confirm", "ENTER = Install now  |  ESC = Go back");

    int cols = s_cols();
    int bw = 60; if (bw > cols - 4) bw = cols - 4;
    int bx = (cols - bw) / 2;
    int by = 3;
    int lc = bx + 3;
    int vc = bx + 18;

    s_box(by, bx, bw, 18, C_BORDER, C_BG);
    s_center(by+1, "\xFE Installation Summary \xFE", C_TITLE);
    s_hline(by+2, bx+1, bw-2, C_DIM);

    int r = by + 4;
    s_putstr(r, lc, "Language     ", C_DIM); s_putstr(r, vc, lang_names[cfg->language], C_ACCENT); r++;
    s_putstr(r, lc, "Keyboard     ", C_DIM); s_putstr(r, vc, kb_names[cfg->kb_layout], C_ACCENT); r++;
    s_putstr(r, lc, "Locale       ", C_DIM); s_putstr(r, vc, locale_codes[cfg->language], C_ACCENT); r++;
    s_putstr(r, lc, "Timezone     ", C_DIM); s_putstr(r, vc, tz_short[cfg->timezone], C_ACCENT); r++;
    s_putstr(r, lc, "Target       ", C_DIM); s_putstr(r, vc, disk_names[cfg->disk_target], C_ACCENT); r++;
    s_putstr(r, lc, "Filesystem   ", C_DIM); s_putstr(r, vc, fs_names[cfg->disk_format], C_ACCENT); r++;
    s_putstr(r, lc, "User         ", C_DIM); s_putstr(r, vc, cfg->username, C_ACCENT); r++;
    s_putstr(r, lc, "Hostname     ", C_DIM); s_putstr(r, vc, cfg->hostname, C_ACCENT); r++;
    s_putstr(r, lc, "Install      ", C_DIM); s_putstr(r, vc, desk_names[cfg->install_desktop], C_ACCENT); r++;

    s_hline(r+1, bx+1, bw-2, C_DIM);
    s_center(r+2, "Press ENTER to begin installation", C_WARN);
    s_flush();

    while(1) {
        KbdEvent ev = s_wait_key();
        if(ev.type == KBD_EV_ENTER) return 1;
        if(ev.type == KBD_EV_ESC)   return -1;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Installation Animation + "Downloading" Desktop
 * ═══════════════════════════════════════════════════════════════════════════ */
static void do_install(SetupConfig* cfg) {
    int cols = s_cols();
    int rows = s_rows();
    int fr = rows - 1;

    for(int r = 0; r < rows; r++) s_fill_row(r, C_BG);
    s_fill_row(0, C_HEADER);
    s_putstr(0, 2, "\xFE NexusOS", C_HEADER);
    s_putstr(0, 14, "Installing...", 0x7E);
    s_fill_row(fr, C_FOOTER);
    s_putstr(fr, 2, "Do not turn off your computer", C_FOOTER);

    int bw = 68; if (bw > cols - 4) bw = cols - 4;
    int bx = (cols - bw) / 2;
    int by = 3;
    int bh = rows - 6; if (bh > 20) bh = 20;

    s_box(by, bx, bw, bh, C_BORDER, C_BG);
    s_center(by+1, "\xFE Installing NexusOS \xFE", C_TITLE);
    s_hline(by+2, bx+1, bw-2, C_DIM);

    static const char* tasks[] = {
        "Partitioning disk...",
        "Formatting filesystem...",
        "Installing base system...",
        "Installing kernel 3.0.0-nexus...",
        "Installing bootloader (GRUB2)...",
        "Installing hardware drivers...",
        "Configuring network interfaces...",
        "Setting locale and timezone...",
        "Creating user account...",
        "Configuring sudo privileges...",
        "Downloading NexusDE desktop...",
        "Installing gaming packages...",
        "Installing GPU drivers (Vulkan)...",
        "Configuring system services...",
        "Running ldconfig...",
        "Generating initramfs...",
        "Finalizing..."
    };
    int tc = 17;
    if(cfg->install_desktop == 3) tc = 14;

    int bar_y = by + bh - 3;
    int bar_x = bx + 4;
    int bar_w = bw - 8;
    int task_row = by + 4;
    int log_start = by + 6;
    int log_max = bar_y - 2;

    for(int t = 0; t < tc; t++) {
        int pct = ((t + 1) * 100) / tc;
        char ps[8]; num_to_str(pct, ps);

        s_fill_area(task_row, bx+4, bw-8, 1, C_BG);
        s_putstr(task_row, bx+4, "\x10 ", C_ACCENT);
        s_putstr(task_row, bx+6, tasks[t], C_TITLE);

        int vis = log_max - log_start;
        int ls = t - vis; if(ls < 0) ls = 0;
        for(int l = ls; l < t; l++) {
            int row = log_start + (l - ls);
            if(row >= log_max) break;
            s_fill_area(row, bx+4, bw-8, 1, C_BG);
            s_putstr(row, bx+4, "  \xFB ", C_OK);
            s_putstr(row, bx+8, tasks[l], C_DIM);
        }

        s_fill_area(bar_y, bar_x, bar_w, 1, C_BG);
        int filled = ((bar_w - 2) * pct) / 100;
        s_putchar(bar_y, bar_x, '\xDD', C_DIM);
        for(int i = 0; i < bar_w - 2; i++) {
            if(i < filled) s_putchar(bar_y, bar_x + 1 + i, '\xDB', C_OK);
            else           s_putchar(bar_y, bar_x + 1 + i, '\xB0', C_DIM);
        }
        s_putchar(bar_y, bar_x + bar_w - 1, '\xDE', C_DIM);

        s_fill_area(bar_y + 1, bar_x, bar_w, 1, C_BG);
        int pc = bar_x + bar_w / 2 - 2;
        s_putstr(bar_y + 1, pc, ps, C_ACCENT);
        s_putstr(bar_y + 1, pc + s_strlen(ps), " %", C_ACCENT);

        s_flush();
        int d = 150 + (t & 1) * 80;
        if(t == 2 || t == 10) d = 350;
        if(t == 11 || t == 12) d = 300;
        if(t == 3) d = 250;
        retraso(d);
    }

    for(int i = 0; i < bar_w - 2; i++)
        s_putchar(bar_y, bar_x + 1 + i, '\xDB', C_OK);
    s_fill_area(bar_y + 1, bar_x, bar_w, 1, C_BG);
    s_putstr(bar_y + 1, bar_x + bar_w/2 - 2, "100 %", C_OK);
    s_fill_area(task_row, bx+4, bw-8, 1, C_BG);
    s_putstr(task_row, bx+4, "\xFB Installation complete!", C_OK);
    s_flush();
    retraso(500);

    if(cfg->install_desktop != 3) {
        for(int r = 2; r < fr; r++) s_fill_row(r, C_BG);
        s_fill_row(0, C_HEADER);
        s_putstr(0, 2, "\xFE NexusOS", C_HEADER);
        s_putstr(0, 14, "Configuring Desktop", 0x7E);

        int dw = 56; if (dw > cols - 4) dw = cols - 4;
        int dx = (cols - dw) / 2;
        int dy = 5;

        s_box(dy, dx, dw, 12, C_BORDER, C_BG);
        s_center(dy+1, "\xFE Downloading NexusDE \xFE", C_TITLE);
        s_hline(dy+2, dx+1, dw-2, C_DIM);

        static const char* dl_items[] = {
            "nexusde-core        2.4 MB",
            "nexusde-themes      1.1 MB",
            "nexus-icons         0.8 MB",
            "nexus-fonts         0.5 MB",
            "nexus-wallpapers    3.2 MB",
            "nexus-taskbar       0.3 MB",
            "nexus-compositor    1.6 MB",
            "nexus-gaming-tools  4.1 MB"
        };
        int dl_count = 8;
        if(cfg->install_desktop == 2) dl_count = 5;

        int dl_bar_y = dy + 10;
        int dl_bar_w = dw - 8;
        int dl_bar_x = dx + 4;

        for(int d = 0; d < dl_count; d++) {
            int pct = ((d + 1) * 100) / dl_count;
            s_fill_area(dy+4, dx+2, dw-4, 1, C_BG);
            s_putstr(dy+4, dx+2, "\x19 ", C_ACCENT);
            s_putstr(dy+4, dx+4, dl_items[d], C_TITLE);

            int fw = (dl_bar_w * pct) / 100;
            s_fill_area(dl_bar_y, dl_bar_x, dl_bar_w, 1, C_BG);
            for(int i = 0; i < dl_bar_w; i++)
                s_putchar(dl_bar_y, dl_bar_x + i, (i < fw) ? '\xDB' : '\xB0', (i < fw) ? C_OK : C_DIM);

            char ps2[8]; num_to_str(pct, ps2);
            s_fill_area(dl_bar_y+1, dl_bar_x, dl_bar_w, 1, C_BG);
            int pc2 = dl_bar_x + dl_bar_w / 2 - 2;
            s_putstr(dl_bar_y+1, pc2, ps2, C_ACCENT);
            s_putstr(dl_bar_y+1, pc2 + s_strlen(ps2), " %", C_ACCENT);

            if(d > 0 && d <= 4) {
                s_fill_area(dy+5 + d, dx+2, dw-4, 1, C_BG);
                s_putstr(dy+5 + d, dx+2, "  \xFB ", C_OK);
                s_putstr(dy+5 + d, dx+6, dl_items[d-1], C_DIM);
            }

            s_flush();
            retraso(200 + (d & 1) * 100);
        }
        for(int i = 0; i < dl_bar_w; i++) s_putchar(dl_bar_y, dl_bar_x + i, '\xDB', C_OK);
        s_fill_area(dl_bar_y+1, dl_bar_x, dl_bar_w, 1, C_BG);
        s_putstr(dl_bar_y+1, dl_bar_x + dl_bar_w/2 - 2, "100 %", C_OK);
        s_flush();
        retraso(300);
    }

    for(int r = 0; r < rows; r++) s_fill_row(r, 0x00);

    int mid = rows / 2 - 5;
    if (mid < 3) mid = 3;
    int cx = (cols - 10) / 2;

    s_center(mid,   "\xFB  Setup Complete  \xFB", C_OK);
    s_hline(mid+1, (cols-24)/2, 24, C_DIM);

    s_center(mid+3, "NexusOS 3.0 Gaming Edition", C_TITLE);
    s_center(mid+4, "has been installed successfully.", C_NORMAL);

    s_putstr(mid+6, cx, "User:     ", C_DIM);
    s_putstr(mid+6, cx+10, cfg->username, C_ACCENT);
    s_putstr(mid+7, cx, "Host:     ", C_DIM);
    s_putstr(mid+7, cx+10, cfg->hostname, C_ACCENT);
    s_putstr(mid+8, cx, "Desktop:  ", C_DIM);
    s_putstr(mid+8, cx+10, desk_names[cfg->install_desktop], C_ACCENT);

    s_hline(mid+10, (cols-24)/2, 24, C_DIM);
    s_flush();

    if(cfg->install_desktop != 3) {
        s_center(mid+12, "Starting NexusDE desktop...", C_ACCENT);
        for(int f = 0; f < 8; f++) {
            int dotx = (cols - 6) / 2;
            for(int d = 0; d < 3; d++) {
                unsigned char dc = (d == (f % 3)) ? C_TITLE : C_DIM;
                s_putchar(mid+13, dotx + d * 2, '.', dc);
            }
            s_flush();
            retraso(120);
        }
    } else {
        s_center(mid+12, "Booting into terminal...", C_ACCENT);
        s_flush();
        retraso(800);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Main Entry Point
 * ═══════════════════════════════════════════════════════════════════════════ */
int setup_run(SetupConfig* cfg) {
    kbd_init(&s_kbd);

    cfg->language = 1;
    cfg->kb_layout = 1;
    cfg->timezone = 1;
    cfg->username[0] = 0;
    cfg->password[0] = 0;
    copiar_texto(cfg->hostname, "nexusos");
    cfg->disk_target = 0;
    cfg->disk_format = 0;
    cfg->install_desktop = 0;
    cfg->completed = 0;

    int step = 0;
    while(step <= 8) {
        int result;
        switch(step) {
            case 0: result = step_welcome();      break;
            case 1: result = step_language(cfg);   break;
            case 2: result = step_keyboard(cfg);   break;
            case 3: result = step_timezone(cfg);   break;
            case 4: result = step_disk(cfg);       break;
            case 5: result = step_user(cfg);       break;
            case 6: result = step_hostname(cfg);   break;
            case 7: result = step_desktop(cfg);    break;
            case 8: result = step_summary(cfg);    break;
            default: result = 1; break;
        }
        if(result < 0) { if(step > 0) step--; else return 0; }
        else if(result == 0) return 0;
        else step++;
    }

    do_install(cfg);
    cfg->completed = 1;
    return 1;
}
