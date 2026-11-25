#ifndef QRX_HOSTNET_H
#define QRX_HOSTNET_H
#include <stdbool.h>
#include <stdint.h>
/* One Winsock-style call. a[0..7] arguments, a[8] result, a[9] WSA error, a[10] ip, a[11] port.
 * ram is guest RAM, for the buffers named in the arguments. */
/* a[1] guest address, a[2] bytes, at most 256: the host's random generator. */
#define QRX_NET_RANDOM 19u
/* a[10] Unix seconds, a[11] microseconds: the host's clock. The guest has no battery clock.
 * a[12] the host's offset from UTC in seconds, east positive. a has 13 words. */
#define QRX_NET_TIME 20u
/* a[1] guest address, a[2] length: new network settings (key = value lines, see qrxnetd); the
 * engine saves them and starts or stops Tor or WireGuard. */
#define QRX_NET_CONFIG 21u
/* a[10] 0 direct, 1 Tor, 2 WireGuard; a[11] 1 when the guest can reach the network */
#define QRX_NET_STATUS 22u
/* IPv6 (and any family) with the address in guest memory: a door address record of 24 bytes,
 * big endian like the guest: 16 bytes address (an IPv4 address as ::ffff:a.b.c.d), u32 port,
 * u32 scope id. The IPv4 operations above stay as they are.
 *   NET_SOCKET6    a[0] 1 stream, 2 datagram: an IPv6 socket (dual stack: v4-mapped works too)
 *   NET_BIND6      a[0] socket, a[1] record      NET_CONNECT6  a[0] socket, a[1] record
 *   NET_SENDTO6    as NET_SENDTO, a[4] record    NET_RECVFROM6 as NET_RECVFROM, a[4] record out
 *   NET_ACCEPT6    a[0] socket, a[1] record out  NET_SOCKNAME6/NET_PEERNAME6 a[0], a[1] record out
 *   NET_RESOLVE6   a[1] name, a[2] output, a[3] records at most: a[8] the number of address
 *                  records written, IPv6 and IPv4 (mapped) as the host's getaddrinfo gives them
 * Through Tor or WireGuard an IPv6 address other than ::1 does not leave (ENETUNREACH), names
 * resolve to IPv4 through the tunnel: nothing goes past it. */
#define QRX_NET_SOCKET6 23u
#define QRX_NET_BIND6 24u
#define QRX_NET_CONNECT6 25u
#define QRX_NET_SENDTO6 26u
#define QRX_NET_RECVFROM6 27u
#define QRX_NET_ACCEPT6 28u
#define QRX_NET_SOCKNAME6 29u
#define QRX_NET_PEERNAME6 30u
#define QRX_NET_RESOLVE6 31u
#define QRX_NET_ADDR6_SIZE 24u
void qrx_hostnet_call(uint32_t op, uint32_t *a, uint8_t *ram);
#endif
