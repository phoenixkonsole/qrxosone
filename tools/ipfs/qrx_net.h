/*
 * qrx_net.h - 68K host-socket transport for QRX IPFS.
 *
 * ARMithlon does not ship an AROS SANA-II/bsdsocket stack.  The engine
 * already owns a host socket service for QRX86; this header reaches it
 * through the existing QRXHostFS MMIO door.  Sockets, DNS and TCP live on
 * the host, while all IPFS/blockstore/libp2p logic still runs as 68k code.
 */
#ifndef QRX_NET_H
#define QRX_NET_H

#include <exec/types.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define QRX_NET_HFS_BASE   ((uintptr_t)0xff000800UL)
#define QRX_NET_HFS_MAGIC  0x51525846u
#define QRX_NET_HFS_FEAT   0x04u
#define QRX_NET_HFS_DATA   0x0cu
#define QRX_NET_HFS_LENGTH 0x14u
#define QRX_NET_HFS_COMMAND 0x18u
#define QRX_NET_HFS_STATUS 0x1cu
#define QRX_NET_HFS_OK     0u
#define QRX_NET_HFS_NET    0x200u
#define QRX_NET_WORDS      13u

#define QRX_NET_OP_SOCKET   1
#define QRX_NET_OP_BIND     2
#define QRX_NET_OP_LISTEN   3
#define QRX_NET_OP_ACCEPT   4
#define QRX_NET_OP_CONNECT  5
#define QRX_NET_OP_SEND     6
#define QRX_NET_OP_RECV     7
#define QRX_NET_OP_POLL     10
#define QRX_NET_OP_CLOSE    11
#define QRX_NET_OP_GETOPT   14
#define QRX_NET_OP_RESOLVE  16

#define QRX_NET_EWOULDBLOCK 10035
#define QRX_NET_EINPROGRESS 10036
#define QRX_NET_EALREADY    10037

int qrx_net_init(void);
int qrx_net_available(void);
int qrx_net_last_error(void);

int qrx_net_socket(int type);
int qrx_net_bind(int fd, const struct sockaddr_in *sa);
int qrx_net_listen(int fd, int backlog);
int qrx_net_accept(int fd, struct sockaddr_in *sa, socklen_t *len);
int qrx_net_accept_nb(int fd, struct sockaddr_in *sa);
int qrx_net_connect(int fd, const struct sockaddr_in *sa);
long qrx_net_send(int fd, const void *data, unsigned long len, int flags);
long qrx_net_recv(int fd, void *data, unsigned long len, int flags);
int qrx_net_poll(int fd, int mask, unsigned *avail);
int qrx_net_close(int fd);
int qrx_net_resolve(const char *host, uint32_t *ip);
struct hostent *qrx_net_gethostbyname(const char *host);
unsigned long qrx_net_inet_addr(const char *text);

/* Drop-in names for the existing IPFS sources.  netdb.h pulls in AROS's
 * proto/bsdsocket.h; drop those library macros before installing the
 * host-door versions, so no SocketBase is required. */
#undef socket
#undef bind
#undef listen
#undef accept
#undef connect
#undef send
#undef recv
#undef CloseSocket
#undef setsockopt
#undef gethostbyname
#undef inet_addr
#define socket(domain, type, protocol) qrx_net_socket((type))
#define bind(fd, sa, len) qrx_net_bind((fd), (const struct sockaddr_in *)(sa))
#define listen(fd, backlog) qrx_net_listen((fd), (backlog))
#define accept(fd, sa, len) qrx_net_accept((fd), (struct sockaddr_in *)(sa), (socklen_t *)(len))
#define connect(fd, sa, len) qrx_net_connect((fd), (const struct sockaddr_in *)(sa))
#define send(fd, data, len, flags) qrx_net_send((fd), (data), (unsigned long)(len), (flags))
#define recv(fd, data, len, flags) qrx_net_recv((fd), (data), (unsigned long)(len), (flags))
#define CloseSocket(fd) qrx_net_close(fd)
#define setsockopt(fd, level, name, val, len) (0)
#define gethostbyname(name) qrx_net_gethostbyname((const char *)(name))
#define inet_addr(text) qrx_net_inet_addr((const char *)(text))

#endif /* QRX_NET_H */
