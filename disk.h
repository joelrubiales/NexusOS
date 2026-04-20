#ifndef DISK_H
#define DISK_H

/* ATA PIO primary master (ports 0x1F0-0x1F7), LBA28 */

int disk_init(void);
int ata_read_sector(unsigned int lba, void* buf512);
int ata_write_sector(unsigned int lba, const void* buf512);

#endif
