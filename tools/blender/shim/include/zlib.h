/* The SDK's zconf.h defines near and far empty; Blender has struct members with those names. */
#ifndef QRX_ZLIB_H
#define QRX_ZLIB_H
#include_next <zlib.h>
#undef near
#undef far
#endif
