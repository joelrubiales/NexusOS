/*
 * Scheduler preventivo round-robin para NexusOS (x86-64, bare-metal).
 *
 * Diseño
 * ──────
 * IRQ0 (PIT, 1000 Hz) → sched_timer_handler (ASM) → sched_tick (C).
 *
 * sched_tick() recibe el RSP del handler (que apunta al marco completo de
 * 15 GPR + 5 qwords de CPU), guarda ese RSP en el TCB de la tarea actual,
 * elige la siguiente tarea READY y devuelve su RSP guardado.
 * El handler ASM cambia RSP al valor devuelto, restaura GPRs e iretq.
 *
 * Cuando sched_enabled == 0 (durante el arranque), sched_tick solo
 * incrementa ticks y envía EOI sin cambiar de tarea.
 */
#include "task.h"
#include "memory.h"
#include "nexus.h"
#include "elf_loader.h"
#include "pit.h"
#include "xhci.h"
#include "net.h"

/* ── Estado global ─────────────────────────────────────────────────── */
volatile int sched_enabled  = 0;
Task         sched_tasks[SCHED_MAX_TASKS];
int          sched_task_count = 0;

static int      current_tid   = 0;
static uint64_t next_id       = 0;
static uint64_t quantum_count = 0;

/* ── Selectores GDT (ver boot.S) ───────────────────────────────────── */
#define SEL_CODE64  0x18ULL   /* 64-bit code ring 0 */
#define SEL_DATA32  0x10ULL   /* 32-bit data ring 0 (SS en Long Mode) */
#define RFLAGS_IF   0x202ULL  /* IF=1, bit 1 siempre activo            */
#define SEL_UCODE64 0x33ULL
#define SEL_UDATA   0x2Bu

/* ── Privado ────────────────────────────────────────────────────────── */

static void task_mark_exit(void) {
    /* Tareas que retornan llaman aquí a través del frame de retorno. */
    sched_task_exit();
}

/*
 * Construye el marco inicial en la pila de una tarea nueva para que
 * cuando el handler haga "pop r15…pop rax; iretq" arranque en `entry`.
 *
 * Layout de la pila (dirección alta → baja):
 *   [SS]     = SEL_DATA32
 *   [RSP]    = stack_top (valor inicial de RSP tras iretq)
 *   [RFLAGS] = RFLAGS_IF
 *   [CS]     = SEL_CODE64
 *   [RIP]    = entry
 *   [RAX]    = 0
 *   [RCX]    = 0
 *   [RDX]    = 0
 *   [RBX]    = 0
 *   [RSI]    = 0
 *   [RDI]    = 0
 *   [RBP]    = 0
 *   [R8]     = 0
 *   [R9]     = 0
 *   [R10]    = 0
 *   [R11]    = 0
 *   [R12]    = 0
 *   [R13]    = 0
 *   [R14]    = 0
 *   [R15]    = 0   ← TCB.rsp apunta aquí
 *
 * El orden de pop en sched_timer_handler es:
 *   pop r15, r14, r13, r12, r11, r10, r9, r8,
 *   pop rbp, rdi, rsi, rbx, rdx, rcx, rax
 *   iretq (consume RIP, CS, RFLAGS, RSP, SS)
 */
static uint64_t* build_initial_frame(uint64_t* stack_top, void (*entry)(void)) {
    uint64_t* sp = stack_top;

    /* Marco de iretq (5 qwords, orden: SS, RSP, RFLAGS, CS, RIP). */
    *--sp = SEL_DATA32;                      /* SS   */
    *--sp = (uint64_t)stack_top;             /* RSP  */
    *--sp = RFLAGS_IF;                       /* RFLAGS */
    *--sp = SEL_CODE64;                      /* CS   */
    *--sp = (uint64_t)entry;                 /* RIP  */

    /* Función de retorno de seguridad: si entry() retorna. */
    (void)task_mark_exit;

    /* 15 GPRs en el mismo orden que los pops del handler. */
    *--sp = 0; /* RAX */
    *--sp = 0; /* RCX */
    *--sp = 0; /* RDX */
    *--sp = 0; /* RBX */
    *--sp = 0; /* RSI */
    *--sp = 0; /* RDI */
    *--sp = 0; /* RBP */
    *--sp = 0; /* R8  */
    *--sp = 0; /* R9  */
    *--sp = 0; /* R10 */
    *--sp = 0; /* R11 */
    *--sp = 0; /* R12 */
    *--sp = 0; /* R13 */
    *--sp = 0; /* R14 */
    *--sp = 0; /* R15 */  /* ← TCB.rsp apunta aquí */

    return sp;
}

static uint64_t* build_user_initial_frame(uint64_t* stack_top_k, uint64_t user_rsp, uint64_t user_rip) {
    uint64_t* sp = stack_top_k;

    *--sp = SEL_UDATA;
    *--sp = user_rsp;
    *--sp = RFLAGS_IF;
    *--sp = SEL_UCODE64;
    *--sp = user_rip;

    *--sp = 0; /* RAX */
    *--sp = 0; /* RCX */
    *--sp = 0; /* RDX */
    *--sp = 0; /* RBX */
    *--sp = 0; /* RSI */
    *--sp = 0; /* RDI */
    *--sp = 0; /* RBP */
    *--sp = 0; /* R8  */
    *--sp = 0; /* R9  */
    *--sp = 0; /* R10 */
    *--sp = 0; /* R11 */
    *--sp = 0; /* R12 */
    *--sp = 0; /* R13 */
    *--sp = 0; /* R14 */
    *--sp = 0; /* R15 */
    return sp;
}

/* ── API pública ────────────────────────────────────────────────────── */

void sched_init(void) {
    /* Tarea 0 = hilo principal del kernel. Su RSP se capturará en el
     * primer tick una vez que sched_enabled se active. */
    sched_tasks[0].id    = next_id++;
    sched_tasks[0].rsp   = 0;
    sched_tasks[0].state = TASK_RUNNING;
    sched_tasks[0].name  = "kernel";
    sched_task_count = 1;
    current_tid  = 0;
    quantum_count = 0;
}

int sched_new_task(void (*entry)(void), const char* name) {
    uint64_t* stack;
    uint64_t* sp;
    int idx;

    if (sched_task_count >= SCHED_MAX_TASKS)
        return -1;

    stack = (uint64_t*)kmalloc((uint64_t)SCHED_STACK_WORDS * 8u);
    if (!stack)
        return -1;

    sp  = build_initial_frame(stack + SCHED_STACK_WORDS, entry);
    idx = sched_task_count;

    sched_tasks[idx].id    = next_id++;
    sched_tasks[idx].rsp   = (uint64_t)sp;
    sched_tasks[idx].state = TASK_READY;
    sched_tasks[idx].name  = name;
    sched_task_count++;
    return idx;
}

int sched_new_user_task(uint64_t user_rip, uint64_t user_rsp, const char* name) {
    uint64_t* stack;
    uint64_t* sp;
    int idx;

    if (sched_task_count >= SCHED_MAX_TASKS)
        return -1;

    stack = (uint64_t*)kmalloc((uint64_t)SCHED_STACK_WORDS * 8u);
    if (!stack)
        return -1;

    sp  = build_user_initial_frame(stack + SCHED_STACK_WORDS, user_rsp, user_rip);
    idx = sched_task_count;

    sched_tasks[idx].id    = next_id++;
    sched_tasks[idx].rsp   = (uint64_t)sp;
    sched_tasks[idx].state = TASK_READY;
    sched_tasks[idx].name  = name;
    sched_task_count++;
    return idx;
}

void sched_task_exit(void) {
    __asm__ volatile("cli");
    if (current_tid < sched_task_count)
        sched_tasks[current_tid].state = TASK_DEAD;
    __asm__ volatile("sti");
    for (;;)
        __asm__ volatile("hlt");
}

/*
 * Llamado desde sched_timer_handler (ASM) con el RSP del frame completo.
 * Devuelve el RSP al que debe cambiar el handler.
 *
 * CRÍTICO: esta función envía el EOI al PIC e incrementa ticks.
 * El handler ASM NO hace nada de eso.
 */
int sched_current_tid(void) {
    return current_tid;
}

uint64_t sched_tick(uint64_t current_rsp) {
    int   next, i;

    /* 1. Incrementar ticks (sustituye pit_irq_tick cuando el scheduler está activo). */
    ticks++;

    xhci_poll();
    net_poll_rx_if_needed();

    /* 2. EOI al PIC maestro (IRQ0 solo necesita PIC1). */
    outb(0x20, 0x20);

    /* 3. Si el scheduler no está habilitado todavía, no cambiar de tarea. */
    if (!sched_enabled || sched_task_count <= 1) {
        sched_tasks[current_tid].rsp = current_rsp;
        return current_rsp;
    }

    /* 4. Quantum: solo cambiar cada SCHED_QUANTUM_TICKS ticks. */
    quantum_count++;
    if (quantum_count < SCHED_QUANTUM_TICKS) {
        sched_tasks[current_tid].rsp = current_rsp;
        return current_rsp;
    }
    quantum_count = 0;

    /* 5. Guardar RSP de la tarea actual. */
    sched_tasks[current_tid].rsp = current_rsp;
    if (sched_tasks[current_tid].state == TASK_RUNNING)
        sched_tasks[current_tid].state = TASK_READY;

    /* 6. Round-robin: buscar la siguiente tarea READY. */
    next = current_tid;
    for (i = 0; i < sched_task_count; i++) {
        next = (next + 1) % sched_task_count;
        if (sched_tasks[next].state == TASK_READY)
            break;
    }

    /* Si no hay otra tarea lista, quedarse en la actual. */
    if (sched_tasks[next].state != TASK_READY)
        next = current_tid;

    /* 7. Activar nueva tarea. */
    sched_tasks[next].state = TASK_RUNNING;
    current_tid = next;
    return sched_tasks[next].rsp;
}
