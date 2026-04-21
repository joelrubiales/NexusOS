#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>

/*
 * Cabecera Ethernet (14 B) tal como llega del cable.
 * El campo type está en orden de red (big-endian): leer con net_ethertype().
 */
typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type_be;
} __attribute__((packed)) ethernet_frame_t;

#define NET_ETH_HDR_LEN 14u

uint16_t net_ethertype(const uint8_t* frame, size_t len);

void net_init(void);
void net_poll_rx_if_needed(void);

/* Llamado desde el driver E1000 al recibir un frame (IRQ o sondeo). */
void net_rx_frame(const uint8_t* data, uint16_t len);

void net_get_ipv4(uint8_t out_be[4]);
void net_get_mac(uint8_t mac[6]);
int  net_ready(void);

#endif
