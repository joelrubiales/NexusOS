#ifndef NEXUS_TAR_H
#define NEXUS_TAR_H

/*
 * tar.h — Motor USTAR para NexusOS.
 *
 * Implementa un iterador de solo avance sobre un archivo TAR cargado en
 * memoria.  Diseñado para el instalador gráfico: permite avanzar entrada
 * por entrada y medir el progreso en bytes, sin copiar ningún dato
 * (zero-copy — el puntero data_ptr apunta directamente al initrd).
 *
 * Flujo típico:
 *   tar_init(base, size);          // llamado desde vfs_init()
 *   uint32_t total = tar_total_payload_bytes();
 *   uint32_t off   = 0;
 *   VFS_Node node;
 *   while (tar_next_entry(&off, &node)) {
 *       // "extraer" node.size bytes de node.data_ptr
 *   }
 */

#include <stdint.h>

/* ── Nodo VFS básico ─────────────────────────────────────────────────────── */

/*
 * Estructura devuelta por tar_next_entry().
 * name    : ruta completa del archivo, null-terminada (max 100 chars).
 * data_ptr: puntero al payload dentro del initrd (zero-copy).
 * size    : tamaño del payload en bytes.
 */
typedef struct {
    char           name[101];
    const uint8_t* data_ptr;
    uint32_t       size;
} VFS_Node;

/* ── API pública ─────────────────────────────────────────────────────────── */

/*
 * tar_init — registra el bloque de memoria del TAR.
 * Debe llamarse antes de cualquier otra función.
 * base : dirección física del primer byte del TAR (dirección lógica identidad).
 * size : longitud en bytes del TAR.
 */
void tar_init(const uint8_t* base, uint32_t size);

/*
 * tar_total_payload_bytes — suma los tamaños de todos los archivos regulares.
 * Necesario para calcular el porcentaje de progreso.
 * Devuelve 0 si el TAR no está inicializado o está vacío.
 */
uint32_t tar_total_payload_bytes(void);

/*
 * tar_next_entry — iterador de solo avance.
 *
 * *offset : en entrada, posición actual en el TAR (iniciar con 0);
 *           en salida, posición del siguiente bloque a examinar.
 * out     : si la función devuelve 1, se rellena con los datos de la
 *           entrada encontrada (solo archivos regulares, '0' o '\0').
 *
 * Devuelve 1 si se encontró un archivo regular, 0 al llegar al final.
 * Los directorios y entradas especiales se saltan automáticamente.
 */
int tar_next_entry(uint32_t* offset, VFS_Node* out);

#endif /* NEXUS_TAR_H */
