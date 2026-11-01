#ifndef QRX_APP_H
#define QRX_APP_H
#include <stddef.h>
typedef enum {
    QRX_APP_INVALID=0, QRX_APP_PE32_I386, QRX_APP_ELF32_I386,
    QRX_APP_ELF68K, QRX_APP_QEX1, QRX_APP_HUNK, QRX_APP_UNSUPPORTED
} QRXAppKind;
QRXAppKind qrx_app_inspect(const char *path);
const char *qrx_app_name(QRXAppKind kind);
/* Build a bounded executable argument list without ever invoking a shell.
 * Explicitly selected host programs only: no automatic untrusted guest call.
 * Runtime must be an existing host-executable path selected by operator. */
int qrx_app_plan(QRXAppKind kind,const char *win_runtime,const char *linux_runtime,
                 const char *guest_path,const char **out,size_t capacity);
#endif
