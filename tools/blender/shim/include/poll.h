/* On top of the shim's select (qrx_posix.c). */
#ifndef QRX_POLL_H
#define QRX_POLL_H
#define POLLIN 1
#define POLLPRI 2
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32
struct pollfd { int fd; short events, revents; };
typedef unsigned int nfds_t;
int poll(struct pollfd *p, nfds_t n, int timeout_ms);
#endif
