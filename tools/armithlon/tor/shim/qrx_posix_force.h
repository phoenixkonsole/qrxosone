/* Forced into every file of the tor port, before any system header. */
#ifndef QRX_POSIX_FORCE_H
#define QRX_POSIX_FORCE_H
/* Socket descriptors start at 32 (see qrx_posix.c), and the door has 64 of them. */
#define FD_SETSIZE 128
#define O_NOFOLLOW 0
/* Big endian. */
#define htole16(x) __builtin_bswap16(x)
#define le16toh(x) __builtin_bswap16(x)
#define htole32(x) __builtin_bswap32(x)
#define le32toh(x) __builtin_bswap32(x)
#define htole64(x) __builtin_bswap64(x)
#define le64toh(x) __builtin_bswap64(x)
#define htobe16(x) (x)
#define be16toh(x) (x)
#define htobe32(x) (x)
#define be32toh(x) (x)
#define htobe64(x) (x)
#define be64toh(x) (x)
int getentropy(void *buf, __SIZE_TYPE__ n);
#ifdef TOR_UNIT_TESTS
/* AROS defines STATIC as static; tor's test build needs its own, empty one to hold. */
#include <exec/types.h>
#undef STATIC
#endif
#endif
