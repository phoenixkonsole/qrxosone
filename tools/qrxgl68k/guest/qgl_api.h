/* What a 68k program calls besides GL itself: contexts on the Mac, presenting a frame, and finding
 * functions by name (for GLEW). */
#ifndef QGL_API_H
#define QGL_API_H
#ifdef __cplusplus
extern "C" {
#endif
/* 1 when the engine has the GL door. */
int qgl_available(void);
/* A context on the host, sharing objects with share (0 for none, else an earlier context). 0 on failure. */
unsigned qgl_create_context(unsigned share);
void qgl_destroy_context(unsigned ctx);
/* Makes ctx current with a window of width x height; resizing keeps the context. ctx 0 releases. */
int qgl_make_current(unsigned ctx, int width, int height);
/* Copies the current context's frame into dst as ARGB, top row first. */
void qgl_read_frame(void *dst, int bytes_per_row, int width, int height);
void *qgl_GetProcAddress(const char *name);
#ifdef __cplusplus
}
#endif
#endif
