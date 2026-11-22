/*
 * QRXMacFS - read-only view of Mac disk images (raw, DiskCopy 4.2,
 * uncompressed NDIF, Apple Partition Map) with MFS, HFS and HFS+/HFSX
 * readers, including HFS-wrapped HFS+ volumes, plus the fork containers
 * AppleSingle, AppleDouble, MacBinary II and BinHex 4.0 (FORK).
 *
 *   QRXMacFS INFO  <image>
 *   QRXMacFS LIST  <image> [partition]
 *   QRXMacFS CAT   <image> <name> [--rsrc]
 *   QRXMacFS EXTRACT <image> <name> <dest> [--rsrc]
 *
 * Nothing is written to the image.  A `<image>.rsrc` sidecar, when present,
 * is read as the NDIF resource fork.
 */

#include <exec/exec.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdlib.h>
#include <string.h>

#include "qrx_hfs.h"
#include "qrx_forkfile.h"
#include "qrx_hfsplus.h"
#include "qrx_macimage.h"
#include "qrx_mfs.h"

#define IMAGE_MAX (8u * 1024u * 1024u)
#define RSRC_MAX (64u * 1024u)
#define FORK_MAX (512u * 1024u)

typedef struct {
    uint8_t *data;
    uint32_t len;
} blob;

/* The HFS reader keeps one B-tree node as scratch; keep it off the stack. */
static qrx_hfs_t hfs_state;
static qrx_hfsp_t hfsp_state;

static uint8_t *load_file(const char *path, uint32_t max, uint32_t *len)
{
    BPTR file;
    LONG size;
    uint8_t *buffer;
    LONG got;

    *len = 0;
    file = Open((STRPTR)path, MODE_OLDFILE);
    if (file == 0)
        return NULL;
    if (Seek(file, 0, OFFSET_END) < 0) {
        Close(file);
        return NULL;
    }
    size = Seek(file, 0, OFFSET_BEGINNING);
    if (size <= 0 || (uint32_t)size > max) {
        Close(file);
        return NULL;
    }
    buffer = (uint8_t *)AllocMem((uint32_t)size, MEMF_ANY);
    if (buffer == NULL) {
        Close(file);
        return NULL;
    }
    got = Read(file, buffer, size);
    Close(file);
    if (got != size) {
        FreeMem(buffer, (uint32_t)size);
        return NULL;
    }
    *len = (uint32_t)size;
    return buffer;
}

static void blob_free(blob *b)
{
    if (b->data) {
        FreeMem(b->data, b->len);
        b->data = NULL;
        b->len = 0;
    }
}

static void put_code4(uint32_t code)
{
    char text[5];
    int i;

    for (i = 0; i < 4; i++) {
        uint8_t c = (uint8_t)(code >> (24 - 8 * i));
        text[i] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '.';
    }
    text[4] = '\0';
    Printf((STRPTR)"%s", (LONG)text);
}

static void print_info(const qrx_macimg_t *img, const qrx_mfs_t *mfs)
{
    Printf((STRPTR)"container=%s size=%ld ",
           (LONG)qrx_macimg_container_name(img->container), (LONG)img->size);
    if (img->container == QRX_MACIMG_DC42)
        Printf((STRPTR)"name=\"%s\" checksum=%s ", (LONG)img->name,
               (LONG)(img->checksum_ok ? "ok" : "bad"));
    if (img->container == QRX_MACIMG_NDIF)
        Printf((STRPTR)"name=\"%s\" blocks=%ld ", (LONG)img->name,
               (LONG)img->ndif_blocks);
    if (img->container == QRX_MACIMG_APM)
        Printf((STRPTR)"partitions=%ld ", (LONG)img->nparts);
    Printf((STRPTR)"volume=%s offset=%ld\n", (LONG)qrx_macimg_fs_name(img->fs),
           (LONG)img->volume_offset);
    if (mfs)
        Printf((STRPTR)"volume name=\"%s\" blocks=%ld alloc=%ld free=%ld files=%ld\n",
               (LONG)mfs->name, (LONG)mfs->alloc_blocks, (LONG)mfs->alloc_size,
               (LONG)mfs->free_blocks, (LONG)mfs->nfiles);
}

static void print_parts(const qrx_macimg_t *img)
{
    int i;

    for (i = 0; i < img->nparts; i++) {
        const qrx_macimg_part_t *part = &img->parts[i];
        Printf((STRPTR)"part %ld start=%ld count=%ld type=\"%s\" name=\"%s\"\n",
               (LONG)(i + 1), (LONG)part->start_block, (LONG)part->block_count,
               (LONG)part->type, (LONG)part->name);
    }
}

static void print_files(const qrx_mfs_t *mfs)
{
    uint16_t i;

    for (i = 0; i < mfs->nfiles; i++) {
        const qrx_mfs_file_t *file = &mfs->files[i];
        Printf((STRPTR)"file %ld \"%s\" type=", (LONG)(i + 1), (LONG)file->name);
        put_code4(file->type_code);
        Printf((STRPTR)" creator=");
        put_code4(file->creator);
        Printf((STRPTR)" data=%ld rsrc=%ld locked=%ld\n", (LONG)file->logical_len,
               (LONG)file->rsrc_len, (LONG)(file->flags & 1u));
    }
}

static void print_hfs_volume(const qrx_hfs_t *hfs)
{
    Printf((STRPTR)"volume fs=hfs name=\"%s\" blocks=%ld alloc=%ld free=%ld\n",
           (LONG)hfs->name, (LONG)hfs->alloc_blocks, (LONG)hfs->alloc_size,
           (LONG)hfs->free_blocks);
    Printf((STRPTR)"volume files=%ld dirs=%ld\n", (LONG)hfs->file_count,
           (LONG)hfs->dir_count);
}

static void print_hfs_entries(const qrx_hfs_t *hfs)
{
    uint16_t i;

    for (i = 0; i < hfs->nentries; i++) {
        const qrx_hfs_entry_t *entry = &hfs->entries[i];

        Printf((STRPTR)"entry %ld parent=%ld ", (LONG)(i + 1),
               (LONG)entry->parent_id);
        if (entry->is_dir) {
            Printf((STRPTR)"dir \"%s\" cnid=%ld icon=%ld\n", (LONG)entry->name,
                   (LONG)entry->cnid, (LONG)entry->icon_id);
        } else {
            Printf((STRPTR)"file \"%s\" cnid=%ld type=", (LONG)entry->name,
                   (LONG)entry->cnid);
            put_code4(entry->type_code);
            Printf((STRPTR)" creator=");
            put_code4(entry->creator);
            Printf((STRPTR)" flags=%ld icon=%ld data=%ld rsrc=%ld\n",
                   (LONG)entry->fd_flags, (LONG)entry->icon_id,
                   (LONG)entry->data_len, (LONG)entry->rsrc_len);
        }
    }
}

static void print_hfsp_volume(const qrx_hfsp_t *fs)
{
    Printf((STRPTR)"volume fs=hfs+ name=\"%s\" blocks=%ld blocksize=%ld "
                   "free=%ld\n",
           (LONG)fs->name, (LONG)fs->total_blocks, (LONG)fs->block_size,
           (LONG)fs->free_blocks);
    Printf((STRPTR)"volume files=%ld dirs=%ld journal=%ld wrapped=%ld\n",
           (LONG)fs->file_count, (LONG)fs->folder_count, (LONG)fs->journaled,
           (LONG)fs->wrapped);
}

static void print_hfsp_entries(const qrx_hfsp_t *fs)
{
    uint16_t i;

    for (i = 0; i < fs->nentries; i++) {
        const qrx_hfsp_entry_t *entry = &fs->entries[i];

        Printf((STRPTR)"entry %ld parent=%ld ", (LONG)(i + 1),
               (LONG)entry->parent_id);
        if (entry->is_dir) {
            Printf((STRPTR)"dir \"%s\" cnid=%ld icon=%ld\n", (LONG)entry->name,
                   (LONG)entry->cnid, (LONG)entry->icon_id);
        } else {
            Printf((STRPTR)"file \"%s\" cnid=%ld type=", (LONG)entry->name,
                   (LONG)entry->cnid);
            put_code4(entry->type_code);
            Printf((STRPTR)" creator=");
            put_code4(entry->creator);
            Printf((STRPTR)" flags=%ld icon=%ld data=%ld rsrc=%ld\n",
                   (LONG)entry->fd_flags, (LONG)entry->icon_id,
                   (LONG)entry->data_len, (LONG)entry->rsrc_len);
        }
    }
}

static int finish_fork(const char *name, uint8_t *fork, uint32_t alloc_size,
                       uint32_t written, const char *dest);

static const char *crc_text(uint8_t state)
{
    if (state == 1u)
        return "ok";
    if (state == 2u)
        return "bad";
    return "none";
}

static int cmd_forkfile(blob *file, const char *dest, int rsrc_flag)
{
    qrx_forkfile_t ff;
    uint32_t work_size = file->len * 2u + 4096u;
    uint8_t *work;
    int32_t status;

    work = (uint8_t *)AllocMem(work_size, MEMF_ANY);
    if (work == NULL) {
        Printf((STRPTR)"error=memory\n");
        return 20;
    }
    status = qrx_forkfile_open(&ff, file->data, file->len, work, work_size);
    if (status != QRX_MACIMG_OK) {
        Printf((STRPTR)"error=%s\n", (LONG)qrx_macimg_strerror(status));
        FreeMem(work, work_size);
        return 20;
    }
    Printf((STRPTR)"fork format=%s name=\"%s\" ",
           (LONG)qrx_forkfile_format_name(ff.format), (LONG)ff.name);
    Printf((STRPTR)"type=");
    put_code4(ff.type_code);
    Printf((STRPTR)" creator=");
    put_code4(ff.creator);
    Printf((STRPTR)" flags=%ld data=%ld rsrc=%ld crc=%s\n", (LONG)ff.fd_flags,
           (LONG)ff.data_len, (LONG)ff.rsrc_len, (LONG)crc_text(ff.crc_state));

    if (dest == NULL) {
        FreeMem(work, work_size);
        return 0;
    }
    {
        uint32_t size = rsrc_flag ? ff.rsrc_len : ff.data_len;
        uint32_t alloc_size;
        uint8_t *fork;

        if (size > FORK_MAX) {
            Printf((STRPTR)"error=too-large\n");
            FreeMem(work, work_size);
            return 20;
        }
        alloc_size = size ? size : 1u;
        fork = (uint8_t *)AllocMem(alloc_size, MEMF_ANY);
        if (fork == NULL) {
            Printf((STRPTR)"error=memory\n");
            FreeMem(work, work_size);
            return 20;
        }
        if (qrx_forkfile_read(&ff, rsrc_flag, 0, fork, size) != QRX_MACIMG_OK) {
            Printf((STRPTR)"error=read\n");
            FreeMem(fork, alloc_size);
            FreeMem(work, work_size);
            return 20;
        }
        FreeMem(work, work_size);
        return finish_fork(ff.name, fork, alloc_size, size, dest);
    }
}

static int usage(void)
{
    Printf((STRPTR)"Usage: QRXMacFS INFO <image> | LIST <image> [part] | "
                   "CAT <image> <name> [--rsrc] | "
                   "EXTRACT <image> <name> <dest> [--rsrc] | "
                   "FORK <container> [dest] [--rsrc]\n");
    return 20;
}

static int load_all(const char *path, blob *image, blob *rsrc, qrx_macimg_t *img)
{
    char sidecar[512];
    size_t n;
    int32_t status;

    image->data = load_file(path, IMAGE_MAX, &image->len);
    if (image->data == NULL) {
        Printf((STRPTR)"error=open\n");
        return 20;
    }
    rsrc->data = NULL;
    rsrc->len = 0;
    n = strlen(path);
    if (n + 6u < sizeof(sidecar)) {
        memcpy(sidecar, path, n);
        memcpy(sidecar + n, ".rsrc", 6u);
        rsrc->data = load_file(sidecar, RSRC_MAX, &rsrc->len);
    }

    status = qrx_macimg_open(img, image->data, image->len, rsrc->data, rsrc->len);
    if (status != QRX_MACIMG_OK) {
        Printf((STRPTR)"error=%s\n", (LONG)qrx_macimg_strerror(status));
        blob_free(rsrc);
        blob_free(image);
        return 20;
    }
    return 0;
}

static void report_image_error(int32_t status)
{
    Printf((STRPTR)"error=%s\n", (LONG)qrx_macimg_strerror(status));
}

static int cmd_list(blob *image, blob *rsrc, const char *path, int partition)
{
    qrx_macimg_t img;
    qrx_mfs_t mfs;
    qrx_mfs_t *mfs_ptr = NULL;
    qrx_hfs_t *hfs_ptr = NULL;
    qrx_hfsp_t *hfsp_ptr = NULL;
    int rc;

    (void)path;
    rc = 0;
    {
        int32_t status = qrx_macimg_open(&img, image->data, image->len,
                                         rsrc->data, rsrc->len);
        if (status != QRX_MACIMG_OK) {
            report_image_error(status);
            return 20;
        }
    }
    if (partition > 0) {
        if (qrx_macimg_select_partition(&img, partition) != QRX_MACIMG_OK) {
            Printf((STRPTR)"error=partition\n");
            return 20;
        }
    } else if (img.container == QRX_MACIMG_APM) {
        print_parts(&img);
    }
    if (img.fs == QRX_MACIMG_FS_MFS &&
        qrx_mfs_open(&mfs, image->data + img.volume_offset, img.volume_size) ==
            QRX_MACIMG_OK)
        mfs_ptr = &mfs;
    else if (img.fs == QRX_MACIMG_FS_HFS &&
             qrx_hfs_open(&hfs_state, image->data + img.volume_offset,
                          img.volume_size) == QRX_MACIMG_OK)
        hfs_ptr = &hfs_state;
    else if (img.fs == QRX_MACIMG_FS_HFSPLUS &&
             qrx_hfsp_open(&hfsp_state, image->data + img.volume_offset,
                           img.volume_size) == QRX_MACIMG_OK)
        hfsp_ptr = &hfsp_state;
    print_info(&img, mfs_ptr);
    if (mfs_ptr)
        print_files(mfs_ptr);
    if (hfs_ptr) {
        print_hfs_volume(hfs_ptr);
        print_hfs_entries(hfs_ptr);
    }
    if (hfsp_ptr) {
        print_hfsp_volume(hfsp_ptr);
        print_hfsp_entries(hfsp_ptr);
    }
    (void)rc;
    return 0;
}

static int cmd_info(blob *image, blob *rsrc)
{
    qrx_macimg_t img;
    qrx_mfs_t mfs;
    qrx_mfs_t *mfs_ptr = NULL;
    qrx_hfs_t *hfs_ptr = NULL;
    qrx_hfsp_t *hfsp_ptr = NULL;
    int32_t status;

    status = qrx_macimg_open(&img, image->data, image->len, rsrc->data,
                             rsrc->len);
    if (status != QRX_MACIMG_OK) {
        report_image_error(status);
        return 20;
    }
    if (img.fs == QRX_MACIMG_FS_MFS &&
        qrx_mfs_open(&mfs, image->data + img.volume_offset, img.volume_size) ==
            QRX_MACIMG_OK)
        mfs_ptr = &mfs;
    else if (img.fs == QRX_MACIMG_FS_HFS &&
             qrx_hfs_open(&hfs_state, image->data + img.volume_offset,
                          img.volume_size) == QRX_MACIMG_OK)
        hfs_ptr = &hfs_state;
    else if (img.fs == QRX_MACIMG_FS_HFSPLUS &&
             qrx_hfsp_open(&hfsp_state, image->data + img.volume_offset,
                           img.volume_size) == QRX_MACIMG_OK)
        hfsp_ptr = &hfsp_state;
    print_info(&img, mfs_ptr);
    if (hfs_ptr)
        print_hfs_volume(hfs_ptr);
    if (hfsp_ptr)
        print_hfsp_volume(hfsp_ptr);
    return 0;
}

static int finish_fork(const char *name, uint8_t *fork, uint32_t alloc_size,
                       uint32_t written, const char *dest)
{
    int rc = 0;

    if (dest == NULL) {
        if (written)
            Write(Output(), fork, (LONG)written);
    } else {
        BPTR out = Open((STRPTR)dest, MODE_NEWFILE);

        if (out == 0) {
            Printf((STRPTR)"error=dest\n");
            rc = 20;
        } else {
            if (written)
                Write(out, fork, (LONG)written);
            Close(out);
            Printf((STRPTR)"extracted=\"%s\" bytes=%ld\n", (LONG)name,
                   (LONG)written);
        }
    }
    FreeMem(fork, alloc_size);
    return rc;
}

static int hfs_fork(const blob *image, const qrx_macimg_t *img, const char *name,
                    int rsrc_flag, const char *dest)
{
    const qrx_hfs_entry_t *entry;
    uint8_t *fork;
    uint32_t size;
    uint32_t alloc_size;
    uint32_t written = 0;

    if (qrx_hfs_open(&hfs_state, image->data + img->volume_offset,
                     img->volume_size) != QRX_MACIMG_OK) {
        Printf((STRPTR)"error=no-hfs\n");
        return 20;
    }
    entry = qrx_hfs_lookup_path(&hfs_state, name);
    if (entry == NULL) {
        Printf((STRPTR)"error=not-found\n");
        return 20;
    }
    if (entry->is_dir) {
        Printf((STRPTR)"error=is-dir\n");
        return 20;
    }
    size = rsrc_flag ? entry->rsrc_len : entry->data_len;
    if (size > FORK_MAX) {
        Printf((STRPTR)"error=too-large\n");
        return 20;
    }
    alloc_size = size ? size : 1u;
    fork = (uint8_t *)AllocMem(alloc_size, MEMF_ANY);
    if (fork == NULL) {
        Printf((STRPTR)"error=memory\n");
        return 20;
    }
    if (qrx_hfs_read_fork(&hfs_state, entry, rsrc_flag, fork, size, &written) !=
        QRX_MACIMG_OK) {
        Printf((STRPTR)"error=read\n");
        FreeMem(fork, alloc_size);
        return 20;
    }
    return finish_fork(entry->name, fork, alloc_size, written, dest);
}

static int hfsp_fork(const blob *image, const qrx_macimg_t *img,
                     const char *name, int rsrc_flag, const char *dest)
{
    const qrx_hfsp_entry_t *entry;
    uint8_t *fork;
    uint32_t size;
    uint32_t alloc_size;
    uint32_t written = 0;

    if (qrx_hfsp_open(&hfsp_state, image->data + img->volume_offset,
                      img->volume_size) != QRX_MACIMG_OK) {
        Printf((STRPTR)"error=no-hfs+\n");
        return 20;
    }
    entry = qrx_hfsp_lookup_path(&hfsp_state, name);
    if (entry == NULL) {
        Printf((STRPTR)"error=not-found\n");
        return 20;
    }
    if (entry->is_dir) {
        Printf((STRPTR)"error=is-dir\n");
        return 20;
    }
    size = rsrc_flag ? entry->rsrc_len : entry->data_len;
    if (size > FORK_MAX) {
        Printf((STRPTR)"error=too-large\n");
        return 20;
    }
    alloc_size = size ? size : 1u;
    fork = (uint8_t *)AllocMem(alloc_size, MEMF_ANY);
    if (fork == NULL) {
        Printf((STRPTR)"error=memory\n");
        return 20;
    }
    if (qrx_hfsp_read_fork(&hfsp_state, entry, rsrc_flag, fork, size,
                           &written) != QRX_MACIMG_OK) {
        Printf((STRPTR)"error=read\n");
        FreeMem(fork, alloc_size);
        return 20;
    }
    return finish_fork(entry->name, fork, alloc_size, written, dest);
}

static int cmd_fork(blob *image, blob *rsrc, const char *name, int rsrc_flag,
                    const char *dest)
{
    qrx_macimg_t img;
    qrx_mfs_t mfs;
    const qrx_mfs_file_t *file = NULL;
    uint8_t *fork;
    uint32_t size;
    uint32_t written = 0;
    uint16_t i;
    int32_t status;

    status = qrx_macimg_open(&img, image->data, image->len, rsrc->data,
                             rsrc->len);
    if (status != QRX_MACIMG_OK) {
        report_image_error(status);
        return 20;
    }
    if (img.fs == QRX_MACIMG_FS_HFS)
        return hfs_fork(image, &img, name, rsrc_flag, dest);
    if (img.fs == QRX_MACIMG_FS_HFSPLUS)
        return hfsp_fork(image, &img, name, rsrc_flag, dest);
    if (img.fs != QRX_MACIMG_FS_MFS ||
        qrx_mfs_open(&mfs, image->data + img.volume_offset, img.volume_size) !=
            QRX_MACIMG_OK) {
        Printf((STRPTR)"error=no-mfs\n");
        return 20;
    }
    for (i = 0; i < mfs.nfiles; i++) {
        if (strcmp(mfs.files[i].name, name) == 0) {
            file = &mfs.files[i];
            break;
        }
    }
    if (file == NULL) {
        Printf((STRPTR)"error=not-found\n");
        return 20;
    }
    size = rsrc_flag ? file->rsrc_len : file->logical_len;
    if (size > FORK_MAX) {
        Printf((STRPTR)"error=too-large\n");
        return 20;
    }
    fork = (uint8_t *)AllocMem(size ? size : 1u, MEMF_ANY);
    if (fork == NULL) {
        Printf((STRPTR)"error=memory\n");
        return 20;
    }
    if (qrx_mfs_read_fork(&mfs, file, rsrc_flag, fork, size, &written) !=
        QRX_MACIMG_OK) {
        Printf((STRPTR)"error=read\n");
        FreeMem(fork, size ? size : 1u);
        return 20;
    }
    return finish_fork(file->name, fork, size ? size : 1u, written, dest);
}

int main(int argc, char **argv)
{
    blob image;
    blob rsrc;
    const char *action;
    int rsrc_flag = 0;
    int rc;
    int i;

    if (argc < 3)
        return usage();
    action = argv[1];
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--rsrc") == 0)
            rsrc_flag = 1;
    }
    if (strcmp(action, "INFO") != 0 && strcmp(action, "LIST") != 0 &&
        strcmp(action, "CAT") != 0 && strcmp(action, "EXTRACT") != 0 &&
        strcmp(action, "FORK") != 0)
        return usage();

    image.data = NULL;
    image.len = 0;
    rsrc.data = NULL;
    rsrc.len = 0;
    if (strcmp(action, "FORK") == 0) {
        /* A fork container is not a disk image: read it directly. */
        image.data = load_file(argv[2], IMAGE_MAX, &image.len);
        if (image.data == NULL) {
            Printf((STRPTR)"error=open\n");
            return 20;
        }
        rc = cmd_forkfile(&image,
                          (argc > 3 && argv[3][0] != '-') ? argv[3] : NULL,
                          rsrc_flag);
        blob_free(&image);
        return rc;
    }
    {
        qrx_macimg_t img;
        rc = load_all(argv[2], &image, &rsrc, &img);
    }
    if (rc == 0) {
        if (strcmp(action, "INFO") == 0)
            rc = cmd_info(&image, &rsrc);
        else if (strcmp(action, "LIST") == 0)
            rc = cmd_list(&image, &rsrc, argv[2],
                          (argc > 3 && argv[3] && argv[3][0] != '-')
                              ? atoi(argv[3])
                              : 0);
        else if (strcmp(action, "CAT") == 0)
            rc = (argc < 4) ? usage()
                            : cmd_fork(&image, &rsrc, argv[3], rsrc_flag, NULL);
        else
            rc = (argc < 5) ? usage()
                            : cmd_fork(&image, &rsrc, argv[3], rsrc_flag,
                                       argv[4]);
    }
    blob_free(&rsrc);
    blob_free(&image);
    return rc;
}
