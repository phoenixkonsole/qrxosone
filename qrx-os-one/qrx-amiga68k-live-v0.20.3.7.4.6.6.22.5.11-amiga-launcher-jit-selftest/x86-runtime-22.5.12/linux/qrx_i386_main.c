/* QRX OS One 22.5.12 - qrx-i386: run Linux i386 ELF programs through qrx86core.
 *   qrx-i386 [-r ROOT] [-t] [-b BUDGET] [-e VAR=VALUE]... PROGRAM [ARGS...]
 * PROGRAM is a guest path below ROOT (default ROOT: none = host paths). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qrx86linux.h"

static void usage(void)
{
    fprintf(stderr, "usage: qrx-i386 [-r ROOT] [-t] [-b BUDGET] [-e VAR=VALUE]... PROGRAM [ARGS...]\n"
                    "  Runs a Linux i386 ELF (static or with its ld-linux.so.2 below ROOT) in the\n"
                    "  QRX qrx86core interpreter.  No native x86 code is executed.\n");
}

int main(int argc, char **argv)
{
    lxopts o;
    char *env[64];
    int envc = 0, i = 1;
    memset(&o, 0, sizeof(o));
    o.root = "";
    env[envc++] = "PATH=/bin:/usr/bin:/sbin:/usr/sbin";
    env[envc++] = "HOME=/";
    env[envc++] = "TERM=dumb";
    env[envc++] = "LANG=C";
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) o.root = argv[++i];
        else if (!strcmp(argv[i], "-t")) o.trace = 1;
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) o.budget = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-e") && i + 1 < argc && envc < 63) env[envc++] = argv[++i];
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { usage(); return 2; }
        i++;
    }
    if (i >= argc) { usage(); return 2; }
    env[envc] = NULL;
    return lx_run(&o, argv[i], argc - i, argv + i, env);
}
