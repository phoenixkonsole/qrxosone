/* In place of AROS's netdb.h, which is tied to bsdsocket.library. Lookups: see qrx_posix.c. */
#ifndef QRX_NETDB_H
#define QRX_NETDB_H
#include <sys/types.h>
#include <sys/socket.h>
struct hostent { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; };
#define h_addr h_addr_list[0]
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3
#define NO_DATA 4
#define NO_ADDRESS NO_DATA
struct servent { char *s_name; char **s_aliases; int s_port; char *s_proto; };
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);
struct protoent { char *p_name; char **p_aliases; int p_proto; };
struct protoent *getprotobyname(const char *name);
extern int h_errno;
struct hostent *gethostbyname(const char *name);
struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type);
int gethostname(char *name, size_t len);
#endif
