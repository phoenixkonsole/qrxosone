/* Declarations the AROS SDK lacks, implemented in tools/blender/port. Included in every file. */
#ifndef QRX_EXTRA_H
#define QRX_EXTRA_H
#ifdef __cplusplus
/* aros/cpu.h names the registers, and Eigen has variables called A0 and so on. No C++ here uses AROS's
 * register macros. */
#include <aros/cpu.h>
#undef A0
#undef A1
#undef A2
#undef A3
#undef A4
#undef A5
#undef A6
#undef A7
#undef D0
#undef D1
#undef D2
#undef D3
#undef D4
#undef D5
#undef D6
#undef D7
#endif
#if defined(__cplusplus) && __cplusplus >= 201103L
typedef struct { long long a; long double b; } max_align_t;
#endif
#ifdef __cplusplus
extern "C" {
#endif
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
char *mkdtemp(char *tmpl);
#ifdef __cplusplus
}
#endif
#endif
