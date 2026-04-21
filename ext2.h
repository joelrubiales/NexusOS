#ifndef NEXUS_EXT2_H
#define NEXUS_EXT2_H

#include <stdint.h>

/* Ext2 — estructuras en disco (little-endian). Ver Linux uapi/linux/ext2_fs.h */

#define EXT2_SUPER_MAGIC 0xEF53u

#define EXT2_ROOT_INO 2u

#define EXT2_S_IFDIR 0x4000u
#define EXT2_S_IFREG 0x8000u
#define EXT2_S_IFMT  0xF000u

#define EXT2_NDIR_BLOCKS 12u
#define EXT2_IND_BLOCK   12u
#define EXT2_DIND_BLOCK  13u
#define EXT2_TIND_BLOCK  14u
#define EXT2_N_BLOCKS    15u

#define EXT2_GOOD_OLD_INODE_SIZE 128u

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    int32_t  s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* Extended (rev >= 1) */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
} ext2_super_block_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_group_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT2_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} ext2_dir_entry_t;

/*
 * Montaje: ahci_port reservado (0 = volumen ATA PIO primario, LBA0 = inicio volumen).
 * Lee superbloque en offset 1024 bytes.
 */
int ext2_mount(int ahci_port);
void ext2_umount(void);
int ext2_mounted(void);

uint32_t ext2_block_size(void);

/* Carga inodo 1-based (2 = raíz). Devuelve 0 o negativo. */
int ext2_read_inode(uint32_t inode_num, ext2_inode_t* out);

/*
 * Resuelve ruta absoluta tipo "/home/user/a.txt" (sin ..).
 * Devuelve inodo y tamaño si es fichero regular.
 */
int ext2_lookup_path(const char* path, uint32_t* out_ino, ext2_inode_t* out_inode);

/*
 * Lee todo el fichero a un buffer kmalloc (caller kfree) o NULL.
 */
uint8_t* ext2_read_file_kmalloc(const char* path, uint32_t* out_size);

/* Lectura por rangos desde un inodo ya cargado (fichero regular). */
int ext2_file_pread(const ext2_inode_t* inode, uint32_t offset, void* buf, uint32_t len);

/* Escritura: no soportada en volumen (devuelve -30 EROFS). Base para futuro. */
int ext2_write_inode_meta(const ext2_inode_t* inode, uint32_t ino_num);

#endif
