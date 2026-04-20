#include "multitasking.h"

#define STACK_WORDS 1024

static unsigned long long stack_a[STACK_WORDS];
static unsigned long long stack_b[STACK_WORDS];

typedef struct {
    unsigned long long saved_rsp;
    unsigned long long init_rsp;
} TaskSlot;

static TaskSlot tasks[2];
static volatile int current = 0;

static unsigned long long kernel_saved_rsp;

static volatile unsigned long long count_a;
static volatile unsigned long long count_b;
static volatile int total_yields;

extern void cpu_switch(unsigned long long *old_sp_out, unsigned long long new_sp);

static void task_a(void) {
    for (;;) {
        count_a++;
        yield();
    }
}

static void task_b(void) {
    for (;;) {
        count_b++;
        yield();
    }
}

static unsigned long long* setup_stack(unsigned long long stack_top_words[],
                                       unsigned int nwords, void (*entry)(void)) {
    unsigned long long* sp = &stack_top_words[nwords];
    *--sp = (unsigned long long)entry;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    return sp;
}

void multitasking_init(void) {
    tasks[0].init_rsp = (unsigned long long)setup_stack(stack_a, STACK_WORDS, task_a);
    tasks[1].init_rsp = (unsigned long long)setup_stack(stack_b, STACK_WORDS, task_b);
    tasks[0].saved_rsp = 0;
    tasks[1].saved_rsp = 0;
    current = 0;
    count_a = count_b = 0;
    total_yields = 0;
}

void yield(void) {
    int self = current;
    int next = 1 - self;
    unsigned long long nsp;

    total_yields++;
    if (total_yields >= 20) {
        cpu_switch(&tasks[self].saved_rsp, kernel_saved_rsp);
        current = self;
        return;
    }

    nsp = tasks[next].saved_rsp ? tasks[next].saved_rsp : tasks[next].init_rsp;
    cpu_switch(&tasks[self].saved_rsp, nsp);
    current = self;
}

void coop_run(void) {
    multitasking_init();
    total_yields = 0;
    count_a = count_b = 0;

    cpu_switch(&kernel_saved_rsp, tasks[0].init_rsp);
    /* Vuelve aquí cuando las tareas han hecho 20 yields */
}

unsigned multitasking_selftest(void) {
    coop_run();
    return (unsigned)(count_a + count_b);
}

unsigned long long multitasking_count_a(void) { return count_a; }
unsigned long long multitasking_count_b(void) { return count_b; }
