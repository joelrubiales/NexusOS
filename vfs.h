#ifndef NEXUS_VFS_H
#define NEXUS_VFS_H

#include <stdint.h>

/*
 * VFS mínimo de NexusOS.
 *
 * Fuente de datos: módulo GRUB (initrd.tar, formato USTAR).
 * GRUB pasa las direcciones físicas de inicio/fin en el Tag de Módulo
 * de la Multiboot2 Information Structure (tipo 3).
 *
 * API:
 *   vfs_init()          — llamar una vez en kernel_main, tras memory_init().
 *   vfs_ready()         — 1 si hay initrd válido montado.
 *   vfs_find()          — devuelve puntero directo al contenido en la RAM
 *                         (cero copias), null si el archivo no existe.
 *   vfs_load_bmp()      — decodifica un BMP 24/32-bit y devuelve un buffer
 *                         en el heap del kernel (kmalloc); se cachea.
 *   vfs_get_wallpaper() — carga /background.bmp una sola vez y cachea.
 *   vfs_get_icon()      — carga /icons/<id>.bmp una sola vez y cachea.
 */

/* Número máximo de iconos en caché. */
#define VFS_ICON_MAX 8

void           vfs_init(uint32_t mod_start, uint32_t mod_end);
int            vfs_ready(void);

/* Busca 'path' en el initrd.tar. Devuelve puntero al payload o NULL.
 * Rellena *out_size con el tamaño real del archivo. */
const uint8_t* vfs_find(const char* path, uint32_t* out_size);

/*
 * Carga un BMP 24/32-bit en formato 0xAARRGGBB (= gfx_draw_image_rgba).
 * Gestiona la orientación (top-down / bottom-up) automáticamente.
 * El buffer se reserva con kmalloc; no lo liberes (OS bare-metal).
 * Devuelve NULL si el archivo no existe o el BMP es inválido.
 */
uint32_t* vfs_load_bmp(const char* path, int* out_w, int* out_h);

/* Wallpaper: carga /background.bmp la primera vez y cachea el resultado. */
const uint32_t* vfs_get_wallpaper(int* out_w, int* out_h);

/*
 * Icono por índice: carga /icons/0.bmp … /icons/7.bmp.
 * Siempre 32×32. Devuelve NULL si no existe → usar fallback nativo.
 */
const uint32_t* vfs_get_icon(int id, int* out_w, int* out_h);

#endif
