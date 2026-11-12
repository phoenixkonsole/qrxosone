/*
 * Temporary diagnostic: identify which diskimage.device command blocks.
 */
#include <exec/exec.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <devices/trackdisk.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

static void mark(const char *s)
{
    Write(Output(), (APTR)s, (LONG)strlen(s));
}

int main(int argc, char **argv)
{
    struct MsgPort *port;
    struct IOStdReq *io;
    struct DriveGeometry geom;
    uint8_t buf[2048];
    LONG rc;
    int open_only = (argc > 1 && strcmp(argv[1], "open") == 0);
    int read_only = (argc > 1 && strcmp(argv[1], "read") == 0);

    mark("PROBE: create port\n");
    port = CreateMsgPort();
    if (!port) return 20;
    io = (struct IOStdReq *)CreateIORequest(port, sizeof(*io));
    if (!io) return 20;

    mark("PROBE: before open\n");
    rc = OpenDevice((STRPTR)"diskimage.device", 0,
                    (struct IORequest *)io, 0);
    mark("PROBE: after open\n");
    if (rc != 0) return 20;
    if (open_only) {
        mark("PROBE: open-only done\n");
        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 0;
    }

    if (read_only) {
        memset(buf, 0, sizeof(buf));
        io->io_Command = CMD_READ;
        io->io_Data = buf;
        io->io_Length = sizeof(buf);
        io->io_Offset = 0;
        mark("PROBE: before read\n");
        rc = DoIO((struct IORequest *)io);
        Printf((STRPTR)"PROBE: read rc=%ld actual=%ld error=%ld sig=%02lx%02lx mdb=%02lx%02lx\n",
               (LONG)rc, (LONG)io->io_Actual, (LONG)io->io_Error,
               (unsigned long)buf[0], (unsigned long)buf[1],
               (unsigned long)buf[1024], (unsigned long)buf[1025]);
        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 0;
    }

    io->io_Command = TD_CHANGESTATE;
    io->io_Data = NULL;
    io->io_Length = 0;
    mark("PROBE: before changestate\n");
    rc = DoIO((struct IORequest *)io);
    mark("PROBE: after changestate\n");

    memset(&geom, 0, sizeof(geom));
    io->io_Command = TD_GETGEOMETRY;
    io->io_Data = &geom;
    io->io_Length = sizeof(geom);
    mark("PROBE: before getgeometry\n");
    rc = DoIO((struct IORequest *)io);
    mark("PROBE: after getgeometry\n");

    memset(buf, 0, sizeof(buf));
    io->io_Command = CMD_READ;
    io->io_Data = buf;
    io->io_Length = sizeof(buf);
    io->io_Offset = 0;
    mark("PROBE: before read\n");
    rc = DoIO((struct IORequest *)io);
    mark("PROBE: after read\n");

    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    mark("PROBE: done\n");
    return 0;
}
