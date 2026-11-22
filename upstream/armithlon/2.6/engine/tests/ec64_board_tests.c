/* The ARMithlon 68100EC64 board: its registers in the device window from long-mode code, a bus
 * fault for an unknown register, and the machine's console queue and sink. */
#include "qrx68100ec64.h"
#include "qrx_ec64_board.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE (UINT64_C(64) << 20)
#define CODE UINT64_C(0x10000)
static unsigned failures;
#define CHECK(x, ...) do { if (!(x)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); failures++; } } while (0)
#define Q64(v) (uint16_t)((uint64_t)(v) >> 48), (uint16_t)((uint64_t)(v) >> 32), (uint16_t)((uint64_t)(v) >> 16), (uint16_t)(v)
#define REG(o) (QEC64_MMIO_BASE + (o))

static uint8_t *ram;
static void put(const uint16_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++) { ram[CODE + i * 2] = (uint8_t)(w[i] >> 8); ram[CODE + i * 2 + 1] = (uint8_t)w[i]; }
    ram[CODE + n * 2] = 0x4A; ram[CODE + n * 2 + 1] = 0xFC;     /* ILLEGAL: the end */
}

static char sunk[64];
static size_t nsunk;
static void sink(void *ctx, unsigned ch) { (void)ctx; if (nsunk < sizeof sunk) sunk[nsunk++] = (char)ch; }

int main(void)
{
    ram = calloc(1, RAM_SIZE);
    qec64_machine *m = qec64_machine_new(ram, RAM_SIZE, 1);
    qec64_board board;
    qec64_board_init(&board, ram, RAM_SIZE);
    qec64_machine_set_mmio(m, qec64_board_mmio, &board);
    qec64_cpu *c = qec64_machine_core(m, 0);

    /* movea.q #ID,a0; move.q (a0),d0; movea.q #FB_WIDTH,a1; move.l #640,(a1); move.l #480,(4,a1);
       move.l #2560,(8,a1); move.l #5,(12,a1); move.l #1,(16,a1); move.q #$123000,d1; movea.q #FB_ADDR,a2;
       move.q d1,(a2); movea.q #CLOCK_UNIX,a3; move.q (a3),d2 */
    static const uint16_t prog[] = {
        0xA008, 0x207C, Q64(REG(QEC64_BOARD_ID)), 0xA008, 0x2010,
        0xA008, 0x227C, Q64(REG(QEC64_FB_WIDTH)), 0x22BC, 0x0000, 0x0280,
        0x237C, 0x0000, 0x01E0, 0x0004, 0x237C, 0x0000, 0x0A00, 0x0008,
        0x237C, 0x0000, 0x0005, 0x000C, 0x237C, 0x0000, 0x0001, 0x0010,
        0xA008, 0x223C, Q64(0x123000), 0xA008, 0x247C, Q64(REG(QEC64_FB_ADDR)), 0xA008, 0x2481,
        0xA008, 0x267C, Q64(REG(QEC64_CLOCK_UNIX)), 0xA008, 0x2413,
    };
    put(prog, sizeof prog / sizeof prog[0]);
    c->m64 = true;
    c->pc = CODE;
    c->a[7] = 0x8000;
    enum qec64_status st = qec64_run(c, 1000);
    CHECK(st == QEC64_STOPPED, "status %d, pc %" PRIx64, st, c->pc);
    CHECK(c->d[0] == QEC64_BOARD_MAGIC, "ID %016" PRIx64, c->d[0]);
    CHECK(c->d[2] > 1700000000u, "clock %" PRIu64, c->d[2]);
    const uint8_t *px; uint32_t w, h, stride, fmt;
    CHECK(qec64_board_framebuffer(&board, &px, &w, &h, &stride, &fmt) && w == 640 && h == 480 && stride == 2560 &&
          fmt == QEC64_FB_FMT_XRGB32 && px == ram + 0x123000, "framebuffer %u x %u stride %u format %u", w, h, stride, fmt);

    /* An unknown register and a wrong size are bus faults */
    static const uint16_t bad[] = { 0xA008, 0x207C, Q64(REG(0x0F00)), 0x2010 };
    put(bad, sizeof bad / sizeof bad[0]);
    c->pc = CODE;
    st = qec64_run(c, 100);
    CHECK(st == QEC64_BUS_FAULT && c->fault_addr == REG(0x0F00), "status %d fault %016" PRIx64, st, c->fault_addr);
    static const uint16_t badsize[] = { 0xA008, 0x207C, Q64(REG(QEC64_BOARD_ID)), 0x2010 };
    put(badsize, sizeof badsize / sizeof badsize[0]);
    c->pc = CODE;
    st = qec64_run(c, 100);
    CHECK(st == QEC64_BUS_FAULT, ".l on the .q ID register: status %d", st);

    /* Input events from the window, in order */
    qec64_board_push_event(&board, 0x01230000u);
    qec64_board_push_event(&board, 0x02230000u);
    static const uint16_t ev[] = { 0xA008, 0x207C, Q64(REG(QEC64_INPUT_EVENT)), 0x2010, 0x2210, 0x2410 };
    put(ev, sizeof ev / sizeof ev[0]);
    c->pc = CODE;
    st = qec64_run(c, 100);
    CHECK(st == QEC64_STOPPED && (uint32_t)c->d[0] == 0x01230000u && (uint32_t)c->d[1] == 0x02230000u && (uint32_t)c->d[2] == 0,
          "events %08x %08x %08x", (unsigned)c->d[0], (unsigned)c->d[1], (unsigned)c->d[2]);

    /* Console: the fixed input first, then what was pushed; output goes to the sink */
    qec64_machine_console_input(m, "ab", 2);
    CHECK(qec64_machine_console_push(m, "cd", 2) == 2, "push");
    char got[6] = { 0 };
    for (int i = 0; i < 5; i++) got[i] = (char)c->on_console_in(c);
    CHECK(!memcmp(got, "abcd", 4) && got[4] == (char)-1, "input '%.4s' then %d", got, got[4]);
    qec64_machine_console_sink(m, sink, NULL);
    c->on_console(c, 'Q');
    CHECK(nsunk == 1 && sunk[0] == 'Q', "sink");

    qec64_machine_free(m);
    free(ram);
    printf("%u failures\n", failures);
    return failures != 0;
}
