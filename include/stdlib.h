/*
 * Stub freestanding: GCC xmmintrin.h → mm_malloc.h incluye <stdlib.h>.
 * No enlazamos malloc; solo hace falta que el preprocesador resuelva tipos.
 */
#ifndef NEXUS_STDLIB_H
#define NEXUS_STDLIB_H

#include <stddef.h>

extern int posix_memalign(void **memptr, size_t alignment, size_t size);

#endif
