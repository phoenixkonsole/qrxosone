/* No dynamic loading on QRX OS One: Blender's add-on libraries are not part of this build. */
#ifndef QRX_DLFCN_H
#define QRX_DLFCN_H
#define RTLD_LAZY 1
#define RTLD_GLOBAL 2
static inline void *dlopen(const char *n, int f) { (void)n; (void)f; return (void *)0; }
static inline void *dlsym(void *h, const char *n) { (void)h; (void)n; return (void *)0; }
static inline int dlclose(void *h) { (void)h; return 0; }
static inline const char *dlerror(void) { return "no dynamic libraries"; }
#endif
