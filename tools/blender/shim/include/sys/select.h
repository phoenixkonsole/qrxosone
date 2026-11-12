/* The SDK's sys/select.h has fd_set but no select(): that one comes from port/qrx_socket.c. */
#ifndef QRX_SYS_SELECT_H
#define QRX_SYS_SELECT_H
#include_next <sys/select.h>
#include <sys/time.h>
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv);
#endif
