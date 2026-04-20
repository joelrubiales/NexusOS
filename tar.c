/*
 * tar.c — Motor USTAR (TAR IEEE 1003.1-1988) para NexusOS.
 *
 * Formato de bloque POSIX USTAR (512 bytes por bloque):
 *   Offset   Bytes  Campo
 *   ─────────────────────────────────────────────────────────
 *    0        100   nombre del archivo (ASCII, null-terminado)
 *    100        8   modo de archivo (octal ASCII)
 *    108        8   UID (octal ASCII)
 *    116        8   GID (octal ASCII)
 *    124       12   tamaño en bytes (octal ASCII, 11 dígitos + '\0')
 *    136       12   timestamp mtime (octal)
 *    148        8   checksum
 *    156        1   tipo: '0'/'\0'=regular, '5'=dir, '2'=symlink, etc.
 *    157      100   nombre del enlace
 *    257        6   magic "ustar"
 *    345      155   prefijo de ruta (para rutas >100 chars)
 *   ─────────────────────────────────────────────────────────
 *   Total: 500 bytes de cabecera, 12 bytes de padding hasta 512.
 *
 *   Después de la cabecera vienen CEIL(size / 512) bloques de datos.
 *   El TAR finaliza con dos bloques consecutivos llenos de ceros.
 *
 * Esta implementación es zero-copy: data_ptr apunta directamente
 * al payload en la memoria del initrd.
 */

#include "tar.h"
#include <stdint.h>
#include <stddef.h>

/* ── Estado del módulo ───────────────────────────────────────────────────── */

static const uint8_t* tar_base = NULL;
static uint32_t       tar_size = 0;

/* ── Helpers internos ───────────────────────────────────────────────────── */

#define TAR_BLOCK_SZ 512u

/* Convierte n bytes de ASCII octal a uint32.
 * Se detiene ante cualquier carácter que no sea '0'-'7'. */
static uint32_t octal_to_u32(const uint8_t* s, int n)
{
    uint32_t v = 0;
    while (n-- > 0 && *s >= '0' && *s <= '7')
        v = v * 8u + (uint32_t)(*s++ - '0');
    return v;
}

/* Avanza `off` hasta el inicio del siguiente bloque TAR de datos.
 * Devuelve el nuevo offset. */
static uint32_t next_block_off(uint32_t off, uint32_t fsize)
{
    uint32_t data_blocks = (fsize + TAR_BLOCK_SZ - 1u) / TAR_BLOCK_SZ;
    return off + TAR_BLOCK_SZ + data_blocks * TAR_BLOCK_SZ;
}

/* Construye la ruta completa de la entrada en out->name (≤100 chars).
 * Combina el campo 'prefix' (offset 345, 155 bytes) y 'name' (offset 0).
 * Formatos soportados: USTAR con y sin prefijo. */
static void build_name(const uint8_t* hdr, VFS_Node* out)
{
    int             ei = 0;
    const uint8_t*  pfx = hdr + 345;   /* campo prefix USTAR */

    /* Prefijo (vacío en la mayoría de archivos cortos). */
    if (pfx[0] != 0) {
        int pi;
        for (pi = 0; pi < 155 && pfx[pi] != 0 && ei < 99; pi++)
            out->name[ei++] = (char)pfx[pi];
        if (ei < 99)
            out->name[ei++] = '/';
    }
    /* Nombre base. */
    {
        int ni;
        for (ni = 0; ni < 100 && hdr[ni] != 0 && ei < 100; ni++)
            out->name[ei++] = (char)hdr[ni];
    }
    out->name[ei] = '\0';
}

/* ── API pública ─────────────────────────────────────────────────────────── */

void tar_init(const uint8_t* base, uint32_t size)
{
    tar_base = base;
    tar_size = size;
}

uint32_t tar_total_payload_bytes(void)
{
    uint32_t off   = 0;
    uint32_t total = 0;

    if (!tar_base || tar_size < TAR_BLOCK_SZ)
        return 0;

    while (off + TAR_BLOCK_SZ <= tar_size) {
        const uint8_t* hdr   = tar_base + off;
        uint32_t       fsize;
        uint8_t        ftype;

        if (hdr[0] == 0)          /* bloque de fin de TAR */
            break;

        fsize = octal_to_u32(hdr + 124, 12);
        ftype = hdr[156];

        if (ftype == '0' || ftype == '\0')
            total += fsize;

        off = next_block_off(off, fsize);
    }

    return total;
}

int tar_next_entry(uint32_t* offset, VFS_Node* out)
{
    if (!tar_base || !offset || !out)
        return 0;

    while (*offset + TAR_BLOCK_SZ <= tar_size) {
        const uint8_t* hdr      = tar_base + *offset;
        uint32_t       fsize;
        uint8_t        ftype;
        uint32_t       next_off;

        if (hdr[0] == 0)
            return 0;   /* fin del TAR */

        fsize    = octal_to_u32(hdr + 124, 12);
        ftype    = hdr[156];
        next_off = next_block_off(*offset, fsize);

        if (ftype == '0' || ftype == '\0') {
            /* Entrada de archivo regular — rellenar out y avanzar. */
            build_name(hdr, out);
            out->size     = fsize;
            out->data_ptr = hdr + TAR_BLOCK_SZ;

            /* Verificar que el payload cabe en el TAR. */
            if (*offset + TAR_BLOCK_SZ + fsize > tar_size)
                return 0;

            *offset = next_off;
            return 1;
        }

        /* Directorio, symlink u otro tipo: saltar. */
        *offset = next_off;
    }

    return 0;
}
