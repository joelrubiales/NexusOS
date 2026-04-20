#include "memory.h"
#include "nexus.h"

/* Pool fijo en RAM alta (mapa identidad @ KHEAP_PHYS_START). Lista enlazada best-fit. */

#define KHMAG_FREE 0x6E6578534F532020ULL
#define KHMAG_USED 0x4E455855534F5321ULL

#define KHEAP_ALIGN   16ULL
#define KHEAP_MIN_SPL 64ULL

typedef struct HeapBlock {
    uint64_t magic;
    uint64_t size; /* bytes útiles (payload) tras este encabezado */
    struct HeapBlock* next;
} HeapBlock;

#define KHEAP_OVERHEAD ((sizeof(HeapBlock) + (KHEAP_ALIGN - 1)) & ~(KHEAP_ALIGN - 1))

static HeapBlock* heap_head;
static int heap_ready;

static uint64_t align_up(uint64_t x) {
    return (x + (KHEAP_ALIGN - 1)) & ~(KHEAP_ALIGN - 1);
}

void kheap_init(void) {
    HeapBlock* h = (HeapBlock*)(uintptr_t)KHEAP_PHYS_START;
    uint64_t payload_max;
    if (KHEAP_SIZE <= KHEAP_OVERHEAD) {
        heap_head = 0;
        heap_ready = 0;
        return;
    }
    payload_max = KHEAP_SIZE - KHEAP_OVERHEAD;
    h->magic = KHMAG_FREE;
    h->size = payload_max;
    h->next = 0;
    heap_head = h;
    heap_ready = 1;
}

void kheap_panic_nomem(const char* msg) {
    const char* s;
    volatile unsigned short* vga = (volatile unsigned short*)0xB8000;
    int row, col;

    __asm__ volatile("cli");

    for (s = "\r\n[NexusOS] FATAL: kmalloc failed: "; *s; s++) {
        while ((inb(0x3FD) & 0x20u) == 0) { }
        outb(0x3F8, (unsigned char)*s);
    }
    for (s = msg; *s; s++) {
        while ((inb(0x3FD) & 0x20u) == 0) { }
        outb(0x3F8, (unsigned char)*s);
    }
    while ((inb(0x3FD) & 0x20u) == 0) { }
    outb(0x3F8, '\r');
    while ((inb(0x3FD) & 0x20u) == 0) { }
    outb(0x3F8, '\n');

    for (row = 0; row < 25; row++) {
        for (col = 0; col < 80; col++)
            vga[row * 80 + col] = (unsigned short)(0x0700 | ' ');
    }
    {
        const char* t = "KMALLOC FAIL - halt (serial log)";
        col = 0;
        while (*t && col < 80)
            vga[col++] = (unsigned short)(0x4F00 | (unsigned char)*t++);
    }

    for (;;) __asm__ volatile("hlt");
}

void* kmalloc(uint64_t size) {
    HeapBlock *h, *best;
    uint64_t need_payload, need_total;

    if (!heap_ready || size == 0) return 0;

    if (size > KHEAP_SIZE || size > (KHEAP_SIZE - KHEAP_OVERHEAD))
        return 0;

    need_payload = align_up(size);
    if (need_payload < size)
        return 0;

    need_total = KHEAP_OVERHEAD + need_payload;
    if (need_total < need_payload || need_total > KHEAP_SIZE)
        return 0;

    best = 0;
    for (h = heap_head; h; h = h->next) {
        if (h->magic == KHMAG_FREE && h->size >= need_payload) {
            if (!best || h->size < best->size)
                best = h;
        }
    }
    if (!best) return 0;

    if (best->size >= need_payload + KHEAP_OVERHEAD + KHEAP_MIN_SPL) {
        HeapBlock* split =
            (HeapBlock*)((unsigned char*)best + KHEAP_OVERHEAD + need_payload);
        split->magic = KHMAG_FREE;
        split->size = best->size - need_payload - KHEAP_OVERHEAD;
        split->next = best->next;
        best->next = split;
        best->size = need_payload;
    }

    best->magic = KHMAG_USED;
    return (unsigned char*)best + KHEAP_OVERHEAD;
}

void kfree(void* ptr) {
    HeapBlock* h;
    if (!ptr || !heap_ready) return;

    h = (HeapBlock*)((unsigned char*)ptr - KHEAP_OVERHEAD);
    if (h->magic != KHMAG_USED)
        return;

    h->magic = KHMAG_FREE;

    if (h->next && h->next->magic == KHMAG_FREE) {
        h->size += KHEAP_OVERHEAD + h->next->size;
        h->next = h->next->next;
    }
    {
        HeapBlock* p = heap_head;
        while (p && p->next != h) p = p->next;
        if (p && p->magic == KHMAG_FREE) {
            p->size += KHEAP_OVERHEAD + h->size;
            p->next = h->next;
        }
    }
}
