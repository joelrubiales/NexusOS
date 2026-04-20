#ifndef NEXUS_EVENT_H
#define NEXUS_EVENT_H

#include <stdint.h>

/*
 * Sistema de Eventos de NexusOS — desacopla drivers de la GUI.
 *
 * Arquitectura SPSC (Single-Producer / Single-Consumer):
 *   Productores : ISRs de teclado (IRQ1) y ratón (IRQ12) → push_event().
 *   Consumidor  : Bucle principal de la GUI → pop_event().
 *
 * Seguridad en un núcleo sin SMP:
 *   Las ISRs corren con IF=0 → push() no es interrumpible.
 *   El bucle principal corre con IF=1 pero solo toca el campo 'head'.
 *   Un barrier de compilador entre la escritura del dato y la actualización
 *   de 'tail' garantiza visibilidad sin necesitar spinlocks.
 */

/* ── Tipos de evento ──────────────────────────────────────────────────── */
typedef enum {
    EVENT_NONE         = 0,
    EVENT_MOUSE_MOVE   = 1,  /* movimiento del ratón */
    EVENT_MOUSE_CLICK  = 2,  /* cambio de estado de botón */
    EVENT_KEY_PRESS    = 3,  /* tecla pulsada (make code, ya traducida) */
    EVENT_WINDOW_CLOSE = 4,  /* petición de cierre de ventana */
} EventType;

/* ── Estructura del evento ────────────────────────────────────────────── */
typedef struct {
    EventType     type;

    /* ── Datos de ratón (MOUSE_MOVE, MOUSE_CLICK) ─────────────────── */
    int           mouse_x;
    int           mouse_y;
    unsigned char mouse_buttons;  /* bitmask: bit0=izq, bit1=der, bit2=centro */
    int           mouse_pressed;  /* 1 = botón pulsado, 0 = botón soltado     */

    /* ── Datos de teclado (KEY_PRESS) ────────────────────────────── */
    unsigned char scancode;       /* scan code Set-1 original             */
    char          ascii;          /* carácter traducido (0 si no imprimible) */
    int           key_extended;   /* 1 si el scan code tuvo prefijo 0xE0   */

    /* ── Datos de ventana (WINDOW_CLOSE) ────────────────────────── */
    int           window_id;
} Event;

/* ── Capacidad del ring buffer ────────────────────────────────────────── */
#define EVT_QUEUE_SIZE 256

/* ── API pública ──────────────────────────────────────────────────────── */

/*
 * push_event: llamar desde ISR o sección crítica.
 * Si el buffer está lleno, descarta el evento (política lossy).
 */
void push_event(Event e);

/*
 * pop_event: llamar desde el bucle principal.
 * Devuelve 1 y rellena *e si había un evento; 0 si el buffer está vacío.
 */
int  pop_event(Event* e);

/* Descarta todos los eventos pendientes (útil al iniciar un nuevo estado). */
void flush_events(void);

#endif
