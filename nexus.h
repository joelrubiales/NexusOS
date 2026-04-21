#ifndef NEXUS_H
#define NEXUS_H

#include <stdint.h>

#define PIT_TICKS_PER_SEC 1000

/* ── I/O ports ────────────────────────────────────────────────────── */
static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(unsigned short port, unsigned short val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned short inw(unsigned short port) {
    unsigned short ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outl(unsigned short port, unsigned int val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned int inl(unsigned short port) {
    unsigned int ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ── Display ──────────────────────────────────────────────────────── */
void limpiar_pantalla(void);
int  kprint_color(char* texto, int cursor, char color);
int  imprimir_centrado(char* t, int c, char color);
void mostrar_intro(void);

/* ── Utilities ────────────────────────────────────────────────────── */
void retraso(int ms);
int  comparar_cadenas(char* s1, char* s2);
void copiar_texto(char* d, char* o);
int  empieza_con(char* c, char* p);
void obtener_hora(char* buffer);
/* DD/MM/YYYY a partir del RTC (CMOS); buffer >= 11 bytes */
void obtener_fecha(char* buffer);

/* ── IDT ──────────────────────────────────────────────────────────── */
void instalar_idt(void);
void idt_irq_install(uint8_t irq, void (*handler_asm)(void));
void pic_irq_unmask(uint8_t irq);

extern volatile unsigned char tecla_nueva;
extern volatile unsigned char tecla_extended;
extern volatile uint64_t ticks;

/* ── VESA text console (emulates VGA text on VESA framebuffer) ──── */
extern int vesa_console_active;
#define VC_MAX_COLS 160
#define VC_MAX_ROWS 120
int vc_get_cols(void);
int vc_get_rows(void);
#define VC_COLS vc_get_cols()
#define VC_ROWS vc_get_rows()
extern unsigned char vesa_text_buf[];
/* Cursor de texto compartido (shell + syscalls write a consola). */
extern int nexus_tty_cursor;
void vesa_console_init(void);
void vesa_console_flush(void);
void vesa_force_refresh(void);
void vesa_double_buffer_init(void);

static inline int get_text_cols(void) { return vesa_console_active ? vc_get_cols() : 80; }
static inline int get_text_stride(void) { return get_text_cols() * 2; }

static inline volatile char* get_text_ptr(void) {
    if(vesa_console_active) return (volatile char*)vesa_text_buf;
    return (volatile char*)0xB8000;
}

#endif
