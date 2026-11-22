/* Second 68100 core: starting jobs and locks that work on both cores. */
#include <stdint.h>
#include <stdlib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include "qrx_core1.h"

#define SMP_BASE ((uintptr_t)0xff000600u)
#define SMP_MAGIC 0x51525844u
static inline uint32_t smp_rd(unsigned off) { return *(volatile const uint32_t *)(SMP_BASE + off); }
static inline void smp_wr(unsigned off, uint32_t v) { *(volatile uint32_t *)(SMP_BASE + off) = v; }

#define CORE1_STACK (16u * 1024u * 1024u)
static char *core1_stack;
static int core1_state = -1;
static void (*volatile core1_fn)(void *);
static void *volatile core1_arg;

int qrx_core1_available(void)
{
	if (core1_state < 0) {
		core1_state = 0;
		/* The registers exist on ARMithlon only. FS-UAE and real machines have no such board:
		 * only Exec knows, and the QRX distribution runs on ARMithlon, so ask for it explicitly. */
		if (!getenv("QRX_NO_CORE1") && smp_rd(0) == SMP_MAGIC && (smp_rd(8) & (1u << 4))) core1_state = 1;
	}
	return core1_state;
}

static void core1_entry(void)
{
	core1_fn(core1_arg);
	smp_wr(0x18, 2);  /* park */
	for (;;) {}
}

int qrx_core1_start(void (*fn)(void *), void *arg)
{
	if (!qrx_core1_available() || qrx_core1_busy()) return 0;
	if (!core1_stack) {
		core1_stack = AllocMem(CORE1_STACK, MEMF_PUBLIC);
		if (!core1_stack) return 0;
	}
	core1_fn = fn;
	core1_arg = arg;
	/* A fresh code cache: the program was loaded after core 1's last job at most. */
	smp_wr(0x10, (uint32_t)(uintptr_t)core1_entry);
	smp_wr(0x14, (uint32_t)(uintptr_t)core1_stack + CORE1_STACK);
	smp_wr(0x18, 1);
	return 1;
}

static void __attribute__((destructor)) core1_exit(void)
{
	/* The stack stays allocated if core 1 is still running on it. */
	if (core1_stack && !(smp_rd(0x0C) & 2u)) {
		FreeMem(core1_stack, CORE1_STACK);
		core1_stack = NULL;
	}
}

int qrx_core1_busy(void) { return core1_state == 1 && (smp_rd(0x0C) & 2u) != 0; }
int qrx_core1_faulted(unsigned *pc, unsigned *addr)
{
	if (core1_state != 1 || !(smp_rd(0x0C) & 4u)) return 0;
	if (pc) *pc = smp_rd(0x38);
	if (addr) *addr = smp_rd(0x3C);
	return 1;
}
unsigned qrx_core1_instructions(void) { return core1_state == 1 ? smp_rd(0x28) : 0; }

int qrx_on_core1(void)
{
	char *sp;
	__asm__ volatile("move.l %%sp,%0" : "=r"(sp));
	return core1_stack && sp >= core1_stack && sp < core1_stack + CORE1_STACK;
}

static inline int tas(qrx_lock_t *l)
{
	char was;
	__asm__ volatile("tas %0\n\tsmi %1" : "+m"(*l), "=d"(was) : : "cc");
	return was;
}
void qrx_yield(void)
{
	if (qrx_on_core1()) return;
	/* A task at the same priority gets its turn; Delay would wait for a whole tick. */
	struct Task *me = FindTask(NULL);
	SetTaskPri(me, me->tc_Node.ln_Pri);
}
void qrx_lock(qrx_lock_t *l)
{
	unsigned tries = 0;
	while (tas(l)) {
		while (*l) {
			if (++tries > 200) { qrx_yield(); tries = 0; }
		}
	}
}
int qrx_trylock(qrx_lock_t *l) { return !tas(l); }
void qrx_unlock(qrx_lock_t *l) { *l = 0; }
