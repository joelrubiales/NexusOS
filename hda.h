#ifndef NEXUS_HDA_H
#define NEXUS_HDA_H

#include <stddef.h>
#include <stdint.h>

/*
 * Intel High Definition Audio (Azalia) — esqueleto de driver MMIO.
 * Clase PCI 0x04 Multimedia, subclase 0x03 HDA.
 *
 * PCM: 16-bit little-endian, estéreo intercalado (LR), 44100 Hz.
 */

int  hda_init(void);
void hda_play_pcm(uint8_t* buffer, size_t size);

#endif
