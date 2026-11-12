/* AROS's definitions, with plain functions in place of bsdsocket.library calls. */
#ifndef QRX_SYS_SOCKET_H
#define QRX_SYS_SOCKET_H
#include_next <sys/socket.h>
#include <stddef.h>
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x20000000
#define SOCK_CLOEXEC 0x10000000
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x80
#endif
#include <sys/types.h>
int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *sa, socklen_t l);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *sa, socklen_t *l);
int connect(int fd, const struct sockaddr *sa, socklen_t l);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *sa, socklen_t l);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *sa, socklen_t *l);
int shutdown(int fd, int how);
int setsockopt(int fd, int level, int name, const void *val, socklen_t l);
int getsockopt(int fd, int level, int name, void *val, socklen_t *l);
int getsockname(int fd, struct sockaddr *sa, socklen_t *l);
int getpeername(int fd, struct sockaddr *sa, socklen_t *l);
int socketpair(int domain, int type, int protocol, int sv[2]);
#endif
