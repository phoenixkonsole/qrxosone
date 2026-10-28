/* Host harness for qrx_hunk.c (22.5.11).  HOST-ONLY evidence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/qrx_hunk.h"

static int analyze(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b = NULL;
    long n = 0;
    static struct qrx_hunk_report r;
    int i, ok;
    if (f) {
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        b = malloc(n ? (size_t)n : 1);
        if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 3; }
        fclose(f);
    } else { printf("OPEN-FAIL\n"); return 3; }
    ok = qrx_hunk_analyze(b, (qrx_u32)n, &r);
    printf("KIND=%d %s\nVALID=%d\nRUNNABLE=%d\nHUNKS=%lu\nRELOCS=%lu\nCHIP=%lu\nOVERLAY=%d\nSYMBOLS=%d\nDEBUG=%d\n",
           r.kind, qrx_bin_kind_name(r.kind), r.valid, r.runnable, r.hunk_count, r.reloc_total, r.chip_bytes,
           r.has_overlay, r.has_symbols, r.has_debug);
    printf("REFS=");
    for (i = 0; i < r.ref_count; i++) printf("%s%s", i ? "," : "", r.refs[i].name);
    printf("\nERROR=%s\n", r.error);
    for (i = 0; i < r.warn_count; i++) printf("WARN=%s\n", r.warn[i]);
    for (i = 0; i < (int)r.hunk_count && i < QRX_HUNK_MAX_HUNKS; i++)
        printf("HUNK%d=%s alloc=%lu file=%lu relocs=%lu mem=%lu\n", i, qrx_hunk_type_name(r.hunks[i].type),
               r.hunks[i].alloc_bytes, r.hunks[i].file_bytes, r.hunks[i].relocs, r.hunks[i].memflags);
    free(b);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--parse")) {
        struct qrx_launch_req q;
        char cmd[1600];
        int ok = qrx_parse_launch_line(argv[2], &q);
        printf("OK=%d\nCHECK=%d ASYNC=%d QUIET=%d FORCE=%d STRICT=%d NOLOAD=%d STACK=%lu\nPROGRAM=[%s]\nARGS=[%s]\nERROR=%s\n",
               ok, q.check_only, q.async, q.quiet, q.force, q.strict, q.noload, q.stack, q.program, q.args, q.error);
        if (ok && qrx_build_command(&q, cmd, sizeof(cmd))) printf("COMMAND=[%s]\n", cmd);
        return ok ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[1], "--quote")) {
        char out[256];
        int ok = qrx_dos_quote(argv[2], out, sizeof(out));
        printf("OK=%d\nQUOTED=[%s]\n", ok, ok ? out : "");
        return ok ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[1], "--rc")) { printf("%s\n", qrx_rc_class(atol(argv[2]))); return 0; }
    if (argc == 2) return analyze(argv[1]);
    fprintf(stderr, "usage: qrx_hunk_host FILE | --parse LINE | --quote S | --rc N\n");
    return 2;
}
