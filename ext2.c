#include "ext2.h"
#include "disk.h"
#include "memory.h"
#include <stddef.h>

#define EROFS 30

static int              ext2_ok;
static uint64_t         ext2_vol0; /* byte offset del volumen en el disco */
static ext2_super_block_t ext2_sb;
static ext2_group_desc_t* ext2_gdt;
static uint32_t         ext2_ngroups;
static uint32_t         ext2_bsize;
static uint32_t         ext2_inode_size;
static uint32_t         ext2_ptrs_per_block;

static uint32_t r32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t r16(const uint8_t* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static int vol_read(uint64_t byte_off, void* buf, size_t len) {
    uint8_t* d = (uint8_t*)buf;
    uint64_t abs = ext2_vol0 + byte_off;
    while (len > 0) {
        uint32_t lba = (uint32_t)(abs / 512u);
        uint32_t sec_off = (uint32_t)(abs % 512u);
        uint8_t sec[512];
        if (ata_read_sector(lba, sec) != 0)
            return -1;
        size_t chunk = 512u - sec_off;
        if (chunk > len)
            chunk = len;
        for (size_t i = 0; i < chunk; i++)
            d[i] = sec[sec_off + i];
        d += chunk;
        len -= chunk;
        abs += chunk;
    }
    return 0;
}

static int read_block(uint32_t block, void* buf) {
    if (block == 0)
        return -1;
    return vol_read((uint64_t)block * (uint64_t)ext2_bsize, buf, (size_t)ext2_bsize);
}

uint32_t ext2_block_size(void) { return ext2_ok ? ext2_bsize : 0u; }

int ext2_mounted(void) { return ext2_ok; }

void ext2_umount(void) {
    if (ext2_gdt) {
        kfree(ext2_gdt);
        ext2_gdt = NULL;
    }
    ext2_ok = 0;
    ext2_ngroups = 0;
}

int ext2_mount(int ahci_port) {
    uint8_t raw[1024];
    uint32_t i;
    uint64_t gdt_byte;
    size_t   gdt_bytes;

    ext2_umount();
    (void)ahci_port;
    if (ahci_port != 0)
        return -1;
    ext2_vol0 = 0;

    if (disk_init() != 0)
        return -2;

    if (vol_read(1024u, raw, sizeof(raw)) != 0)
        return -3;

    __builtin_memcpy(&ext2_sb, raw, sizeof(ext2_sb));
    if (ext2_sb.s_magic != EXT2_SUPER_MAGIC)
        return -4;

    ext2_bsize = 1024u << ext2_sb.s_log_block_size;
    if (ext2_bsize < 1024u || ext2_bsize > (8u * 1024u * 1024u))
        return -5;

    ext2_inode_size = ext2_sb.s_inode_size;
    if (ext2_inode_size < EXT2_GOOD_OLD_INODE_SIZE)
        ext2_inode_size = EXT2_GOOD_OLD_INODE_SIZE;

    ext2_ptrs_per_block = ext2_bsize / 4u;
    ext2_ngroups =
        (ext2_sb.s_blocks_count + ext2_sb.s_blocks_per_group - 1u) / ext2_sb.s_blocks_per_group;

    gdt_byte = 1024ull + 1024ull;
    gdt_bytes = (size_t)ext2_ngroups * sizeof(ext2_group_desc_t);
    ext2_gdt = (ext2_group_desc_t*)kmalloc((uint64_t)gdt_bytes);
    if (!ext2_gdt)
        return -6;

    {
        uint8_t* p = (uint8_t*)ext2_gdt;
        uint64_t off = gdt_byte;
        size_t   left = gdt_bytes;
        while (left > 0) {
            size_t chunk = left;
            if (chunk > 65536u)
                chunk = 65536u;
            if (vol_read(off, p, chunk) != 0) {
                kfree(ext2_gdt);
                ext2_gdt = NULL;
                return -7;
            }
            p += chunk;
            off += chunk;
            left -= chunk;
        }
    }

    for (i = 0; i < ext2_ngroups; i++) {
        if (ext2_gdt[i].bg_inode_table == 0 || ext2_gdt[i].bg_block_bitmap == 0) {
            kfree(ext2_gdt);
            ext2_gdt = NULL;
            return -8;
        }
    }

    ext2_ok = 1;
    return 0;
}

int ext2_read_inode(uint32_t inode_num, ext2_inode_t* out) {
    uint32_t group, idx, table_block;
    uint64_t byte_in_table;
    uint32_t blk_off, inner;
    uint8_t* block_buf;

    if (!ext2_ok || !out || inode_num == 0)
        return -1;

    group = (inode_num - 1u) / ext2_sb.s_inodes_per_group;
    idx = (inode_num - 1u) % ext2_sb.s_inodes_per_group;
    if (group >= ext2_ngroups)
        return -1;

    table_block = ext2_gdt[group].bg_inode_table;
    byte_in_table = (uint64_t)idx * (uint64_t)ext2_inode_size;
    blk_off = (uint32_t)(byte_in_table / (uint64_t)ext2_bsize);
    inner = (uint32_t)(byte_in_table % (uint64_t)ext2_bsize);

    if (inner + sizeof(ext2_inode_t) > ext2_bsize)
        return -1;

    block_buf = (uint8_t*)kmalloc((uint64_t)ext2_bsize);
    if (!block_buf)
        return -1;
    if (read_block(table_block + blk_off, block_buf) != 0) {
        kfree(block_buf);
        return -1;
    }
    __builtin_memcpy(out, block_buf + inner, sizeof(ext2_inode_t));
    kfree(block_buf);
    return 0;
}

static int file_bmap(const ext2_inode_t* in, uint32_t file_block, uint32_t* out_phys) {
    uint8_t* tmp;

    if (file_block < EXT2_NDIR_BLOCKS) {
        *out_phys = in->i_block[file_block];
        return *out_phys ? 0 : -1;
    }

    file_block -= EXT2_NDIR_BLOCKS;
    if (file_block < ext2_ptrs_per_block) {
        uint32_t ib = in->i_block[EXT2_IND_BLOCK];
        if (!ib)
            return -1;
        tmp = (uint8_t*)kmalloc((uint64_t)ext2_bsize);
        if (!tmp)
            return -1;
        if (read_block(ib, tmp) != 0) {
            kfree(tmp);
            return -1;
        }
        *out_phys = r32(tmp + (size_t)file_block * 4u);
        kfree(tmp);
        return *out_phys ? 0 : -1;
    }

    file_block -= ext2_ptrs_per_block;
    if (file_block < ext2_ptrs_per_block * ext2_ptrs_per_block) {
        uint32_t dblk = in->i_block[EXT2_DIND_BLOCK];
        uint32_t l1 = file_block / ext2_ptrs_per_block;
        uint32_t l2 = file_block % ext2_ptrs_per_block;
        uint32_t ib;
        if (!dblk)
            return -1;
        tmp = (uint8_t*)kmalloc((uint64_t)ext2_bsize);
        if (!tmp)
            return -1;
        if (read_block(dblk, tmp) != 0) {
            kfree(tmp);
            return -1;
        }
        ib = r32(tmp + (size_t)l1 * 4u);
        kfree(tmp);
        if (!ib)
            return -1;
        tmp = (uint8_t*)kmalloc((uint64_t)ext2_bsize);
        if (!tmp)
            return -1;
        if (read_block(ib, tmp) != 0) {
            kfree(tmp);
            return -1;
        }
        *out_phys = r32(tmp + (size_t)l2 * 4u);
        kfree(tmp);
        return *out_phys ? 0 : -1;
    }

    (void)in;
    return -1;
}

int ext2_file_pread(const ext2_inode_t* inode, uint32_t offset, void* buf, uint32_t len) {
    uint8_t* d = (uint8_t*)buf;
    uint32_t got = 0;
    if (!ext2_ok || !inode || !buf || len == 0)
        return 0;
    if (offset >= inode->i_size)
        return 0;
    if (offset + len > inode->i_size)
        len = inode->i_size - offset;

    while (got < len) {
        uint32_t fbn = (offset + got) / ext2_bsize;
        uint32_t bio = (offset + got) % ext2_bsize;
        uint32_t phys;
        uint32_t chunk;
        uint8_t* blk;

        if (file_bmap(inode, fbn, &phys) != 0)
            break;
        blk = (uint8_t*)kmalloc((uint64_t)ext2_bsize);
        if (!blk)
            break;
        if (read_block(phys, blk) != 0) {
            kfree(blk);
            break;
        }
        chunk = ext2_bsize - bio;
        if (chunk > len - got)
            chunk = len - got;
        for (uint32_t i = 0; i < chunk; i++)
            d[got + i] = blk[bio + i];
        kfree(blk);
        got += chunk;
    }
    return (int)got;
}

static int dir_lookup_ino(uint32_t dir_ino, const char* name, int name_len, uint32_t* out_ino) {
    ext2_inode_t di;
    uint8_t* data;
    uint32_t pos;

    if (ext2_read_inode(dir_ino, &di) != 0)
        return -1;
    if ((di.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -1;
    if (di.i_size == 0 || di.i_size > 16u * 1024u * 1024u)
        return -1;

    data = (uint8_t*)kmalloc(di.i_size);
    if (!data)
        return -1;
    if (ext2_file_pread(&di, 0, data, di.i_size) != (int)di.i_size) {
        kfree(data);
        return -1;
    }

    pos = 0;
    while (pos + 8u <= di.i_size) {
        uint32_t ino_ent = r32(data + pos);
        uint16_t rl = r16(data + pos + 4u);
        uint8_t nl = data[pos + 6u];
        if (rl < 8u || pos + (uint32_t)rl > di.i_size)
            break;
        if (nl == (uint8_t)name_len) {
            int match = 1;
            int j;
            const char* en = (const char*)(data + pos + 8u);
            for (j = 0; j < name_len; j++) {
                if (en[j] != name[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                *out_ino = ino_ent;
                kfree(data);
                return 0;
            }
        }
        pos += (uint32_t)rl;
    }
    kfree(data);
    return -1;
}

static const char* skip_slash(const char* p) {
    while (*p == '/')
        p++;
    return p;
}

int ext2_lookup_path(const char* path, uint32_t* out_ino, ext2_inode_t* out_inode) {
    const char* p;
    uint32_t cur;

    if (!ext2_ok || !path || !out_ino || !out_inode)
        return -1;

    p = skip_slash(path);
    if (*p == 0)
        return ext2_read_inode(EXT2_ROOT_INO, out_inode) == 0 ? (*out_ino = EXT2_ROOT_INO, 0) : -1;

    cur = EXT2_ROOT_INO;
    for (;;) {
        const char* start = p;
        uint32_t next;
        ext2_inode_t tin;
        int len;

        while (*p && *p != '/')
            p++;
        len = (int)(p - start);
        if (len <= 0)
            return -1;

        if (dir_lookup_ino(cur, start, len, &next) != 0)
            return -1;

        if (*p == 0) {
            *out_ino = next;
            return ext2_read_inode(next, out_inode);
        }

        if (ext2_read_inode(next, &tin) != 0)
            return -1;
        if ((tin.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
            return -1;
        cur = next;
        p = skip_slash(p + 1);
    }
}

uint8_t* ext2_read_file_kmalloc(const char* path, uint32_t* out_size) {
    uint32_t ino;
    ext2_inode_t inode;
    uint8_t* buf;

    if (out_size)
        *out_size = 0;
    if (ext2_lookup_path(path, &ino, &inode) != 0)
        return NULL;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG)
        return NULL;
    if (inode.i_size == 0) {
        buf = (uint8_t*)kmalloc(1);
        if (buf && out_size)
            *out_size = 0;
        return buf;
    }
    buf = (uint8_t*)kmalloc(inode.i_size);
    if (!buf)
        return NULL;
    if (ext2_file_pread(&inode, 0, buf, inode.i_size) != (int)inode.i_size) {
        kfree(buf);
        return NULL;
    }
    if (out_size)
        *out_size = inode.i_size;
    return buf;
}

int ext2_write_inode_meta(const ext2_inode_t* inode, uint32_t ino_num) {
    (void)inode;
    (void)ino_num;
    return -EROFS;
}
