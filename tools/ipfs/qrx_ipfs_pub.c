/*
 * qrx_ipfs_pub.c - complete command line client for QRX IPFS.
 *
 *   QRXIPFSPub STATUS
 *   QRXIPFSPub ADD   <datei|drawer>
 *   QRXIPFSPub CAT   <cid> <ziel>
 *   QRXIPFSPub LS    <cid>
 *   QRXIPFSPub GET   <cid> <ziel>
 *   QRXIPFSPub GETPIN <cid> <ziel>
 *   QRXIPFSPub PIN   <cid>
 *   QRXIPFSPub PINCID <cid> <ziel>
 *   QRXIPFSPub PINPATH <pfad>
 *   QRXIPFSPub UNPINPATH <pfad>
 *   QRXIPFSPub UNPIN <cid>
 *   QRXIPFSPub SYNC  [wurzel]
 *   QRXIPFSPub VERSION
 *   QRXIPFSPub REPO
 *   QRXIPFSPub WARN
 *
 * ADD writes a revision line to IPFS:REVISIONS.  The local QRXIPFSD node
 * listens on 127.0.0.1:5001 (override with QRX_IPFS_API).
 */
#include "qrx_ipfs.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>
#include <stdio.h>

static void usage(void)
{
    Printf((CONST_STRPTR)
        "QRXIPFSPub " QRX_IPFS_VERSION " - IPFS-Client\n"
        "  STATUS | ADD <pfad> | CAT <cid> <ziel>\n"
        "  LS <cid> | GET <cid> <ziel> | GETPIN <cid> <ziel>\n"
        "  PIN <cid> | PINCID <cid> <ziel> | PINPATH <pfad>\n"
        "  UNPIN <cid> | UNPINPATH <pfad>\n"
        "  SYNC [wurzel] | VERSION | REPO | WARN\n"
        "API: %s (QRX_IPFS_API)\n", qrx_ipfs_api());
}

#define PUB_PIN_MAP "IPFS:.qrx-node/path-pins"

static int pub_cid_ok(const char *cid)
{
    const char *p;
    size_t n;
    if (!cid) return 0;
    n = strlen(cid);
    if (n < 8 || n > 95) return 0;
    for (p = cid; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return 0;
    }
    return 1;
}

static int pub_path_ok(const char *path)
{
    return path && *path && !strchr(path, '\n') && !strchr(path, '\r') &&
           !strchr(path, '\t');
}

static void pub_pin_map_append(char sign, const char *cid, const char *path)
{
    BPTR f;
    if (!pub_path_ok(path)) return;
    if (sign == '+' && !pub_cid_ok(cid)) return;
    qrx_ipfs_ensure_volume();
    f = Open((CONST_STRPTR)PUB_PIN_MAP, MODE_READWRITE);
    if (!f) f = Open((CONST_STRPTR)PUB_PIN_MAP, MODE_NEWFILE);
    if (!f) return;
    Seek(f, 0, OFFSET_END);
    if (sign == '+')
        FPrintf(f, (CONST_STRPTR)"+%s\t%s\n", cid, path);
    else
        FPrintf(f, (CONST_STRPTR)"-\t%s\n", path);
    Close(f);
}

static int pub_pin_map_lookup(const char *path, char *cid, unsigned long cap)
{
    BPTR f;
    char line[640];
    int found = 0;
    if (!path || !cid || cap < 2) return 0;
    cid[0] = 0;
    f = Open((CONST_STRPTR)PUB_PIN_MAP, MODE_OLDFILE);
    if (!f) return 0;
    while (FGets(f, line, sizeof(line)))
    {
        char *tab;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if ((line[0] != '+' && line[0] != '-') ||
            !(tab = strchr(line, '\t')))
            continue;
        *tab = 0;
        if (strcmp(tab + 1, path)) continue;
        if (line[0] == '+') {
            strncpy(cid, line + 1, cap - 1);
            cid[cap - 1] = 0;
            found = 1;
        } else {
            cid[0] = 0;
            found = 0;
        }
    }
    Close(f);
    return found;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 5; }

    if (!strcmp(argv[1], "WARN")) {
        Printf((CONST_STRPTR)"%s\n\n%s\n", QRX_IPFS_WARNING, QRX_IPFS_PUBLIC_NOTE);
        return 0;
    }

    if (!strcmp(argv[1], "STATUS")) {
        char agent[64], peer[128];
        if (!qrx_ipfs_status(agent, sizeof(agent), peer, sizeof(peer))) {
            Printf((CONST_STRPTR)"IPFS nicht erreichbar: %s\n", qrx_ipfs_error());
            return 10;
        }
        Printf((CONST_STRPTR)"IPFS bereit\nAgent: %s\nPeer-ID: %s\nAPI: %s\n",
               agent, peer, qrx_ipfs_api());
        return 0;
    }

    if (!strcmp(argv[1], "ADD")) {
        char cid[128];
        BPTR lock;
        if (argc < 3) { usage(); return 5; }
        Printf((CONST_STRPTR)"%s\n", QRX_IPFS_WARNING);
        lock = Lock((CONST_STRPTR)argv[2], ACCESS_READ);
        if (!lock) {
            Printf((CONST_STRPTR)"Fehler: Pfad nicht gefunden: %s\n", argv[2]);
            return 11;
        }
        UnLock(lock);
        if (!qrx_ipfs_add(argv[2], 1, 1, cid, sizeof(cid))) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 12;
        }
        qrx_ipfs_ensure_volume();
        qrx_ipfs_revision_append(QRX_IPFS_REVISION_LOG, argv[2], cid, 0);
        pub_pin_map_append('+', cid, argv[2]);
        Printf((CONST_STRPTR)"CID: %s\nVeröffentlicht und gepinnt.\n", cid);
        return 0;
    }

    if (!strcmp(argv[1], "CAT")) {
        if (argc < 4) { usage(); return 5; }
        if (!qrx_ipfs_cat(argv[2], argv[3])) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 13;
        }
        Printf((CONST_STRPTR)"Gespeichert: %s\n", argv[3]);
        return 0;
    }

    if (!strcmp(argv[1], "LS")) {
        struct qrx_ipfs_link *links;
        int n, i;
        if (argc < 3) { usage(); return 5; }
        links = (struct qrx_ipfs_link *)AllocVec(
            sizeof(struct qrx_ipfs_link) * QRX_IPFS_MAX_LINKS, MEMF_ANY);
        if (!links) {
            Printf((CONST_STRPTR)"Fehler: kein Speicher.\n");
            return 16;
        }
        n = qrx_ipfs_ls(argv[2], links, QRX_IPFS_MAX_LINKS);
        if (n < 0) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            FreeVec(links);
            return 17;
        }
        Printf((CONST_STRPTR)"%d Eintrag/Eintraege in %s\n", n, argv[2]);
        for (i = 0; i < n; ++i)
            Printf((CONST_STRPTR)"%s %8lu  %s\n    %s\n",
                   links[i].is_dir ? "DIR " : "FILE", links[i].size,
                   links[i].name, links[i].cid);
        FreeVec(links);
        return 0;
    }

    if (!strcmp(argv[1], "GET")) {
        if (argc < 4) { usage(); return 5; }
        if (!qrx_ipfs_get(argv[2], argv[3])) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 18;
        }
        Printf((CONST_STRPTR)"Abgerufen: %s -> %s\n", argv[2], argv[3]);
        return 0;
    }

    if (!strcmp(argv[1], "GETPIN")) {
        if (argc < 4) { usage(); return 5; }
        if (!pub_cid_ok(argv[2]) || !pub_path_ok(argv[3])) {
            Printf((CONST_STRPTR)"Fehler: CID oder Ziel ungueltig.\n");
            return 22;
        }
        if (!qrx_ipfs_get(argv[2], argv[3])) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 18;
        }
        if (!qrx_ipfs_pin_add(argv[2])) {
            Printf((CONST_STRPTR)"Abgerufen, aber Pinning fehlgeschlagen: %s\n",
                   qrx_ipfs_error());
            return 23;
        }
        pub_pin_map_append('+', argv[2], argv[3]);
        Printf((CONST_STRPTR)"Abgerufen und gepinnt: %s -> %s\nCID: %s\n",
               argv[2], argv[3], argv[2]);
        return 0;
    }

    if (!strcmp(argv[1], "PINPATH")) {
        char cid[128];
        BPTR lock;
        if (argc < 3) { usage(); return 5; }
        if (!pub_path_ok(argv[2])) {
            Printf((CONST_STRPTR)"Fehler: Pfad ungueltig.\n");
            return 22;
        }
        lock = Lock((CONST_STRPTR)argv[2], ACCESS_READ);
        if (!lock) {
            Printf((CONST_STRPTR)"Fehler: Pfad nicht gefunden: %s\n", argv[2]);
            return 11;
        }
        UnLock(lock);
        if (!qrx_ipfs_add(argv[2], 1, 1, cid, sizeof(cid))) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 12;
        }
        qrx_ipfs_ensure_volume();
        qrx_ipfs_revision_append(QRX_IPFS_REVISION_LOG, argv[2], cid, 0);
        pub_pin_map_append('+', cid, argv[2]);
        Printf((CONST_STRPTR)"Gepinnt: %s\nCID: %s\n", argv[2], cid);
        return 0;
    }

    if (!strcmp(argv[1], "PINCID")) {
        if (argc < 4) { usage(); return 5; }
        if (!pub_cid_ok(argv[2]) || !pub_path_ok(argv[3])) {
            Printf((CONST_STRPTR)"Fehler: CID oder Ziel ungueltig.\n");
            return 22;
        }
        if (!qrx_ipfs_pin_add(argv[2])) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 14;
        }
        pub_pin_map_append('+', argv[2], argv[3]);
        Printf((CONST_STRPTR)"Gepinnt: %s\nCID: %s\n", argv[3], argv[2]);
        return 0;
    }

    if (!strcmp(argv[1], "UNPINPATH")) {
        char cid[128];
        if (argc < 3) { usage(); return 5; }
        if (!pub_pin_map_lookup(argv[2], cid, sizeof(cid))) {
            Printf((CONST_STRPTR)"Fehler: Kein Pin fuer %s gespeichert.\n", argv[2]);
            return 24;
        }
        if (!qrx_ipfs_unpin(cid)) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 19;
        }
        pub_pin_map_append('-', NULL, argv[2]);
        Printf((CONST_STRPTR)"Ungepinnt: %s\nCID: %s\n", argv[2], cid);
        return 0;
    }

    if (!strcmp(argv[1], "PIN")) {
        if (argc < 3) { usage(); return 5; }
        if (!qrx_ipfs_pin_add(argv[2])) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 14;
        }
        Printf((CONST_STRPTR)"Gepinnt: %s\n", argv[2]);
        return 0;
    }

    if (!strcmp(argv[1], "UNPIN")) {
        if (argc < 3) { usage(); return 5; }
        if (!qrx_ipfs_unpin(argv[2])) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 19;
        }
        Printf((CONST_STRPTR)"Ungepinnt: %s\n", argv[2]);
        return 0;
    }

    if (!strcmp(argv[1], "VERSION")) {
        char version[80];
        if (!qrx_ipfs_version(version, sizeof(version))) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 20;
        }
        Printf((CONST_STRPTR)"IPFS-Version: %s\n", version);
        return 0;
    }

    if (!strcmp(argv[1], "REPO")) {
        unsigned long bytes = 0, objects = 0;
        if (!qrx_ipfs_repo(&bytes, &objects)) {
            Printf((CONST_STRPTR)"Fehler: %s\n", qrx_ipfs_error());
            return 21;
        }
        Printf((CONST_STRPTR)"Repo: %lu Bytes, %lu Objekte\n", bytes, objects);
        return 0;
    }

    if (!strcmp(argv[1], "SYNC")) {
        const char *root = argc > 2 ? argv[2] : "IPFS:";
        int n;
        qrx_ipfs_ensure_volume();
        n = qrx_ipfs_volume_sync(root, QRX_IPFS_REVISION_LOG, 0);
        if (n < 0) {
            Printf((CONST_STRPTR)"Sync fehlgeschlagen: %s\n", qrx_ipfs_error());
            return 15;
        }
        Printf((CONST_STRPTR)"%d Datei(en) veröffentlicht.\n", n);
        return 0;
    }

    usage();
    return 5;
}
