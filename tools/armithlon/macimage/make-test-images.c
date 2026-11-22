/*
 * Writes the synthetic Mac images the guest test suite mounts.
 * Everything is generated from qrx_testimg.c; no Apple material is used.
 */

#include <stdio.h>
#include <string.h>

#include "qrx_testimg.h"

static uint8_t raw[QRX_TESTIMG_400K];
static uint8_t dc42[QRX_TESTIMG_DC42_MAX];
static uint8_t rsrc[QRX_TESTIMG_RSRC_MAX];
static uint8_t apm[QRX_TESTIMG_APM_SIZE];
static uint8_t hfs[QRX_TESTIMG_HFS800];
static uint8_t hfsp[QRX_TESTIMG_HFSP];
static uint8_t hfsp_wrap[QRX_TESTIMG_HFSP_WRAP];
static uint8_t forkblob[QRX_TESTIMG_FORK_CAP];

static int write_file(const char *dir, const char *name, const uint8_t *data,
                      uint32_t len)
{
    char path[1024];
    FILE *file;

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return 0;
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return 0;
    }
    if (len > 0u && fwrite(data, 1u, len, file) != len) {
        fprintf(stderr, "short write %s\n", path);
        fclose(file);
        return 0;
    }
    fclose(file);
    printf("  %-26s %8lu bytes\n", name, (unsigned long)len);
    return 1;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    uint32_t raw_len = 0;
    uint32_t dc42_len = 0;
    uint32_t rsrc_len = 0;
    uint32_t apm_len = 0;
    uint32_t hfs_len = 0;
    uint32_t hfsp_len = 0;
    uint32_t wrap_len = 0;
    uint32_t fork_len = 0;

    if (!qrx_testimg_mfs400(raw, sizeof(raw), &raw_len)) {
        fprintf(stderr, "cannot build the MFS volume\n");
        return 1;
    }
    if (!write_file(dir, "mfs400.img", raw, raw_len))
        return 1;

    if (!qrx_testimg_dc42(raw, raw_len, dc42, sizeof(dc42), &dc42_len) ||
        !write_file(dir, "mfs400.dc42.img", dc42, dc42_len))
        return 1;

    if (!qrx_testimg_dc42_badsum(raw, raw_len, dc42, sizeof(dc42), &dc42_len) ||
        !write_file(dir, "mfs400.badcsum.img", dc42, dc42_len))
        return 1;

    if (!qrx_testimg_bcem_rsrc(800u, rsrc, sizeof(rsrc), &rsrc_len))
        return 1;
    if (!write_file(dir, "mfs400.ndif.img", raw, raw_len) ||
        !write_file(dir, "mfs400.ndif.img.rsrc", rsrc, rsrc_len))
        return 1;

    if (!write_file(dir, "mfs400.shortndif.img", raw, 2048u) ||
        !write_file(dir, "mfs400.shortndif.img.rsrc", rsrc, rsrc_len))
        return 1;

    if (!qrx_testimg_apm(raw, raw_len, apm, sizeof(apm), &apm_len) ||
        !write_file(dir, "apm.img", apm, apm_len))
        return 1;

    if (!qrx_testimg_hfs800(hfs, sizeof(hfs), &hfs_len) ||
        !write_file(dir, "hfs800.img", hfs, hfs_len))
        return 1;

    if (!qrx_testimg_hfsp(hfsp, sizeof(hfsp), &hfsp_len, 0) ||
        !write_file(dir, "hfsp.img", hfsp, hfsp_len))
        return 1;

    if (!qrx_testimg_hfsp_wrapper(hfsp, hfsp_len, hfsp_wrap,
                                  sizeof(hfsp_wrap), &wrap_len) ||
        !write_file(dir, "hfsp.wrapper.img", hfsp_wrap, wrap_len))
        return 1;

    if (!qrx_testimg_applesingle(forkblob, sizeof(forkblob), &fork_len) ||
        !write_file(dir, "hello.as", forkblob, fork_len))
        return 1;

    if (!qrx_testimg_appledouble(forkblob, sizeof(forkblob), &fork_len) ||
        !write_file(dir, "payload.ad", forkblob, fork_len))
        return 1;

    if (!qrx_testimg_macbinary(forkblob, sizeof(forkblob), &fork_len) ||
        !write_file(dir, "payload.mb", forkblob, fork_len))
        return 1;

    if (!qrx_testimg_binhex(forkblob, sizeof(forkblob), &fork_len) ||
        !write_file(dir, "runs.hqx", forkblob, fork_len))
        return 1;

    return 0;
}
