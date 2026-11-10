/* In place of AROS's arpa/inet.h, which calls bsdsocket.library. */
#ifndef QRX_ARPA_INET_H
#define QRX_ARPA_INET_H
#include <netinet/in.h>
char *inet_ntoa(struct in_addr a);
int inet_aton(const char *s, struct in_addr *a);
in_addr_t inet_addr(const char *s);
#endif
