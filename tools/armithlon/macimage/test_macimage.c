#include <stdio.h>
#include <string.h>

#include "qrx_macimage.h"
#include "qrx_forkfile.h"
#include "qrx_hfs.h"
#include "qrx_hfsplus.h"
#include "qrx_mfs.h"
#include "qrx_testimg.h"

static int failures;
static int checks;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void check_u32(uint32_t got, uint32_t want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %lu want %lu\n", what, (unsigned long)got,
               (unsigned long)want);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
    }
}

static const qrx_mfs_file_t *find_file(const qrx_mfs_t *mfs, const char *name)
{
    uint16_t i;

    for (i = 0; i < mfs->nfiles; i++) {
        if (strcmp(mfs->files[i].name, name) == 0)
            return &mfs->files[i];
    }
    return NULL;
}

static uint8_t raw[QRX_TESTIMG_400K];
static uint8_t dc42[QRX_TESTIMG_DC42_MAX];
static uint8_t rsrc[QRX_TESTIMG_RSRC_MAX];
static uint8_t apm[QRX_TESTIMG_APM_SIZE];
static uint8_t apm_raw[QRX_TESTIMG_APM_SIZE];
static uint8_t fork[2048];
static uint8_t bigfork[9216];
static uint8_t hfs_img[QRX_TESTIMG_HFS800];
static uint8_t hfsp_img[QRX_TESTIMG_HFSP];
static uint8_t hfsx_img[QRX_TESTIMG_HFSP];
static uint8_t hfsp_wrap[QRX_TESTIMG_HFSP_WRAP];
static qrx_hfsp_t hfsp;
static qrx_forkfile_t ff;
static uint8_t forkblob[QRX_TESTIMG_FORK_CAP];
static uint8_t forkwork[QRX_TESTIMG_FORK_CAP];

int main(void)
{
    uint32_t raw_len = 0;
    uint32_t dc42_len = 0;
    uint32_t rsrc_len = 0;
    uint32_t apm_len = 0;
    uint32_t hfs_len = 0;
    uint32_t hfsp_len = 0;
    uint32_t wrap_len = 0;
    uint32_t fork_len = 0;
    uint32_t written = 0;
    qrx_macimg_t img;
    qrx_mfs_t mfs;
    qrx_hfs_t hfs;
    const qrx_mfs_file_t *file;
    const qrx_hfs_entry_t *entry;
    const qrx_hfsp_entry_t *fentry;
    uint32_t i;
    int32_t status;

    /* ---- raw 400K MFS volume ------------------------------------------- */
    check(qrx_testimg_mfs400(raw, sizeof(raw), &raw_len) == 1, "build mfs400");
    check_u32(raw_len, QRX_TESTIMG_400K, "mfs400 length");

    status = qrx_macimg_open(&img, raw, raw_len, NULL, 0);
    check(status == QRX_MACIMG_OK, "open raw");
    check(img.container == QRX_MACIMG_RAW, "raw container");
    check(img.fs == QRX_MACIMG_FS_MFS, "raw volume is mfs");
    check_u32(img.volume_offset, 0, "raw volume offset");

    status = qrx_mfs_open(&mfs, raw, raw_len);
    check(status == QRX_MACIMG_OK, "mfs open");
    check_str(mfs.name, "Test Disk", "volume name");
    check_u32(mfs.alloc_blocks, 391, "drNmAlBlks");
    check_u32(mfs.alloc_size, 1024, "drAlBlkSiz");
    check_u32(mfs.free_blocks, 387, "drFreeBks");
    check_u32(mfs.nfiles, 3, "file count");

    file = find_file(&mfs, "Hello");
    check(file != NULL, "find Hello");
    if (file) {
        check_u32(file->logical_len, 5, "Hello length");
        check_u32(file->type_code, 0x54455854u, "Hello type");
        check_u32(file->creator, 0x74747874u, "Hello creator");
        status = qrx_mfs_read_fork(&mfs, file, 0, fork, sizeof(fork), &written);
        check(status == QRX_MACIMG_OK, "read Hello");
        check_u32(written, 5, "Hello bytes");
        check(memcmp(fork, "Hello", 5) == 0, "Hello content");
    }

    file = find_file(&mfs, "Payload");
    check(file != NULL, "find Payload");
    if (file) {
        check_u32(file->logical_len, 1300, "Payload length");
        check_u32(file->rsrc_len, 64, "Payload rsrc length");
        status = qrx_mfs_read_fork(&mfs, file, 0, fork, sizeof(fork), &written);
        check(status == QRX_MACIMG_OK, "read Payload data");
        check_u32(written, 1300, "Payload bytes");
        for (i = 0; i < 1300u; i++) {
            if (fork[i] != qrx_testimg_payload_byte(i)) {
                check(0, "Payload pattern");
                break;
            }
        }
        status = qrx_mfs_read_fork(&mfs, file, 1, fork, sizeof(fork), &written);
        check(status == QRX_MACIMG_OK, "read Payload rsrc");
        check_u32(written, 64, "Payload rsrc bytes");
        for (i = 0; i < 64u; i++) {
            if (fork[i] != qrx_testimg_rsrc_byte(i)) {
                check(0, "Payload rsrc pattern");
                break;
            }
        }
        /* A buffer smaller than the fork must be refused, not truncated. */
        check(qrx_mfs_read_fork(&mfs, file, 0, fork, 100u, &written) ==
                  QRX_MACIMG_ERR_RANGE, "short buffer rejected");
    }

    file = find_file(&mfs, "Empty");
    check(file != NULL, "find Empty");
    if (file) {
        check_u32(file->logical_len, 0, "Empty length");
        check(qrx_mfs_read_fork(&mfs, file, 0, fork, sizeof(fork), &written) ==
                  QRX_MACIMG_OK, "read Empty");
        check_u32(written, 0, "Empty bytes");
    }

    /* ---- DiskCopy 4.2 -------------------------------------------------- */
    check(qrx_testimg_dc42(raw, raw_len, dc42, sizeof(dc42), &dc42_len) == 1,
          "build dc42");
    check_u32(dc42_len, QRX_TESTIMG_400K + 84u, "dc42 length");
    status = qrx_macimg_open(&img, dc42, dc42_len, NULL, 0);
    check(status == QRX_MACIMG_OK, "open dc42");
    check(img.container == QRX_MACIMG_DC42, "dc42 container");
    check(img.checksum_ok == 1u, "dc42 checksum ok");
    check(img.fs == QRX_MACIMG_FS_MFS, "dc42 volume is mfs");
    check_str(img.name, "Test Disk", "dc42 image name");
    check_u32(img.size, QRX_TESTIMG_400K, "dc42 payload size");
    status = qrx_mfs_open(&mfs, dc42 + 84u, raw_len);
    check(status == QRX_MACIMG_OK, "mfs inside dc42");

    check(qrx_testimg_dc42_badsum(raw, raw_len, dc42, sizeof(dc42), &dc42_len) == 1,
          "build bad dc42");
    status = qrx_macimg_open(&img, dc42, dc42_len, NULL, 0);
    check(status == QRX_MACIMG_ERR_CHECKSUM, "dc42 checksum mismatch");
    check(img.container == QRX_MACIMG_DC42, "bad dc42 still dc42");

    /* ---- NDIF data fork with a bcem resource --------------------------- */
    check(qrx_testimg_bcem_rsrc(800u, rsrc, sizeof(rsrc), &rsrc_len) == 1,
          "build bcem rsrc");
    status = qrx_macimg_open(&img, raw, raw_len, rsrc, rsrc_len);
    check(status == QRX_MACIMG_OK, "open ndif");
    check(img.container == QRX_MACIMG_NDIF, "ndif container");
    check(img.fs == QRX_MACIMG_FS_MFS, "ndif volume is mfs");
    check_u32(img.ndif_blocks, 800, "bcem block count");
    check_str(img.name, "Test Disk", "bcem image name");

    /* A data fork shorter than the announced disk is chunked: refuse it. */
    status = qrx_macimg_open(&img, raw, 2048u, rsrc, rsrc_len);
    check(status == QRX_MACIMG_ERR_NDIF, "chunked ndif refused");
    /* Without the resource fork the same bytes are plain raw sectors. */
    status = qrx_macimg_open(&img, raw, 2048u, NULL, 0);
    check(status == QRX_MACIMG_OK && img.container == QRX_MACIMG_RAW,
          "ndif data fork alone is raw");

    /* ---- Apple Partition Map ------------------------------------------- */
    check(qrx_testimg_apm(raw, raw_len, apm_raw, sizeof(apm_raw), &apm_len) == 1,
          "build apm");
    memcpy(apm, apm_raw, apm_len);
    status = qrx_macimg_open(&img, apm, apm_len, NULL, 0);
    check(status == QRX_MACIMG_OK, "open apm");
    check(img.container == QRX_MACIMG_APM, "apm container");
    check_u32((uint32_t)img.nparts, 2, "apm partition count");
    check_u32(img.volume_part, 2, "apm picked the volume partition");
    check_u32(img.volume_offset, 64u * 512u, "apm volume offset");
    check(img.fs == QRX_MACIMG_FS_MFS, "apm volume is mfs");
    check_str(img.parts[1].name, "MacTest", "apm partition name");
    check_str(img.parts[1].type, "Apple_HFS", "apm partition type");

    status = qrx_macimg_select_partition(&img, 2);
    check(status == QRX_MACIMG_OK, "select partition 2");
    status = qrx_mfs_open(&mfs, apm + img.volume_offset, img.volume_size);
    check(status == QRX_MACIMG_OK, "mfs inside apm");
    check_str(mfs.name, "Test Disk", "apm volume name");
    check(qrx_macimg_select_partition(&img, 9) == QRX_MACIMG_ERR_PART,
          "partition out of range");


    /* ---- 800K HFS volume: catalog tree, forks, Finder info ------------ */
    check(qrx_testimg_hfs800(hfs_img, sizeof(hfs_img), &hfs_len) == 1,
          "build hfs800");
    check_u32(hfs_len, QRX_TESTIMG_HFS800, "hfs800 length");
    status = qrx_macimg_open(&img, hfs_img, hfs_len, NULL, 0);
    check(status == QRX_MACIMG_OK, "open hfs container");
    check(img.fs == QRX_MACIMG_FS_HFS, "image detected as hfs");

    status = qrx_hfs_open(&hfs, hfs_img, hfs_len);
    check(status == QRX_MACIMG_OK, "hfs open");
    check_str(hfs.name, "Test Disk", "hfs volume name");
    check_u32(hfs.alloc_blocks, 1580, "hfs allocation blocks");
    check_u32(hfs.alloc_size, 512, "hfs allocation size");
    check_u32(hfs.free_blocks, 1557, "hfs free blocks");
    check_u32(hfs.file_count, 4, "hfs file count");
    check_u32(hfs.dir_count, 2, "hfs directory count");
    check_u32(hfs.cat_node_size, 1024, "hfs catalog node size");
    check_u32(hfs.nentries, 5, "hfs catalog entries");

    entry = qrx_hfs_lookup(&hfs, 1, "Hello");
    check(entry != NULL, "hfs find Hello");
    if (entry) {
        check(entry->is_dir == 0, "Hello is a file");
        check_u32(entry->type_code, 0x54455854u, "Hello type");
        check_u32(entry->creator, 0x74747874u, "Hello creator");
        check_u32(entry->fd_flags, 0x0100u, "Hello Finder flags");
        check_u32(entry->data_len, 5, "Hello data length");
        check_u32(entry->rsrc_len, 0, "Hello rsrc length");
        status = qrx_hfs_read_fork(&hfs, entry, 0, fork, sizeof(fork), &written);
        check(status == QRX_MACIMG_OK, "hfs read Hello");
        check_u32(written, 5, "hfs Hello bytes");
        check(memcmp(fork, "Hello", 5) == 0, "hfs Hello content");
    }

    entry = qrx_hfs_lookup(&hfs, 1, "Payload");
    check(entry != NULL, "hfs find Payload");
    if (entry) {
        check_u32(entry->data_len, 1300, "Payload data length");
        check_u32(entry->rsrc_len, 64, "Payload rsrc length");
        status = qrx_hfs_read_fork(&hfs, entry, 0, fork, sizeof(fork), &written);
        check(status == QRX_MACIMG_OK, "hfs read Payload data");
        check_u32(written, 1300, "Payload data bytes");
        for (i = 0; i < 1300u; i++) {
            if (fork[i] != qrx_testimg_payload_byte(i)) {
                check(0, "Payload data pattern");
                break;
            }
        }
        status = qrx_hfs_read_fork(&hfs, entry, 1, fork, sizeof(fork), &written);
        check(status == QRX_MACIMG_OK, "hfs read Payload rsrc");
        check_u32(written, 64, "Payload rsrc bytes");
        for (i = 0; i < 64u; i++) {
            if (fork[i] != qrx_testimg_rsrc_byte(i)) {
                check(0, "Payload rsrc pattern");
                break;
            }
        }
        check(qrx_hfs_read_fork(&hfs, entry, 1, fork, 32u, &written) ==
                  QRX_MACIMG_ERR_RANGE, "hfs short buffer rejected");
    }

    entry = qrx_hfs_lookup(&hfs, 1, "Docs");
    check(entry != NULL && entry->is_dir == 1, "hfs find Docs folder");
    if (entry)
        check_u32(entry->cnid, 16, "Docs cnid");
    entry = qrx_hfs_lookup(&hfs, 16, "Note");
    check(entry != NULL, "hfs find Note below Docs");
    if (entry)
        check_u32(entry->data_len, 700, "Note data length");
    entry = qrx_hfs_lookup_path(&hfs, "Docs:Note");
    check(entry != NULL, "hfs path lookup Docs:Note");
    if (entry) {
        status = qrx_hfs_read_fork(&hfs, entry, 0, fork, sizeof(fork), &written);
        check(status == QRX_MACIMG_OK, "hfs read Note");
        check_u32(written, 700, "Note bytes");
        check(fork[0] == qrx_testimg_rsrc_byte(8u), "Note first byte");
        check(fork[699] == qrx_testimg_rsrc_byte(707u), "Note last byte");
    }
    check(qrx_hfs_lookup_path(&hfs, ":Docs:Note") == entry,
          "hfs path lookup ignores a leading colon");

    entry = qrx_hfs_lookup(&hfs, 1, "Frag");
    check(entry != NULL, "hfs find Frag");
    if (entry) {
        check_u32(entry->data_len, 2048, "Frag data length");
        status = qrx_hfs_read_fork(&hfs, entry, 0, fork, sizeof(fork), &written);
        check(status == QRX_MACIMG_OK, "hfs read Frag through overflow tree");
        check_u32(written, 2048, "Frag bytes");
        check(fork[0] == qrx_testimg_payload_byte(0u), "Frag first byte");
        check(fork[1536] == qrx_testimg_payload_byte(1536u),
              "Frag third extent byte");
        check(fork[2047] == qrx_testimg_payload_byte(2047u),
              "Frag overflow extent byte");
    }

    check(qrx_hfs_lookup(&hfs, 1, "Missing") == NULL, "hfs missing name");
    check(qrx_hfs_lookup_path(&hfs, "Docs:Missing") == NULL,
          "hfs missing path");
    check(qrx_hfs_lookup_path(&hfs, "Hello:Note") == NULL,
          "hfs cannot descend into a file");
    check(qrx_hfs_open(&hfs, hfs_img, 2000u) == QRX_MACIMG_ERR_RANGE,
          "hfs rejects a truncated volume");
    check(qrx_hfs_open(&hfs, raw, raw_len) == QRX_MACIMG_ERR_FORMAT,
          "hfs rejects an MFS volume");


    /* ---- 2 MiB HFS+ volume: Unicode, overflow, attribute fork -------- */
    check(qrx_testimg_hfsp(hfsp_img, sizeof(hfsp_img), &hfsp_len, 0) == 1,
          "build hfsp");
    check_u32(hfsp_len, QRX_TESTIMG_HFSP, "hfsp length");
    status = qrx_macimg_open(&img, hfsp_img, hfsp_len, NULL, 0);
    check(status == QRX_MACIMG_OK, "open hfsp container");
    check(img.fs == QRX_MACIMG_FS_HFSPLUS, "image detected as hfs+");

    status = qrx_hfsp_open(&hfsp, hfsp_img, hfsp_len);
    check(status == QRX_MACIMG_OK, "hfsp open");
    check_str(hfsp.name, "Test Disk", "hfsp volume name");
    check_u32(hfsp.block_size, 1024, "hfsp block size");
    check_u32(hfsp.total_blocks, 2048, "hfsp total blocks");
    check_u32(hfsp.free_blocks, 2022, "hfsp free blocks");
    check_u32(hfsp.file_count, 5, "hfsp file count");
    check_u32(hfsp.folder_count, 2, "hfsp folder count");
    check(hfsp.journaled == 0, "hfsp volume is not journaled");
    check(hfsp.wrapped == 0, "hfsp volume is not wrapped");
    check(hfsp.case_sensitive == 0, "hfsp folds case");
    check_u32(hfsp.nentries, 6, "hfsp catalog entries");

    fentry = qrx_hfsp_lookup(&hfsp, 2, "Hello");
    check(fentry != NULL, "hfsp find Hello");
    if (fentry) {
        check_u32(fentry->type_code, 0x54455854u, "hfsp Hello type");
        check_u32(fentry->creator, 0x74747874u, "hfsp Hello creator");
        check_u32(fentry->fd_flags, 0x0100u, "hfsp Hello Finder flags");
        check_u32(fentry->data_len, 5, "hfsp Hello length");
        status = qrx_hfsp_read_fork(&hfsp, fentry, 0, fork, sizeof(fork),
                                    &written);
        check(status == QRX_MACIMG_OK, "hfsp read Hello");
        check_u32(written, 5, "hfsp Hello bytes");
        check(memcmp(fork, "Hello", 5) == 0, "hfsp Hello content");
    }
    /*
     * HFS+ folds ASCII case, so the lower-case query has to find the same
     * record; an HFSX volume below proves the difference.
     */
    check(qrx_hfsp_lookup(&hfsp, 2, "hello") == fentry,
          "hfsp lookup is case-insensitive");

    fentry = qrx_hfsp_lookup(&hfsp, 2, "Payload");
    check(fentry != NULL, "hfsp find Payload");
    if (fentry) {
        check_u32(fentry->data_len, 1300, "hfsp Payload data length");
        check_u32(fentry->rsrc_len, 64, "hfsp Payload rsrc length");
        status = qrx_hfsp_read_fork(&hfsp, fentry, 0, fork, sizeof(fork),
                                    &written);
        check(status == QRX_MACIMG_OK, "hfsp read Payload data");
        check_u32(written, 1300, "hfsp Payload data bytes");
        for (i = 0; i < 1300u; i++) {
            if (fork[i] != qrx_testimg_payload_byte(i)) {
                check(0, "hfsp Payload data pattern");
                break;
            }
        }
        status = qrx_hfsp_read_fork(&hfsp, fentry, 1, fork, sizeof(fork),
                                    &written);
        check(status == QRX_MACIMG_OK, "hfsp read Payload rsrc");
        check_u32(written, 64, "hfsp Payload rsrc bytes");
        for (i = 0; i < 64u; i++) {
            if (fork[i] != qrx_testimg_rsrc_byte(i)) {
                check(0, "hfsp Payload rsrc pattern");
                break;
            }
        }
        check(qrx_hfsp_read_fork(&hfsp, fentry, 1, fork, 32u, &written) ==
                  QRX_MACIMG_ERR_RANGE, "hfsp short buffer rejected");
    }

    fentry = qrx_hfsp_lookup(&hfsp, 2, "Caf\xE9");
    check(fentry != NULL, "hfsp unicode name");
    if (fentry) {
        check(fentry->rsrc_via_attr == 1u, "Café rsrc from attribute tree");
        check_u32(fentry->rsrc_len, 40, "Café rsrc length");
        check_u32(fentry->data_len, 0, "Café data length");
        status = qrx_hfsp_read_fork(&hfsp, fentry, 1, fork, sizeof(fork),
                                    &written);
        check(status == QRX_MACIMG_OK, "hfsp read attribute resource fork");
        check_u32(written, 40, "Café rsrc bytes");
        for (i = 0; i < 40u; i++) {
            if (fork[i] != qrx_testimg_rsrc_byte(i + 64u)) {
                check(0, "Café rsrc pattern");
                break;
            }
        }
    }

    fentry = qrx_hfsp_lookup(&hfsp, 2, "Frag");
    check(fentry != NULL, "hfsp find Frag");
    if (fentry) {
        check_u32(fentry->data_len, 9000, "hfsp Frag length");
        status = qrx_hfsp_read_fork(&hfsp, fentry, 0, bigfork,
                                    sizeof(bigfork), &written);
        check(status == QRX_MACIMG_OK, "hfsp read Frag through overflow tree");
        check_u32(written, 9000, "hfsp Frag bytes");
        check(bigfork[0] == qrx_testimg_payload_byte(0u),
              "hfsp Frag first byte");
        check(bigfork[8191] == qrx_testimg_payload_byte(8191u),
              "hfsp Frag eighth extent byte");
        check(bigfork[8192] == qrx_testimg_payload_byte(8192u),
              "hfsp Frag overflow extent byte");
        check(bigfork[8999] == qrx_testimg_payload_byte(8999u),
              "hfsp Frag last byte");
    }

    fentry = qrx_hfsp_lookup_path(&hfsp, "Docs:Note");
    check(fentry != NULL, "hfsp path lookup Docs:Note");
    if (fentry) {
        check_u32(fentry->data_len, 700, "hfsp Note length");
        status = qrx_hfsp_read_fork(&hfsp, fentry, 0, fork, sizeof(fork),
                                    &written);
        check(status == QRX_MACIMG_OK, "hfsp read Note");
        check_u32(written, 700, "hfsp Note bytes");
        check(fork[699] == qrx_testimg_rsrc_byte(707u), "hfsp Note last byte");
    }
    check(qrx_hfsp_lookup_path(&hfsp, "Docs:Missing") == NULL,
          "hfsp missing path");
    check(qrx_hfsp_lookup_path(&hfsp, "Hello:Note") == NULL,
          "hfsp cannot descend into a file");
    check(qrx_hfsp_open(&hfsp, hfsp_img, 2000u) == QRX_MACIMG_ERR_RANGE,
          "hfsp rejects a truncated volume");
    check(qrx_hfsp_open(&hfsp, raw, raw_len) == QRX_MACIMG_ERR_FORMAT,
          "hfsp rejects an MFS volume");

    /* ---- HFS wrapper around the same HFS+ volume --------------------- */
    check(qrx_testimg_hfsp_wrapper(hfsp_img, hfsp_len, hfsp_wrap,
                                   sizeof(hfsp_wrap), &wrap_len) == 1,
          "build hfsp wrapper");
    check_u32(wrap_len, QRX_TESTIMG_HFSP_WRAP, "wrapper length");
    status = qrx_macimg_open(&img, hfsp_wrap, wrap_len, NULL, 0);
    check(status == QRX_MACIMG_OK, "open wrapper container");
    check(img.fs == QRX_MACIMG_FS_HFSPLUS, "wrapper detected as hfs+");
    status = qrx_hfsp_open(&hfsp, hfsp_wrap, wrap_len);
    check(status == QRX_MACIMG_OK, "hfsp open through wrapper");
    check(hfsp.wrapped == 1, "wrapper flag set");
    check_u32(hfsp.offset, 4096, "wrapper volume offset");
    check_str(hfsp.name, "Test Disk", "wrapper volume name");
    check_u32(hfsp.nentries, 6, "wrapper catalog entries");
    fentry = qrx_hfsp_lookup_path(&hfsp, "Docs:Note");
    if (fentry) {
        status = qrx_hfsp_read_fork(&hfsp, fentry, 0, fork, sizeof(fork),
                                    &written);
        check(status == QRX_MACIMG_OK, "wrapper reads the embedded volume");
        check_u32(written, 700, "wrapper Note bytes");
    } else {
        check(0, "wrapper finds Docs:Note");
    }

    /* ---- HFSX keeps names case-sensitive ----------------------------- */
    check(qrx_testimg_hfsp(hfsx_img, sizeof(hfsx_img), &hfsp_len, 1) == 1,
          "build hfsx");
    status = qrx_macimg_open(&img, hfsx_img, hfsp_len, NULL, 0);
    check(img.fs == QRX_MACIMG_FS_HFSPLUS, "hfsx detected as hfs+");
    check(qrx_hfsp_open(&hfsp, hfsx_img, hfsp_len) == QRX_MACIMG_OK,
          "hfsx open");
    check(hfsp.case_sensitive == 1, "hfsx compares case");
    check_u32(hfsp.nentries, 7, "hfsx catalog entries");
    fentry = qrx_hfsp_lookup(&hfsp, 2, "Hello");
    check(fentry != NULL && fentry->cnid == 2u, "hfsx finds Hello");
    fentry = qrx_hfsp_lookup(&hfsp, 2, "hello");
    check(fentry != NULL && fentry->cnid == 7u,
          "hfsx finds the lower-case hello");
    check(qrx_hfsp_lookup(&hfsp, 2, "HELLO") == NULL,
          "hfsx rejects a wrong-case query");


    /* ---- fork containers: AppleSingle/Double, MacBinary, BinHex ------- */
    check(qrx_testimg_applesingle(forkblob, sizeof(forkblob), &fork_len) == 1,
          "build applesingle");
    status = qrx_forkfile_open(&ff, forkblob, fork_len, NULL, 0);
    check(status == QRX_MACIMG_OK, "applesingle open");
    check(ff.format == QRX_FORKFILE_APPLESINGLE, "applesingle format");
    check(ff.has_name == 1u, "applesingle has a name");
    check_str(ff.name, "Hello", "applesingle name");
    check_u32(ff.type_code, 0x54455854u, "applesingle type");
    check_u32(ff.creator, 0x74747874u, "applesingle creator");
    check_u32(ff.fd_flags, 0x0100u, "applesingle Finder flags");
    check_u32(ff.fd_location, 0x00400080u, "applesingle icon location");
    check_u32(ff.created, 0xC0000000u, "applesingle created");
    check_u32(ff.modified, 0xC0000100u, "applesingle modified");
    check_u32(ff.data_len, 5, "applesingle data length");
    check_u32(ff.rsrc_len, 64, "applesingle rsrc length");
    status = qrx_forkfile_read(&ff, 0, 0, fork, 5);
    check(status == QRX_MACIMG_OK, "applesingle read data fork");
    check(memcmp(fork, "Hello", 5) == 0, "applesingle data content");
    status = qrx_forkfile_read(&ff, 1, 0, fork, 64);
    check(status == QRX_MACIMG_OK, "applesingle read rsrc fork");
    for (i = 0; i < 64u; i++) {
        if (fork[i] != qrx_testimg_rsrc_byte(i)) {
            check(0, "applesingle rsrc pattern");
            break;
        }
    }
    check(qrx_forkfile_read(&ff, 0, 3, fork, 4) == QRX_MACIMG_ERR_RANGE,
          "fork read past the end is refused");
    check(qrx_forkfile_read(&ff, 2, 0, fork, 1) == QRX_MACIMG_ERR_RANGE,
          "unknown fork is refused");

    check(qrx_testimg_appledouble(forkblob, sizeof(forkblob), &fork_len) == 1,
          "build appledouble");
    status = qrx_forkfile_open(&ff, forkblob, fork_len, NULL, 0);
    check(status == QRX_MACIMG_OK, "appledouble open");
    check(ff.format == QRX_FORKFILE_APPLEDOUBLE, "appledouble format");
    check_str(ff.name, "Payload", "appledouble name");
    check_u32(ff.type_code, 0x42494E41u, "appledouble type");
    check_u32(ff.creator, 0x5152582Bu, "appledouble creator");
    check_u32(ff.fd_flags, 0x0080u, "appledouble Finder flags");
    check_u32(ff.data_len, 0, "appledouble has no data fork");
    check(ff.data == NULL, "appledouble data pointer is empty");
    check_u32(ff.rsrc_len, 64, "appledouble rsrc length");
    status = qrx_forkfile_read(&ff, 1, 0, fork, 64);
    check(status == QRX_MACIMG_OK, "appledouble read rsrc fork");
    check(fork[63] == qrx_testimg_rsrc_byte(63u), "appledouble rsrc last byte");

    check(qrx_testimg_macbinary(forkblob, sizeof(forkblob), &fork_len) == 1,
          "build macbinary");
    status = qrx_forkfile_open(&ff, forkblob, fork_len, NULL, 0);
    check(status == QRX_MACIMG_OK, "macbinary open");
    check(ff.format == QRX_FORKFILE_MACBINARY, "macbinary format");
    check_str(ff.name, "Payload", "macbinary name");
    check_u32(ff.type_code, 0x42494E41u, "macbinary type");
    check_u32(ff.creator, 0x5152582Bu, "macbinary creator");
    check_u32(ff.fd_flags, 0x0100u, "macbinary Finder flags");
    check_u32(ff.fd_location, 0x00400080u, "macbinary icon location");
    check_u32(ff.data_len, 260, "macbinary data length");
    check_u32(ff.rsrc_len, 64, "macbinary rsrc length");
    check(ff.crc_state == 1u, "macbinary header CRC matches");
    status = qrx_forkfile_read(&ff, 0, 0, fork, 260u);
    check(status == QRX_MACIMG_OK, "macbinary read data fork");
    for (i = 0; i < 260u; i++) {
        if (fork[i] != qrx_testimg_payload_byte(i)) {
            check(0, "macbinary data pattern");
            break;
        }
    }
    status = qrx_forkfile_read(&ff, 1, 0, fork, 64);
    check(status == QRX_MACIMG_OK, "macbinary read rsrc fork");
    check(fork[0] == qrx_testimg_rsrc_byte(0u), "macbinary rsrc first byte");
    /* A damaged header must not hide the file: the CRC state reports it. */
    forkblob[100] ^= 0xFFu;
    status = qrx_forkfile_open(&ff, forkblob, fork_len, NULL, 0);
    check(status == QRX_MACIMG_OK, "macbinary still opens with a bad CRC");
    check(ff.crc_state == 2u, "macbinary reports the CRC mismatch");
    check(qrx_forkfile_open(&ff, forkblob, 100u, NULL, 0) ==
              QRX_MACIMG_ERR_SHORT, "macbinary truncated header refused");

    check(qrx_testimg_binhex(forkblob, sizeof(forkblob), &fork_len) == 1,
          "build binhex");
    check(qrx_forkfile_open(&ff, forkblob, fork_len, NULL, 0) ==
              QRX_MACIMG_ERR_RANGE, "binhex needs a scratch buffer");
    status = qrx_forkfile_open(&ff, forkblob, fork_len, forkwork,
                               sizeof(forkwork));
    check(status == QRX_MACIMG_OK, "binhex open");
    check(ff.format == QRX_FORKFILE_BINHEX, "binhex format");
    check_str(ff.name, "Runs", "binhex name");
    check_u32(ff.type_code, 0x42494E41u, "binhex type");
    check_u32(ff.creator, 0x5152582Bu, "binhex creator");
    check_u32(ff.data_len, 260, "binhex data length");
    check_u32(ff.rsrc_len, 64, "binhex rsrc length");
    status = qrx_forkfile_read(&ff, 0, 0, bigfork, 260u);
    check(status == QRX_MACIMG_OK, "binhex read data fork");
    for (i = 0; i < 260u; i++) {
        if (bigfork[i] != qrx_testimg_binhex_byte(i)) {
            check(0, "binhex data pattern (run-length decoded)");
            break;
        }
    }
    status = qrx_forkfile_read(&ff, 1, 0, fork, 64);
    check(status == QRX_MACIMG_OK, "binhex read rsrc fork");
    check(fork[63] == qrx_testimg_rsrc_byte(63u), "binhex rsrc last byte");
    check(qrx_forkfile_open(&ff, forkblob, fork_len, forkwork, 100u) ==
              QRX_MACIMG_ERR_RANGE, "binhex scratch too small refused");

    memset(forkblob, 0, 64u);
    check(qrx_forkfile_open(&ff, forkblob, 64u, NULL, 0) ==
              QRX_MACIMG_ERR_FORMAT, "unknown fork container refused");

    /* ---- negative cases ------------------------------------------------ */
    memset(raw, 0, 4096u);
    status = qrx_macimg_open(&img, raw, 4096u, NULL, 0);
    check(status == QRX_MACIMG_OK && img.fs == QRX_MACIMG_FS_NONE,
          "zeros are a container without a volume");
    check(qrx_mfs_open(&mfs, raw, 4096u) == QRX_MACIMG_ERR_FORMAT,
          "mfs rejects a foreign volume");
    check(qrx_macimg_open(&img, raw, 100u, NULL, 0) == QRX_MACIMG_ERR_SHORT,
          "short image rejected");
    check(qrx_macimg_open(NULL, raw, 4096u, NULL, 0) == QRX_MACIMG_ERR_SHORT,
          "null image rejected");

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0)
        printf("macimage: PASS\n");
    else
        printf("macimage: FAIL\n");
    return failures == 0 ? 0 : 1;
}
