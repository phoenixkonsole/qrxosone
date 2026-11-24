/* _Unwind_DeleteException for the Linux cross build: Homebrew's bare-metal m68k-elf libgcc has it,
 * m68k-linux-gnu keeps it in libgcc_eh.a together with the glibc-only frame lookup
 * (_dl_find_object). libsupc++ refers to it when an exception object is freed; the build has no
 * exceptions (-fno-exceptions, a throw ends the program), so this is the whole of what it does,
 * as in libgcc's unwind.inc: hand the object to its own cleanup function. */
#include <stddef.h>

struct qrx_unwind_exception {
    unsigned long long exception_class;
    void (*exception_cleanup)(int reason, struct qrx_unwind_exception *exc);
};

#define URC_FOREIGN_EXCEPTION_CAUGHT 1

void _Unwind_DeleteException(struct qrx_unwind_exception *exc)
{
    if (exc && exc->exception_cleanup)
        exc->exception_cleanup(URC_FOREIGN_EXCEPTION_CAUGHT, exc);
}
