#include "disk.h"
#include "nexus.h"

#define ATA_DATA        0x1F0
#define ATA_ERR         0x1F1
#define ATA_SECCNT      0x1F2
#define ATA_LBALO       0x1F3
#define ATA_LBAMID      0x1F4
#define ATA_LBAHI       0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_CMD         0x1F7
#define ATA_ALTSTS      0x3F6

#define CMD_READ_PIO    0x20
#define CMD_WRITE_PIO   0x30

static int ata_wait_ready(void) {
    int t = 0;
    while (t++ < 500000) {
        unsigned char st = inb(ATA_CMD);
        if (!(st & 0x80)) return 0;
    }
    return -1;
}

static int ata_wait_drq(void) {
    int t = 0;
    while (t++ < 500000) {
        unsigned char st = inb(ATA_CMD);
        if (st & 0x01) return -2;
        if (st & 0x08) return 0;
    }
    return -1;
}

int disk_init(void) {
    /* Seleccionar disco master, pequeño delay */
    outb(ATA_DRIVE, 0xA0);
    for (volatile int i = 0; i < 1000; i++);
    return ata_wait_ready();
}

static int ata_do_io(unsigned int lba, void* buf512, int writing) {
    unsigned short* w = (unsigned short*)buf512;
    int i;
    if (lba >= 0x0FFFFFFF) return -1;

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    for (volatile int d = 0; d < 100; d++);
    outb(ATA_SECCNT, 1);
    outb(ATA_LBALO, (unsigned char)(lba & 0xFF));
    outb(ATA_LBAMID, (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBAHI, (unsigned char)((lba >> 16) & 0xFF));
    outb(ATA_CMD, writing ? CMD_WRITE_PIO : CMD_READ_PIO);

    if (ata_wait_ready() < 0) return -3;
    if (ata_wait_drq() < 0) return -4;

    if (!writing) {
        for (i = 0; i < 256; i++) w[i] = inw(ATA_DATA);
    } else {
        for (i = 0; i < 256; i++) outw(ATA_DATA, w[i]);
    }

    /* Esperar que termine la operación */
    for (volatile int d = 0; d < 500000; d++) {
        unsigned char st = inb(ATA_CMD);
        if (!(st & 0x88)) break;
    }
    return 0;
}

int ata_read_sector(unsigned int lba, void* buf512) {
    return ata_do_io(lba, buf512, 0);
}

int ata_write_sector(unsigned int lba, const void* buf512) {
    return ata_do_io(lba, (void*)buf512, 1);
}
