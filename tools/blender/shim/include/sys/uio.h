#ifndef QRX_UIO_H
#define QRX_UIO_H
#include <sys/types.h>
struct iovec { void *iov_base; size_t iov_len; };
ssize_t writev(int fd, const struct iovec *iov, int count);
#endif
