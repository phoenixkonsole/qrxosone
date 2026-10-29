/* QRX OS One 22.5.12 - run main() on a large private stack on AROS.
 * Shell commands start with a few KiB of stack; the x86 interpreters nest
 * guest callbacks (window procedures inside API calls), so they switch to a
 * 1 MiB stack with exec.library NewStackSwap().  Elsewhere this is a no-op. */
#ifndef QRX_BIGSTACK_H
#define QRX_BIGSTACK_H
#ifdef __AROS__
#include <exec/tasks.h>
#include <exec/memory.h>
#include <proto/exec.h>
#define QRX_BIGSTACK_SIZE (1024u * 1024u)
#define QRX_BIGSTACK_MAIN(real_main)                                              \
    static IPTR qrx_bigstack_entry(IPTR ac, IPTR av)                              \
    { return (IPTR)real_main((int)ac, (char **)av); }                             \
    int main(int argc, char **argv)                                               \
    {                                                                             \
        struct StackSwapStruct sss;                                               \
        struct StackSwapArgs ssa;                                                 \
        IPTR rc;                                                                  \
        APTR stk = AllocMem(QRX_BIGSTACK_SIZE, MEMF_ANY);                         \
        if (!stk) return real_main(argc, argv);                                   \
        sss.stk_Lower = stk;                                                      \
        sss.stk_Upper = (APTR)((IPTR)stk + QRX_BIGSTACK_SIZE);                            \
        sss.stk_Pointer = sss.stk_Upper;                                    \
        memset(&ssa, 0, sizeof(ssa));                                             \
        ssa.Args[0] = (IPTR)argc; ssa.Args[1] = (IPTR)argv;                       \
        rc = NewStackSwap(&sss, (LONG_FUNC)qrx_bigstack_entry, &ssa);             \
        FreeMem(stk, QRX_BIGSTACK_SIZE);                                          \
        return (int)rc;                                                           \
    }
#else
#define QRX_BIGSTACK_MAIN(real_main) int main(int argc, char **argv) { return real_main(argc, argv); }
#endif
#endif
