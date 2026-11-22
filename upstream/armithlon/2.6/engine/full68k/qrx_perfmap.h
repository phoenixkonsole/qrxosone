/* Linux perf's map of JIT code: with QRX_PERF_MAP=1 each translated block is written to
 * /tmp/perf-<pid>.map as "start size name", so `perf record` / `perf report` name the hot blocks
 * by their guest address (68k / 68100EC64 / x86) instead of showing bare addresses.
 * A code arena that is flushed and reused keeps the old lines; perf takes the newest one. */
#ifndef QRX_PERFMAP_H
#define QRX_PERFMAP_H
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static inline FILE *qrx_perfmap_file(void)
{
    static FILE *f;
    static int state;                       /* 0 not looked at yet, 1 on, 2 off */
    if (state == 0) {
        const char *e = getenv("QRX_PERF_MAP");
        state = 2;
        if (e && *e && *e != '0') {
            char path[64];
            snprintf(path, sizeof path, "/tmp/perf-%d.map", (int)getpid());
            f = fopen(path, "a");
            if (f) { setvbuf(f, NULL, _IOLBF, 0); state = 1; }
        }
    }
    return state == 1 ? f : NULL;
}

__attribute__((format(printf, 3, 4)))
static inline void qrx_perfmap(const void *code, size_t bytes, const char *fmt, ...)
{
    FILE *f = qrx_perfmap_file();
    if (!f || !bytes) return;
    char name[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof name, fmt, ap);
    va_end(ap);
    fprintf(f, "%lx %zx %s\n", (unsigned long)(size_t)code, bytes, name);
}
#endif
