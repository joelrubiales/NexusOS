#ifndef NIC_H
#define NIC_H

typedef struct {
    unsigned char mac[6];
    char          name[16];   // "E1000", "RTL8139", etc.
    unsigned char valid;
} NicInfo;

// Escanea PCI y rellena NicInfo. Devuelve NicInfo.valid.
NicInfo nic_detect();

// Formatea mac como "xx:xx:xx:xx:xx:xx\0" en out (necesita 18 bytes)
void nic_mac_str(const NicInfo* nic, char* out);

#endif
