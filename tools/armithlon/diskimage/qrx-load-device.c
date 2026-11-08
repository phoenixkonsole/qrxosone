/*
 * QRX OS One: keep DEVS:diskimage.device resident/open for the native mount
 * tools. The 68k AROS runtime used here does not resolve the disk device
 * through ramlib after the loader exits.
 */
#include <exec/exec.h>
#include <exec/io.h>
#include <exec/resident.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

static struct Resident *find_resident(UBYTE *base, ULONG size)
{
    ULONG off;
    for (off = 0; off + sizeof(struct Resident) < size; off += 2) {
        struct Resident *r = (struct Resident *)(base + off);
        if (r->rt_MatchWord == RTC_MATCHWORD &&
            r->rt_MatchTag == r &&
            (UBYTE *)r->rt_EndSkip > (UBYTE *)r &&
            r->rt_Name != NULL)
            return r;
    }
    return NULL;
}

int main(void)
{
    struct Resident *res;
    BPTR seg;
    UBYTE *base;
    struct MsgPort *port;
    struct IOStdReq *io;
    LONG err;

    if (FindResident((STRPTR)"diskimage.device") == NULL) {
        seg = LoadSeg((STRPTR)"DEVS:diskimage.device");
        if (seg == 0) {
            PutStr((STRPTR)"QRXLoadDevice: cannot load DEVS:diskimage.device\n");
            return 20;
        }
        base = (UBYTE *)BADDR(seg);
        res = find_resident(base, 0x8000);
        if (res == NULL) {
            PutStr((STRPTR)"QRXLoadDevice: no resident structure found\n");
            UnLoadSeg(seg);
            return 20;
        }
        if (InitResident(res, seg) == NULL &&
            FindResident((STRPTR)"diskimage.device") == NULL) {
            PutStr((STRPTR)"QRXLoadDevice: InitResident failed\n");
            UnLoadSeg(seg);
            return 20;
        }
    }

    port = CreateMsgPort();
    io = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
    if (io == NULL)
        return 20;
    err = OpenDevice((STRPTR)"diskimage.device", 0, (struct IORequest *)io, 0);
    if (err != 0) {
        PutStr((STRPTR)"QRXLoadDevice: cannot open diskimage.device unit 0\n");
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
    }

    for (;;)
        Delay(100);
}
