/* tor uses glob for %include in its configuration only. Patterns match nothing here. */
#ifndef QRX_GLOB_H
#define QRX_GLOB_H
#include <stddef.h>
typedef struct { size_t gl_pathc; char **gl_pathv; size_t gl_offs; } glob_t;
#define GLOB_ERR 1
#define GLOB_MARK 2
#define GLOB_NOSORT 4
#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3
int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g);
void globfree(glob_t *g);
#endif
