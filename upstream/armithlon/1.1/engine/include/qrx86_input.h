#ifndef QRX86_INPUT_H
#define QRX86_INPUT_H
/* QRX86 host-interpreter input transport, version 1.
 * Independent from guest pointers and the QRX86 private windowing structures.
 * One event per authenticated (filesystem permissions) local Unix connection.
 * Network-byte-order frame: magic, version, type, code, character, x, y, button.
 */
#include <stdint.h>
#include <stdbool.h>
#define QRX86_INPUT_KEY_DOWN   1u
#define QRX86_INPUT_KEY_UP     2u
#define QRX86_INPUT_MOUSE_MOVE 3u
#define QRX86_INPUT_MOUSE_DOWN 4u
#define QRX86_INPUT_MOUSE_UP   5u
#define QRX86_INPUT_MAGIC UINT32_C(0x51383649) /* 'Q86I' */
#define QRX86_INPUT_VERSION 1u
typedef struct QRX86InputEvent {
    uint32_t type, code, character, x, y, button;
} QRX86InputEvent;
/* The listener is owned by the native QRX86 host process. Never overwrite
 * an existing socket; close() also removes only its own pathname. */
typedef struct QRX86InputListener {
    int fd;
    char path[104];
} QRX86InputListener;
bool qrx86_input_listen(QRX86InputListener *listener,const char *path);
/* Returns 1 for an input event, 0 on timeout/invalid packet, -1 on failure.
 * 'timeout_ms' is capped to 250ms to keep UI shutdown responsive. */
int qrx86_input_wait(QRX86InputListener *listener,QRX86InputEvent *out,uint32_t timeout_ms);
void qrx86_input_close(QRX86InputListener *listener);
bool qrx86_input_send(const char *path,const QRX86InputEvent *event);
#endif
