/* AROS gives the POSIX minimum, 32767, as SSIZE_MAX, which tor reads as its allocation limit. */
#ifndef QRX_LIMITS_H
#define QRX_LIMITS_H
#include_next <limits.h>
#undef SSIZE_MAX
#define SSIZE_MAX 2147483647
#endif
