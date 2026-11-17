#ifndef QRX68100EC64_H
#define QRX68100EC64_H
/* 68100EC64: the 68100 with 64-bit registers, addresses and operations. See
 * docs/armithlon/68100ec64.md for the instruction set. This is the reference interpreter:
 * no globals, one qec64_cpu per core, so any number of host threads can share one RAM. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum qec64_status {
    QEC64_RUNNING = 0,   /* the instruction budget ran out */
    QEC64_STOPPED,       /* STOP or the ILLEGAL instruction $4AFC */
    QEC64_BUS_FAULT,     /* an access outside RAM; fault_addr has the address */
    QEC64_UNIMPLEMENTED, /* an instruction this core does not have; fault_pc has its place */
    QEC64_DIVIDE_ZERO,
    QEC64_TRAP,          /* TRAP #n; trap_number has n */
    QEC64_PRIVILEGE,     /* a privileged instruction in user state */
    QEC64_CHK,           /* CHK or CHK2 out of bounds */
    QEC64_TRAPV,         /* TRAPV or TRAPcc taken */
    QEC64_WAITING,       /* after STOP: nothing to do until qec64_raise */
    QEC64_HALTED,        /* the program ended the machine; exit_code has its value */
    QEC64_DOUBLE_FAULT   /* no stack or no vector for an exception; fault_addr has the stack pointer */
};

/* Every status except RUNNING, WAITING, HALTED and DOUBLE_FAULT becomes an exception once the
 * program has written VBR in long mode. Until then the core stops and reports it. */

/* The device window: the top 4 GB of the 64-bit address space */
#define QEC64_MMIO_BASE UINT64_C(0xFFFFFFFF00000000)

typedef struct qec64_cpu qec64_cpu;
struct qec64_cpu {
    uint64_t d[16], a[16];   /* a[7] is the stack pointer */
    uint64_t pc;
    uint8_t ccr;             /* X N Z V C, as on the 68000 */
    bool m64;                /* long mode; off, the core is a 32-bit integer 68040 */
    uint32_t core_id;
    uint8_t *ram;
    uint64_t ram_size;
    enum qec64_status status;
    uint64_t fault_pc, fault_addr;
    uint32_t trap_number;
    uint64_t instructions;
    uint64_t budget;
    bool supervisor;
    uint8_t ipl;             /* interrupts at or below this level wait, level 7 always comes */
    bool waiting;            /* after STOP */
    uint64_t other_sp;       /* the stack pointer of the state not in use */
    uint64_t vbr;
    bool vbr_set;
    uint32_t pending;        /* one bit per interrupt level, set from any thread by qec64_raise */
    uint32_t num_cores;
    uint64_t timer_us;       /* period of the timer interrupt, 0 is off */
    uint64_t exit_code;
    uint64_t start_sp;       /* the stack for the next core this one starts */
    bool started;
    /* From MOVEC. The machine that owns this core sets them, see qrx68100ec64_machine.c. */
    void (*on_ipi)(qec64_cpu *self, uint64_t core_mask);
    void (*on_timer)(qec64_cpu *self);
    void (*on_console)(qec64_cpu *self, unsigned ch);
    int (*on_console_in)(qec64_cpu *self);   /* the next input character, or -1 */
    /* Accesses at QEC64_MMIO_BASE and above, the machine's devices: true when handled, else a
     * bus fault. In long mode only. */
    bool (*on_mmio)(qec64_cpu *self, uint64_t addr, unsigned size, bool write, uint64_t *value);
    void *machine;
    /* JIT, see qrx68100ec64_jit.c: the core's own state, and the machine's map of the RAM that
     * holds translated code (one byte per 256 bytes, QEC64_CODE_SHIFT) with a generation that
     * every write into such RAM bumps. NULL code_map: no JIT on this machine. */
    void *jit;
    uint8_t *code_map;
    uint32_t *code_gen;
};
#define QEC64_CODE_SHIFT 8

/* QRX_EC64_JIT_VERIFY=1 (qrx68100ec64_jit.c): the interpreter's RAM writes with their old contents,
 * so a block can run in both and the two results be compared. Per thread; NULL is off. Device
 * accesses, CAS and TAS count as side effects: such a block is not run twice. */
typedef struct { uint64_t addr; uint8_t n; uint8_t old[8]; } qec64_undo;
extern __thread qec64_undo *qec64_undo_log;
extern __thread unsigned qec64_undo_n, qec64_undo_cap, qec64_side_effects;

/* Like qec64_run, but long mode code runs translated to AArch64 where the JIT can. Falls back to
 * qec64_run without code_map, outside long mode and on other hosts. */
enum qec64_status qec64_jit_run(qec64_cpu *c, uint64_t max);
/* Frees the core's translations. */
void qec64_jit_free(qec64_cpu *c);
/* Allocates code_map and code_gen for RAM of ram_size bytes; free both with free(). */
bool qec64_jit_map_new(uint64_t ram_size, uint8_t **code_map, uint32_t **code_gen);

void qec64_init(qec64_cpu *c, uint8_t *ram, uint64_t ram_size, uint32_t core_id);
/* Runs up to max instructions and returns why it ended. */
enum qec64_status qec64_run(qec64_cpu *c, uint64_t max);
/* Sets an interrupt level pending, from any thread. */
void qec64_raise(qec64_cpu *c, unsigned level);

/* A machine: cores on host threads sharing RAM, see docs/armithlon/68100ec64.md. */
#define QEC64_MAX_CORES 8
typedef struct qec64_machine qec64_machine;
qec64_machine *qec64_machine_new(uint8_t *ram, uint64_t ram_size, unsigned cores);
void qec64_machine_free(qec64_machine *m);
qec64_cpu *qec64_machine_core(qec64_machine *m, unsigned i);
/* Console output goes to out, up to cap bytes. */
void qec64_machine_console(qec64_machine *m, char *out, size_t cap);
size_t qec64_machine_console_used(const qec64_machine *m);
/* Console input: reads of MOVEC $8F0 return these bytes in turn, then -1. */
void qec64_machine_console_input(qec64_machine *m, const char *in, size_t len);
/* Console output to fn as it comes, from the core's thread under the machine's lock. */
void qec64_machine_console_sink(qec64_machine *m, void (*fn)(void *ctx, unsigned ch), void *ctx);
/* More console input while the machine runs (after the bytes given above); returns how many fit. */
size_t qec64_machine_console_push(qec64_machine *m, const char *s, size_t n);
/* The devices at QEC64_MMIO_BASE for every core; qec64_machine_user() gives back user. */
void qec64_machine_set_mmio(qec64_machine *m, bool (*fn)(qec64_cpu *, uint64_t, unsigned, bool, uint64_t *), void *user);
void *qec64_machine_user(const qec64_cpu *c);
/* From a device on core c's thread: sleep until an interrupt is pending for c, its timer is due,
 * or max_us passed. Returns the microseconds slept. The idle task's way to stop spinning. */
uint64_t qec64_machine_idle(qec64_cpu *c, uint64_t max_us);
/* Ends qec64_machine_run() from another thread, as the time limit would. */
void qec64_machine_stop(qec64_machine *m);
/* Starts core 0 at pc, in 32-bit mode as after reset, and waits for the end: a halt, or a failure on
 * any core, or the time limit. Returns the status of the core that ended it, or QEC64_RUNNING. */
enum qec64_status qec64_machine_run(qec64_machine *m, uint64_t pc, uint64_t sp, double seconds, unsigned *which);
#endif
