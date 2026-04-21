#ifndef NEXUS_TASK_H
#define NEXUS_TASK_H

#include <stdint.h>

/* ── Estados del TCB ────────────────────────────────────────────────── */
#define TASK_DEAD    0
#define TASK_READY   1
#define TASK_RUNNING 2
#define TASK_SLEEP   3

/* Máximo de tareas simultáneas. */
#define SCHED_MAX_TASKS   16

/* Palabras de 64 bits por pila de tarea (8 KiB = 1024 × 8). */
#define SCHED_STACK_WORDS 1024

/*
 * Quantum preventivo: ticks del PIT entre dos cambios de contexto.
 * PIT_TICKS_PER_SEC = 1000 Hz → SCHED_QUANTUM_TICKS = 10 → 10 ms por tarea.
 */
#define SCHED_QUANTUM_TICKS 10

/* ── Task Control Block (TCB) ───────────────────────────────────────── */
typedef struct Task {
    uint64_t    rsp;        /* RSP guardado cuando la tarea no está en CPU */
    uint64_t    id;         /* identificador único                         */
    int         state;      /* TASK_DEAD / TASK_READY / TASK_RUNNING / TASK_SLEEP */
    const char* name;       /* etiqueta legible                            */
} Task;

/* ── API pública ────────────────────────────────────────────────────── */
void     sched_init(void);

/*
 * Crea una nueva tarea del kernel (anillo 0).
 * Reserva una pila con kmalloc y construye el marco inicial para iretq.
 * Devuelve el índice de tarea o -1 si no hay hueco.
 */
int      sched_new_task(void (*entry)(void), const char* name);

/* Ring 3: RIP y RSP usuario (tras iretq). Devuelve índice de tarea o -1. */
int      sched_new_user_task(uint64_t user_rip, uint64_t user_rsp, const char* name);

/* Marca la tarea actual como DEAD (llamar desde el cuerpo de una tarea al terminar). */
void     sched_task_exit(void);

/*
 * Llamado DESDE el handler ASM de IRQ0 (sched_timer_handler).
 * Recibe el RSP actual (marco completo empujado por el handler), devuelve el
 * nuevo RSP al que debe cambiar el handler (misma tarea si no hay switch).
 */
uint64_t sched_tick(uint64_t current_rsp);

/* Índice de la tarea en ejecución (0 .. sched_task_count-1). Válido en syscalls / IRQ. */
int sched_current_tid(void);

/* 0 = solo tick sin switch; 1 = preemption activa. */
extern volatile int sched_enabled;

/* Acceso de solo lectura a la tabla de tareas (para herramientas de diagnóstico). */
extern Task sched_tasks[SCHED_MAX_TASKS];
extern int  sched_task_count;

#endif
