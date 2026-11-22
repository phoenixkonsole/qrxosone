/*
 * qrx_ipfs.h - QRX <-> IPFS client used by QRXIPFS, QRX Drive and the
 * IPFS drive sync daemon.
 *
 * The client talks to a local IPFS HTTP API (default 127.0.0.1:5001).
 * QRXIPFSD is the native 68K local node core; Kubo remains optional for
 * the public peer network.  The QRX applications are the desktop front end.
 */
#ifndef QRX_IPFS_H
#define QRX_IPFS_H

#include <exec/types.h>

#define QRX_IPFS_VERSION        "1.0"
#define QRX_IPFS_DEFAULT_API    "127.0.0.1:5001"
#define QRX_IPFS_VOLUME         "IPFS:"
#define QRX_IPFS_VOLUME_DIR     "SYS:Storage/IPFS"
#define QRX_IPFS_REVISION_LOG   "IPFS:REVISIONS"
#define QRX_IPFS_STATE_FILE     "IPFS:.qrx-ipfs-state"
#define QRX_IPFS_MAX_LINKS      512

/* One entry returned by /api/v0/ls. */
struct qrx_ipfs_link {
    char name[256];
    char cid[128];
    unsigned long size;
    int is_dir;
};

/* The warning shown before anything leaves the machine. */
#define QRX_IPFS_WARNING \
    "OEFFENTLICH! Alles, was nach IPFS geht, ist weltweit abrufbar. " \
    "Jede Datei erhaelt einen unveraenderlichen CID und kann von " \
    "jedem mit IPFS-Zugang gelesen werden - auch nach dem Loeschen " \
    "im lokalen Ordner. Keine Passwoerter oder privaten Daten ablegen!"

#define QRX_IPFS_PUBLIC_NOTE \
    "IPFS eignet sich zum Teilen von Dateien und zum Hosten " \
    "statischer Webseiten. Der CID bleibt dauerhaft gueltig."

/* --- lifecycle ---------------------------------------------------- */
int  qrx_ipfs_open(void);                /* open bsdsocket.library      */
void qrx_ipfs_close(void);
int  qrx_ipfs_configured(void);
int  qrx_ipfs_set_api(const char *host_port);   /* "host" or "host:port" */
const char *qrx_ipfs_api(void);
const char *qrx_ipfs_error(void);

/* Create SYS:Storage/IPFS and the IPFS: assign when missing. */
int  qrx_ipfs_ensure_volume(void);

/* --- daemon ------------------------------------------------------- */
/* Returns 1 when the API answers; fills agent/peer when buffers given. */
int  qrx_ipfs_status(char *agent, unsigned long agent_cap,
                     char *peer, unsigned long peer_cap);

/* --- add / fetch / pin -------------------------------------------- */
/* Add a file or, with recursive != 0, a whole drawer.
 * cid receives the last Hash returned by the daemon (the drawer CID for
 * recursive adds).  Returns 1 on success. */
int  qrx_ipfs_add(const char *path, int recursive, int pin,
                  char *cid, unsigned long cid_cap);

int  qrx_ipfs_cat(const char *cid, const char *dest);
/* List a directory CID.  Returns the number of entries, or -1 on error. */
int  qrx_ipfs_ls(const char *cid, struct qrx_ipfs_link *links, int max);
/* Fetch a file or whole UnixFS directory recursively. */
int  qrx_ipfs_get(const char *cid, const char *dest);
int  qrx_ipfs_pin_add(const char *cid);
int  qrx_ipfs_unpin(const char *cid);
int  qrx_ipfs_pin_ls(const char *cid);   /* 1 = pinned recursively */
int  qrx_ipfs_version(char *out, unsigned long cap);
int  qrx_ipfs_repo(unsigned long *bytes, unsigned long *objects);

/* --- revisions ---------------------------------------------------- */
/* Append "date  object  cid  bytes" to log_path (created when missing). */
int  qrx_ipfs_revision_append(const char *log_path, const char *object,
                              const char *cid, unsigned long bytes);

/* Tail of the revision log for the GUI.  Returns bytes copied. */
long qrx_ipfs_revision_tail(const char *log_path, char *out, unsigned long cap);

/* Publish every changed file below root and log a revision per file.
 * publish_all != 0 republishes unchanged files too and records a
 * drawer snapshot in the log.  Returns number of published files, or
 * -1 on error. */
int  qrx_ipfs_volume_sync(const char *root, const char *log_path,
                          int publish_all);

#endif
