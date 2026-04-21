#include "net.h"
#include "e1000.h"
#include <stddef.h>

/* QEMU user networking (slirp) — misma IP por defecto que muchas VMs. */
static const uint8_t g_ipv4[4] = { 10u, 0u, 2u, 15u };
static uint8_t       g_mac[6];
static int           g_ready;

uint16_t net_ethertype(const uint8_t* frame, size_t len) {
    if (!frame || len < NET_ETH_HDR_LEN)
        return 0;
    return (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);
}

void net_get_ipv4(uint8_t out_be[4]) {
    unsigned i;
    for (i = 0; i < 4u; i++)
        out_be[i] = g_ipv4[i];
}

void net_get_mac(uint8_t mac[6]) {
    unsigned i;
    for (i = 0; i < 6u; i++)
        mac[i] = g_mac[i];
}

int net_ready(void) { return g_ready; }

void net_init(void) {
    g_ready = 0;
    if (e1000_init() != 0)
        return;
    e1000_get_mac(g_mac);
    g_ready = 1;
}

void net_poll_rx_if_needed(void) {
    static unsigned n;
    if (!e1000_needs_poll())
        return;
    n++;
    if ((n % 25u) != 0u)
        return;
    e1000_poll_rx_if_needed();
}

#define ETHERTYPE_ARP 0x0806u

#define ARP_HTYPE_ETHER 0x0001u
#define ARP_PTYPE_IPV4  0x0800u
#define ARP_OP_REQUEST  0x0001u
#define ARP_OP_REPLY    0x0002u

static int ipv4_eq(const uint8_t* a, const uint8_t* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

void net_rx_frame(const uint8_t* data, uint16_t len) {
    const uint8_t *arp, *sha, *spa, *tha, *tpa;
    uint16_t et, oper;
    uint8_t  reply[64];
    uint8_t* arpo;

    if (!g_ready || !data || len < NET_ETH_HDR_LEN)
        return;

    et = net_ethertype(data, len);
    if (et != ETHERTYPE_ARP)
        return;
    if (len < 14u + 28u)
        return;

    arp = data + 14u;
    if ((((uint16_t)arp[0] << 8) | arp[1]) != ARP_HTYPE_ETHER)
        return;
    if ((((uint16_t)arp[2] << 8) | arp[3]) != ARP_PTYPE_IPV4)
        return;
    if (arp[4] != 6u || arp[5] != 4u)
        return;

    oper = (uint16_t)(((uint16_t)arp[6] << 8) | arp[7]);
    if (oper != ARP_OP_REQUEST)
        return;

    sha = arp + 8u;
    spa = sha + 6u;
    tha = spa + 4u;
    tpa = tha + 6u;

    if (!ipv4_eq(tpa, g_ipv4))
        return;

    __builtin_memset(reply, 0, sizeof(reply));
    __builtin_memcpy(reply, sha, 6u);
    __builtin_memcpy(reply + 6u, g_mac, 6u);
    reply[12] = (uint8_t)(ETHERTYPE_ARP >> 8);
    reply[13] = (uint8_t)ETHERTYPE_ARP;

    arpo = reply + 14u;
    arpo[0] = 0;
    arpo[1] = 1;
    arpo[2] = (uint8_t)(ARP_PTYPE_IPV4 >> 8);
    arpo[3] = (uint8_t)ARP_PTYPE_IPV4;
    arpo[4] = 6;
    arpo[5] = 4;
    arpo[6] = (uint8_t)(ARP_OP_REPLY >> 8);
    arpo[7] = (uint8_t)ARP_OP_REPLY;
    __builtin_memcpy(arpo + 8u, g_mac, 6u);
    __builtin_memcpy(arpo + 14u, g_ipv4, 4u);
    __builtin_memcpy(arpo + 18u, sha, 6u);
    __builtin_memcpy(arpo + 24u, spa, 4u);

    (void)e1000_transmit(reply, 60u);
}
