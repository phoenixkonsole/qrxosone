/*
 * QRXAppRun - run a classic MacOS PPC application under the QRXPPC HLE.
 *
 * Usage: QRXAppRun <application> [resource-fork]
 *
 * The first file is a MacBinary application or a bare PEF container.  When a
 * separate resource fork is given, both files are wrapped into a MacBinary
 * image in memory first, which is what a file extracted from HFS or a .sit
 * archive usually needs.  Every Toolbox trap the application calls is printed;
 * the run ends at the first trap the HLE does not implement.
 *
 * Exit codes: 0 main returned, 2 unimplemented trap, 3 not a PEF application,
 * 1 any other stop.
 */
#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qrx_apprun.h"

#define APP_MEM_SIZE (4u * 1024u * 1024u)
#define APP_ZONE_BASE 0x1000u
#define APP_ZONE_SIZE 0x8000u
#define APP_GLUE 0x0800u
#define APP_LOAD_BASE 0x10000u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint8_t *slurp(const char *path, uint32_t *len_out)
{
    BPTR f;
    int32_t len;
    uint8_t *buf;
    int32_t got = 0;

    f = Open((STRPTR)path, MODE_OLDFILE);
    if (!f)
        return 0;
    if (Seek(f, 0, OFFSET_END) < 0) {
        Close(f);
        return 0;
    }
    len = Seek(f, 0, OFFSET_BEGINNING);
    if (len <= 0) {
        Close(f);
        return 0;
    }
    buf = (uint8_t *)AllocVec((uint32_t)len, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) {
        Close(f);
        return 0;
    }
    while (got < len) {
        int32_t n = Read(f, buf + got, (uint32_t)(len - got));
        if (n <= 0)
            break;
        got += n;
    }
    Close(f);
    if (got != len) {
        FreeVec(buf);
        return 0;
    }
    *len_out = (uint32_t)len;
    return buf;
}

/* Wrap a data and resource fork into a MacBinary image (version 0, no CRC). */
static uint8_t *wrap_macbinary(const uint8_t *data, uint32_t dlen,
                               const uint8_t *rsrc, uint32_t rlen,
                               const char *name, uint32_t *out_len)
{
    uint32_t pad_d = (dlen + 127u) & ~127u;
    uint32_t pad_r = (rlen + 127u) & ~127u;
    uint32_t total = QRX_MACBIN_HEADER + pad_d + pad_r;
    uint8_t *img = (uint8_t *)AllocVec(total, MEMF_PUBLIC | MEMF_CLEAR);
    unsigned len = 0u;
    const char *base = name;
    const char *p;

    if (!img)
        return 0;
    for (p = name; *p != '\0'; p++)
        if (*p == ':' || *p == '/')
            base = p + 1;
    while (base[len] != '\0' && len < QRX_MACBIN_NAME_MAX)
        len++;
    img[0] = 0u;                  /* no MacBinary II header, so no CRC */
    img[1] = (uint8_t)len;
    memcpy(img + 2u, base, len);
    put32(img + 65u, 0x4150504Cu);          /* 'APPL' */
    put32(img + 69u, 0x71525841u);          /* 'qRXA' */
    put32(img + 83u, dlen);
    put32(img + 87u, rlen);
    memcpy(img + QRX_MACBIN_HEADER, data, dlen);
    if (rlen != 0u)
        memcpy(img + QRX_MACBIN_HEADER + pad_d, rsrc, rlen);
    *out_len = total;
    return img;
}

static int looks_macbinary(const uint8_t *img, uint32_t size)
{
    uint32_t dlen, rlen, need;

    if (size < QRX_MACBIN_HEADER || img[0] != 0u || img[1] < 1u ||
        img[1] > QRX_MACBIN_NAME_MAX)
        return 0;
    dlen = get32(img + 83u);
    rlen = get32(img + 87u);
    if (dlen > QRX_MACBIN_FORK_MAX || rlen > QRX_MACBIN_FORK_MAX)
        return 0;
    need = QRX_MACBIN_HEADER + ((dlen + 127u) & ~127u) + ((rlen + 127u) & ~127u);
    return need <= size;
}

static void print_traps(const qrx_apprun_t *run)
{
    unsigned i, n = run->ntrace;

    if (n > QRX_APPRUN_TRACE)
        n = QRX_APPRUN_TRACE;
    for (i = 0u; i < n; i++) {
        const qrx_apprun_trap_t *t = &run->trace[i];

        Printf((STRPTR)"trap 0x%08lx sel=%lu r4=0x%08lx r5=0x%08lx -> 0x%08lx%s\n",
               (unsigned long)t->trap, (unsigned long)t->selector,
               (unsigned long)t->arg0, (unsigned long)t->arg1,
               (unsigned long)t->result,
               t->implemented ? (STRPTR)"" : (STRPTR)" MISSING");
    }
}

int main(int argc, char **argv)
{
    uint8_t *image = 0;
    uint8_t *data = 0;
    uint8_t *rsrc = 0;
    uint32_t image_len = 0;
    uint32_t dlen = 0;
    uint32_t rlen = 0;
    uint8_t *mem = 0;
    qrx_ppc_t ppc;
    static qrx_apprun_t run;
    qrx_apprun_status_t st;
    int rc = 1;

    if (argc < 2) {
        PutStr((STRPTR)"usage: QRXAppRun <application> [resource-fork]\n");
        return 1;
    }
    if (argc >= 3) {
        data = slurp(argv[1], &dlen);
        rsrc = slurp(argv[2], &rlen);
        if (!data || !rsrc) {
            PutStr((STRPTR)"QRXAppRun: cannot read both forks\n");
            if (data) FreeVec(data);
            if (rsrc) FreeVec(rsrc);
            return 1;
        }
        image = wrap_macbinary(data, dlen, rsrc, rlen, argv[1], &image_len);
    } else {
        image = slurp(argv[1], &image_len);
        dlen = image_len;
    }
    if (!image) {
        PutStr((STRPTR)"QRXAppRun: cannot read application\n");
        if (data) FreeVec(data);
        if (rsrc) FreeVec(rsrc);
        return 1;
    }

    mem = (uint8_t *)AllocVec(APP_MEM_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!mem) {
        PutStr((STRPTR)"QRXAppRun: no guest memory\n");
        FreeVec(image);
        if (data) FreeVec(data);
        if (rsrc) FreeVec(rsrc);
        return 1;
    }
    qrx_ppc_init(&ppc, mem, APP_MEM_SIZE);

    Printf((STRPTR)"QRXAppRun: %s (%lu bytes)\n", (STRPTR)argv[1],
           (unsigned long)image_len);
    st = qrx_apprun_init(&run, &ppc, APP_ZONE_BASE, APP_ZONE_SIZE, APP_GLUE,
                         APP_MEM_SIZE - 0x1000u, APP_MEM_SIZE - 0x8000u);
    if (st != QRX_APPRUN_OK) {
        Printf((STRPTR)"QRXAppRun: init failed: %s\n",
               (STRPTR)qrx_apprun_strerror(st));
        goto done;
    }
    st = qrx_apprun_load(&run, image, image_len, APP_LOAD_BASE);
    Printf((STRPTR)"QRXAppRun: %s\n", (STRPTR)run.detail);
    if (st != QRX_APPRUN_OK) {
        Printf((STRPTR)"QRXAppRun: refused: %s\n",
               (STRPTR)qrx_apprun_strerror(st));
        rc = 3;
        goto done;
    }
    Printf((STRPTR)"QRXAppRun: import stubs: %lu with a trap, %lu indirect, "
                    "%lu without HLE\n",
           (unsigned long)run.nimport_stub, (unsigned long)run.nimport_wrapper,
           (unsigned long)run.nimport_missing);
    st = qrx_apprun_start(&run, 2000000u);
    print_traps(&run);
    Printf((STRPTR)"QRXAppRun: %s after %lu trap(s), %lu implemented\n",
           (STRPTR)qrx_apprun_strerror(st), (unsigned long)run.ntrace_total,
           (unsigned long)run.nimplemented);
    if (run.unimplemented_trap != 0u)
        Printf((STRPTR)"QRXAppRun: first missing trap 0x%08lx\n",
               (unsigned long)run.unimplemented_trap);
    {
        const char *library = 0;
        const char *missing = qrx_apprun_missing_import(&run, &library);

        if (missing)
            Printf((STRPTR)"QRXAppRun: first missing import %s::%s\n",
                   (STRPTR)(library ? library : "?"), (STRPTR)missing);
    }
    Printf((STRPTR)"QRXAppRun: %s\n", (STRPTR)run.detail);
    if (st == QRX_APPRUN_EXITED)
        rc = 0;
    else if (st == QRX_APPRUN_UNIMPLEMENTED)
        rc = 2;
    else
        rc = 1;

done:
    FreeVec(mem);
    FreeVec(image);
    if (data) FreeVec(data);
    if (rsrc) FreeVec(rsrc);
    return rc;
}
