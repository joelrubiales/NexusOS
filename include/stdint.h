/* Freestanding stdint for NexusOS (-nostdinc). Requires GCC/clang. */
#ifndef NEXUS_STDINT_H
#define NEXUS_STDINT_H

typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;

typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __INT64_TYPE__ int64_t;

typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTPTR_TYPE__ intptr_t;

#define UINT8_C(x)  (x##u)
#define UINT16_C(x) (x##u)
#define UINT32_C(x) (x##u)
#define UINT64_C(x) (x##ull)

#define INT64_C(x)  (x##ll)
#define UINT64_MAX  UINT64_C(18446744073709551615)

#endif
