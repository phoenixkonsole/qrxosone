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

struct Device *DiskImageBase;

int main(int argc, char **argv)
{
    struct MsgPort *port;
    struct IOStdReq *io;
    struct TagItem tags[2];
    STRPTR path = NULL;
    STRPTR mounted = NULL;
    BOOL writeprotect = FALSE;
    BOOL eject = FALSE;
    ULONG unit;
    LONG rc;
    LONG err;

    if (argc < 3) {
        PutStr((STRPTR)"Usage: QRXMountImage <unit> <file> | <unit> --eject\n");
        return 20;
    }
    unit = (ULONG)atoi(argv[1]);
    if (strcmp(argv[2], "--eject") == 0)
        eject = TRUE;
    else
        path = (STRPTR)argv[2];

    port = CreateMsgPort();
    if (port == NULL)
        return 20;
    io = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
    if (io == NULL) {
        DeleteMsgPort(port);
        return 20;
    }

    err = OpenDevice((STRPTR)"diskimage.device", unit, (struct IORequest *)io, 0);
    if (err != 0) {
        Printf((STRPTR)"QRXMountImage: cannot open diskimage.device unit %ld (error %ld)\n",
               (LONG)unit, err);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
    }

    DiskImageBase = (struct Device *)io->io_Device;
    tags[0].ti_Tag = DITAG_FILENAME;
    tags[0].ti_Data = eject ? (IPTR)NULL : (IPTR)path;
    tags[1].ti_Tag = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = UnitControlA(unit, tags);
    if (rc != 0) {
        Printf((STRPTR)"QRXMountImage: UnitControlA failed (error %ld)\n", rc);
        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
    }

    rc = UnitInfo(unit, &mounted, &writeprotect);
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

    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return 0;
}
