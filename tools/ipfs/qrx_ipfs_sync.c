/*
 * qrx_ipfs_sync.c - publish changes made on the IPFS: drive.
 *
 * Start from Startup-Sequence (Run >NIL: C:QRXIPFSSync) or manually.
 * Every file written through QRX Drive, the shell or any other program
 * is added to IPFS and logged in IPFS:REVISIONS.  Old versions stay
 * addressable by their immutable CIDs and pinned in the local daemon.
 *
 * Environment:
 *   QRX_IPFS_API         override daemon address (host:port)
 *   QRX_IPFS_SYNC_SECS   poll interval, default 10 seconds
 *   QRX_IPFS_SYNC_ALL    "1" publishes everything on start
 */
#include "qrx_ipfs.h"

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned long env_long(const char *name, unsigned long fallback)
{
    char buf[32];
    if (GetVar((CONST_STRPTR)name, (STRPTR)buf, sizeof(buf) - 1, GVF_GLOBAL_ONLY) > 0) {
        long v = atol(buf);
        if (v > 0) return (unsigned long)v;
    }
    return fallback;
}

int main(void)
{
    unsigned long interval = env_long("QRX_IPFS_SYNC_SECS", 10);
    char all[8];
    int publish_all = 0;
    struct Task *me = FindTask(NULL);
    ULONG sig = 0;

    if (GetVar((CONST_STRPTR)"QRX_IPFS_SYNC_ALL", (STRPTR)all, sizeof(all) - 1, GVF_GLOBAL_ONLY) > 0)
        publish_all = all[0] == '1' || all[0] == 'y' || all[0] == 'Y';

    /* Startup-Sequence runs before the network is necessarily up.  Wait for
     * the volume and bsdsocket.library instead of dying during boot. */
    SetSignal(0L, SIGBREAKF_CTRL_C);
    while (!qrx_ipfs_ensure_volume() || !qrx_ipfs_open()) {
        Printf((CONST_STRPTR)"QRXIPFSSync: warte auf IPFS:/Netzwerk: %s\n",
               qrx_ipfs_error());
        Delay(5 * 50UL);
        sig = SetSignal(0L, 0L);
        if (sig & SIGBREAKF_CTRL_C) return 12;
    }

    Printf((CONST_STRPTR)"QRXIPFSSync: IPFS: -> %s, alle %lu s\n",
           qrx_ipfs_api(), interval);
    Printf((CONST_STRPTR)"%s\n", QRX_IPFS_WARNING);
    for (;;) {
        int n = qrx_ipfs_volume_sync(QRX_IPFS_VOLUME, QRX_IPFS_REVISION_LOG, publish_all);
        if (n > 0)
            Printf((CONST_STRPTR)"QRXIPFSSync: %d Datei(en) veröffentlicht.\n", n);
        else if (n < 0)
            Printf((CONST_STRPTR)"QRXIPFSSync: %s\n", qrx_ipfs_error());
        publish_all = 0;

        Delay(interval * 50UL);
        sig = SetSignal(0L, 0L);
        if (sig & SIGBREAKF_CTRL_C) break;
        (void)me;
    }
    qrx_ipfs_close();
    Printf((CONST_STRPTR)"QRXIPFSSync beendet.\n");
    return 0;
}
