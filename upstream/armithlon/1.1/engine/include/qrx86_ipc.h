#ifndef QRX86_IPC_H
#define QRX86_IPC_H
/* ARMITHLON 0.7 experimental HOST process transport for QRX86's native
 * Win32 2D HLE output. Unix-domain sockets: Linux + macOS, not Amiga 68k.
 * Zero native pointers or untrusted shaders cross the boundary. */
#include "qrx_accel.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
bool qrx86_ipc_send(const char *socket_path,const uint32_t *xrgb,
                    uint32_t width,uint32_t height);
bool qrx86_ipc_read_one(int fd,QRXAccelStream *out);
/* v0.9 Q86C: strictly bounded clear/triangle command frames. No guest pointers,
 * textures, shader blobs or handles cross this diagnostic socket. */
bool qrx86_ipc_send_commands(const char *socket_path,const QRXAccelFrame *frame);
/* Serve only when explicitly enabled by ARMITHLON_QRX86_SOCKET. Binding
 * requires the endpoint to be absent and the directory to be trusted. */
int qrx86_ipc_serve(const char *path,QRXAccelStream *stream,const atomic_bool *running);
#endif
