#ifndef NEXUS_STATUS_H
#define NEXUS_STATUS_H

#include <stdint.h>

/*
 * Códigos de resultado del kernel (convención: 0 = éxito).
 * Extender según subsistemas sin reutilizar valores asignados.
 */
typedef enum NexusStatus {
    NEXUS_OK            = 0,
    NEXUS_ERR_INVAL     = -1,
    NEXUS_ERR_NOMEM     = -2,
    NEXUS_ERR_IO        = -3,
    NEXUS_ERR_NOT_FOUND = -4,
    NEXUS_ERR_BUSY      = -5,
} NexusStatus;

static inline int32_t nexus_status_to_i32(NexusStatus s) {
    return (int32_t)s;
}

#endif
