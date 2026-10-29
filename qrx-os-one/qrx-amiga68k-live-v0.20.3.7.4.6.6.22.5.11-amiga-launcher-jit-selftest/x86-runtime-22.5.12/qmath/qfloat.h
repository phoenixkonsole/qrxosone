/* QRX OS One 22.5.12 - exact double <-> decimal conversion (see qfloat.c). */
#ifndef QFLOAT_H
#define QFLOAT_H
#include <stddef.h>
#define QF_LEFT  1
#define QF_PLUS  2
#define QF_SPACE 4
#define QF_ALT   8
#define QF_ZERO  16
/* conv: f F e E g G; prec < 0 -> 6; returns the full length (output truncated to outn-1) */
int qf_format(char *out, size_t outn, double v, int conv, int prec, int flags, int width);
double qf_strtod(const char *s, char **end);
#endif
