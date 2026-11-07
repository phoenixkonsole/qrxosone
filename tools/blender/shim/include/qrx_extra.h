/* Declarations the AROS SDK lacks, implemented in tools/blender/port. Included in every file. */
#ifndef QRX_EXTRA_H
#define QRX_EXTRA_H
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
