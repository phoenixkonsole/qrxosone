/* Second 68100 core and core-independent locks for Blender on QRX OS One.
 * Core 1 reaches shared RAM and its registers only: no ROM, no AROS libraries, no interrupts.
 * See docs/BLENDER_AROS68K.md and upstream/armithlon/2.6/engine/guest/qrx68100_smp.h. */
#ifndef QRX_CORE1_H
#define QRX_CORE1_H
#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the machine has a parallel core 1 and it was not turned off (QRX_NO_CORE1). */
int qrx_core1_available(void);
/* Starts fn(arg) on core 1. Returns 0 if it is not available or already running. */
int qrx_core1_start(void (*fn)(void *), void *arg);
/* 1 while the job runs. */
int qrx_core1_busy(void);
/* 1 if core 1 stopped on a fault; the place is written to pc and addr. */
int qrx_core1_faulted(unsigned *pc, unsigned *addr);
/* Instructions core 1 ran in its current or last job. */
unsigned qrx_core1_instructions(void);
/* Is the caller running on core 1? Works from its stack pointer, so it costs little. */
int qrx_on_core1(void);

typedef volatile unsigned char qrx_lock_t;
/* Usable on both cores. On core 0 a waiter yields to other tasks, because the holder may be one. */
void qrx_lock(qrx_lock_t *l);
int qrx_trylock(qrx_lock_t *l);
void qrx_unlock(qrx_lock_t *l);
/* Lets core 0 give other AROS tasks a turn. */
void qrx_yield(void);

/* Printing to the engine's log, for core 0 only. */
void qrx_log(const char *s);
int qrx_printf(const char *fmt, ...);
#ifdef __cplusplus
}
#endif
#endif
