/*
 * QRX ABI Startup Isolation 01
 * First explicit Exec library call only.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>

int main(void)
{
    struct Library *lib;
    lib = OpenLibrary((STRPTR)"dos.library", 0);
    if (!lib)
        return 20;
    CloseLibrary(lib);
    return 0;
}
