/*
 * qrx_vol_cli.c - QRXVol command line tool.
 *
 *   QRXVol CREATE  <image.qrxvol> <source> [-user NAME]
 *   QRXVol INFO    <image.qrxvol>
 *   QRXVol VERIFY  <image.qrxvol> [-user NAME]
 *   QRXVol EXTRACT <image.qrxvol> <dest> [-user NAME]
 *
 * The password is taken from QRXVOL_PASSWORD when set (the GUI and QRX
 * Drive use this), otherwise the tool asks on the console.
 */
#include "qrx_volimage.h"

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>
#include <stdio.h>

static int read_password(char *buf, unsigned long cap)
{
    long got;
    char *p;

    buf[0] = 0;
    got = GetVar((CONST_STRPTR)"QRXVOL_PASSWORD", (STRPTR)buf, (LONG)cap - 1, GVF_GLOBAL_ONLY);
    if (got > 0) return 1;
    Printf((CONST_STRPTR)"Passwort: ");
    if (!FGets(Input(), (STRPTR)buf, (LONG)cap)) return 0;
    p = strchr(buf, '\n');
    if (p) *p = 0;
    p = strchr(buf, '\r');
    if (p) *p = 0;
    return buf[0] != 0;
}

static const char *option_user(int argc, char **argv, const char *fallback)
{
    int i;
    for (i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "-user") || !strcmp(argv[i], "--user"))
            return argv[i + 1];
    return fallback;
}

static void usage(void)
{
    Printf((CONST_STRPTR)
        "QRXVol " QRX_VOL_MAGIC " - verschluesselte QRX Volume Images\n"
        "  CREATE  <image.qrxvol> <quelle> [-user NAME]\n"
        "  INFO    <image.qrxvol>\n"
        "  VERIFY  <image.qrxvol> [-user NAME]\n"
        "  EXTRACT <image.qrxvol> <ziel> [-user NAME]\n"
        "Passwort: QRXVOL_PASSWORD oder Abfrage.\n");
}

int main(int argc, char **argv)
{
    char err[200];
    char password[QRX_VOL_PASS_MAX + 1];
    const char *user;

    if (argc < 3) { usage(); return 5; }

    if (!strcmp(argv[1], "INFO")) {
        struct qrx_volinfo info;
        if (!qrx_volimage_info(argv[2], &info, err, sizeof(err))) {
            Printf((CONST_STRPTR)"Fehler: %s\n", err);
            return 10;
        }
        Printf((CONST_STRPTR)"Benutzer: %s\nIterationen: %lu\nPayload: %lu Bytes\n",
               info.user, info.iterations, info.payload_bytes);
        return 0;
    }

    user = option_user(argc, argv, "qrx");
    if (!read_password(password, sizeof(password))) {
        Printf((CONST_STRPTR)"Fehler: kein Passwort.\n");
        return 11;
    }

    if (!strcmp(argv[1], "CREATE")) {
        if (!qrx_volimage_create(argv[2], argv[3], user, password, err, sizeof(err))) {
            Printf((CONST_STRPTR)"Fehler: %s\n", err);
            return 12;
        }
        Printf((CONST_STRPTR)"Volume erstellt: %s\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "VERIFY")) {
        if (!qrx_volimage_verify(argv[2], user, password, err, sizeof(err))) {
            Printf((CONST_STRPTR)"Fehler: %s\n", err);
            return 13;
        }
        Printf((CONST_STRPTR)"Volume OK: %s\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "EXTRACT")) {
        if (argc < 4) { usage(); return 5; }
        if (!qrx_volimage_extract(argv[2], argv[3], user, password, err, sizeof(err))) {
            Printf((CONST_STRPTR)"Fehler: %s\n", err);
            return 14;
        }
        Printf((CONST_STRPTR)"Volume entpackt nach: %s\n", argv[3]);
        return 0;
    }
    usage();
    return 5;
}
