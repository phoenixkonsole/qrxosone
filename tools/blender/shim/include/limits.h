/* The SDK's limits.h ignores -funsigned-char, which Blender builds with. */
#ifndef QRX_LIMITS_H
#define QRX_LIMITS_H
#include_next <limits.h>
#ifdef __CHAR_UNSIGNED__
#undef CHAR_MAX
#undef CHAR_MIN
#define CHAR_MAX 255
#define CHAR_MIN 0
#endif
#endif
