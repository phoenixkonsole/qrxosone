#ifndef QRX_NETPOLICY_H
#define QRX_NETPOLICY_H
/* The guest's network through Tor or WireGuard (qrx_netpolicy.c, qrxnetd). With either on, a guest
 * connection never leaves the Mac directly: no tunnel, no connection. */
#include <stddef.h>
#include <stdint.h>

/* 0 direct, 1 Tor, 2 WireGuard, 3 offline (from the saved settings) */
int qrx_net_mode(void);
/* direct, or the tunnel is up */
int qrx_net_ready(void);
/* new settings from the guest (key = value lines); restarts qrxnetd */
int qrx_net_configure(const char *text, size_t len);
/* A Unix socket to qrxnetd with a SOCKS5 CONNECT to ip:port already sent; -1 and errno without */
int qrx_net_socks_connect(uint32_t ip, uint16_t port);
/* The greeting answer and the reply together: QRX_SOCKS_REPLY bytes. 0 not complete yet, 1 connected,
 * else -errno */
#define QRX_SOCKS_REPLY 12
int qrx_net_socks_reply(const uint8_t *r, size_t have);
/* a name through the tunnel; -1 when it cannot be found or there is no tunnel */
int qrx_net_resolve(const char *name, uint32_t *ip);
#endif
