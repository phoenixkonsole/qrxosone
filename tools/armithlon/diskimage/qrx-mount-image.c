/*
 * QRX OS One: insert or eject a disk image through diskimage.device.
 *
 * MountImage() currently returns ERROR_BAD_NUMBER on this 68k runtime, while
 * the lower-level UnitControlA() API works.  No MountDiskImage, host agent or
 * host filesystem is involved: the image is opened by the native device.
 */
#include <exec/exec.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/diskimage.h>
#include <stdlib.h>
#include <string.h>

/* DITAG_Filename is TAG_USER + 4 in <devices/diskimage.h>.  The SDK header
 * pulls in unavailable plugin headers, so keep the documented tag value here. */
#define DITAG_FILENAME (TAG_USER + 4)
#define DITAG_ERROR (TAG_USER + 1)
#define DITAG_SETDEVICETYPE (TAG_USER + 19)

struct Device *DiskImageBase;

static void mark(const char *s)
{
    Write(Output(), (APTR)s, (LONG)strlen(s));
}

int main(int argc, char **argv)
{
    struct MsgPort *port;
    struct IOStdReq *io;
    struct TagItem tags[4];
    STRPTR path = NULL;
    STRPTR mounted = NULL;
    BOOL writeprotect = FALSE;
    BOOL eject = FALSE;
    BOOL hold = FALSE;
    BOOL bare = FALSE;
    BOOL info_only = FALSE;
    ULONG unit;
    LONG rc;
    LONG err = 0;

    if (argc < 3) {
        PutStr((STRPTR)"Usage: QRXMountImage <unit> <file> [--hold] | <unit> --eject\n");
        return 20;
    }
    unit = (ULONG)atoi(argv[1]);
    if (strcmp(argv[2], "--eject") == 0)
        eject = TRUE;
    else
        path = (STRPTR)argv[2];
    if (argc > 3 && strcmp(argv[3], "--hold") == 0)
        hold = TRUE;
    if (argc > 3 && strcmp(argv[3], "--bare") == 0)
        bare = TRUE;
    if (argc > 3 && strcmp(argv[3], "--info") == 0)
        info_only = TRUE;

    mark("QMI: before port\n");
    port = CreateMsgPort();
    mark("QMI: after port\n");
    if (port == NULL)
        return 20;
    io = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
    if (io == NULL) {
        DeleteMsgPort(port);
        return 20;
    }

    mark("QMI: before open\n");
    err = OpenDevice((STRPTR)"diskimage.device", unit, (struct IORequest *)io, 0);
    mark("QMI: after open\n");
    if (err != 0) {
        Printf((STRPTR)"QRXMountImage: cannot open diskimage.device unit %ld (error %ld)\n",
               (LONG)unit, err);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
    }

    DiskImageBase = (struct Device *)io->io_Device;
    if (info_only) {
        mark("QMI: before UnitInfo-only\n");
        rc = UnitInfo(unit, &mounted, &writeprotect);
        mark("QMI: after UnitInfo-only\n");
        Printf((STRPTR)"QRXMountImage: UnitInfo rc=%ld mounted=%s wp=%ld\n",
               (LONG)rc, mounted ? mounted : (STRPTR)"<null>",
               (LONG)writeprotect);
        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 0;
    }
    /* Keep DITAG_Error first: the device retains its pointer between requests. */
    tags[0].ti_Tag = DITAG_ERROR;
    tags[0].ti_Data = (IPTR)&err;
    /* Unit 4 is the CD/DVD slot. The device defaults to direct access, which
     * makes ODFileSystem reject the 2048-byte optical sector size. */
    tags[1].ti_Tag = DITAG_SETDEVICETYPE;
    tags[1].ti_Data = (unit == 4 && !eject) ? 5 : 0; /* DG_CDROM / DG_DIRECT_ACCESS */
    tags[2].ti_Tag = DITAG_FILENAME;
    tags[2].ti_Data = eject ? (IPTR)NULL : (IPTR)path;
    tags[3].ti_Tag = TAG_DONE;
    tags[3].ti_Data = 0;
    if (bare) {
        tags[0].ti_Tag = DITAG_FILENAME;
        tags[0].ti_Data = (IPTR)path;
        tags[1].ti_Tag = TAG_DONE;
        tags[1].ti_Data = 0;
    }

    mark("QMI: before UnitControlA\n");
    rc = UnitControlA(unit, tags);
    mark("QMI: after UnitControlA\n");
    if (rc != 0 || err != 0) {
        Printf((STRPTR)"QRXMountImage: UnitControlA failed (error %ld, device error %ld)\n", rc, err);
        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
    }

    mark("QMI: before UnitInfo\n");
    rc = UnitInfo(unit, &mounted, &writeprotect);
    mark("QMI: after UnitInfo\n");
    if (eject) {
        Printf((STRPTR)"QRXMountImage: unit %ld ejected\n", (LONG)unit);
    } else if (rc == 0 && mounted != NULL) {
        Printf((STRPTR)"QRXMountImage: unit %ld <- %s%s\n",
               (LONG)unit, mounted, writeprotect ? (STRPTR)" (read-only)" : (STRPTR)"");
    } else {
        Printf((STRPTR)"QRXMountImage: unit %ld did not accept the image\n", (LONG)unit);
        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
    }

    if (hold) {
        /* Test/automation mode: keep the unit and image resident. */
        for (;;)
            Delay(100);
    }

    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return 0;
}
