/**
 * The network card's cable: Ethernet frames over UDP.
 *
 * See xbox_net_udp.c for the format and the switches.
 */
#ifndef XBOX_NET_UDP_H
#define XBOX_NET_UDP_H

#include <stdint.h>

/* The largest frame carried: a full Ethernet frame without its checksum. */
#define XBOX_NET_FRAME_MAX 1514u

/* Open the socket and start receiving, from RECOMP_SYSLINK_LOCAL and
 * RECOMP_SYSLINK_REMOTE. Returns 1 when the tunnel is up. own_mac is this
 * console's address, so frames that come back to it are not delivered. */
int  xbox_NetUdpStart(const uint8_t own_mac[6]);

/* Send one frame now, as one datagram. */
void xbox_NetUdpSend(const uint8_t *frame, uint32_t len);

/* The next received frame that is due (RECOMP_SYSLINK_DELAY_MS holds frames
 * back), copied to out without taking it: its length, or 0 when none is due.
 * xbox_NetUdpPop then takes it -- the card pops only once a receive buffer
 * has accepted the frame, so a full ring delays a frame instead of losing
 * it. */
uint32_t xbox_NetUdpPeek(uint8_t *out, uint32_t cap);
void     xbox_NetUdpPop(void);

/* Counters for the summary line. */
void xbox_NetUdpStats(unsigned long *sent, unsigned long *received,
                      unsigned long *dropped);

#endif /* XBOX_NET_UDP_H */
