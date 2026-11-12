/*
 * qrx_volimage.c - encrypted QRX volume images.
 *
 * Format (big endian):
 *   0  "QRXVOL1"          8
 *   8  version            u32
 *  12  kdf iterations     u32
 *  16  username length    u32
 *  20  flags              u32  (1 = archive)
 *  24  salt               16
 *  40  iv                 16
 *  56  payload length     u32
 *  60  created days       u32
 *  64  reserved           32
 *  96  username           user_len bytes
 *      ciphertext         payload_len bytes
 *      hmac               32
 */
#include "qrx_volimage.h"
#include "qrx_crypto.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>
#include <stdio.h>

#define HDR_LEN      96
#define CHUNK        1024

struct volhdr {
    unsigned long version;
    unsigned long iterations;
    unsigned long user_len;
    unsigned long flags;
    unsigned char salt[16];
    unsigned char iv[16];
    unsigned long payload_len;
    unsigned long created;
};

struct writer {
    BPTR f;
    struct qrx_ctr ctr;
    struct qrx_hmac256 hmac;
    unsigned char buf[CHUNK];
    unsigned int used;
};

struct reader {
    BPTR f;
    struct qrx_ctr ctr;
    unsigned char buf[CHUNK];
    unsigned int pos, len;
};

static void set_err(char *err, unsigned long cap, const char *msg)
{
    if (!err || !cap) return;
    strncpy(err, msg ? msg : "", cap - 1);
    err[cap - 1] = 0;
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned long get32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static int hdr_read(BPTR f, struct volhdr *h, char *err, unsigned long cap)
{
    unsigned char b[HDR_LEN];

    if (Read(f, b, HDR_LEN) != HDR_LEN) {
        set_err(err, cap, "Kein gueltiges QRX-Volume (Header zu kurz).");
        return 0;
    }
    if (memcmp(b, QRX_VOL_MAGIC, 7) != 0) {
        set_err(err, cap, "Kein QRX-Volume-Image (Magic fehlt).");
        return 0;
    }
    h->version = get32(b + 8);
    h->iterations = get32(b + 12);
    h->user_len = get32(b + 16);
    h->flags = get32(b + 20);
    memcpy(h->salt, b + 24, 16);
    memcpy(h->iv, b + 40, 16);
    h->payload_len = get32(b + 56);
    h->created = get32(b + 60);
    if (h->version != QRX_VOL_VERSION || h->user_len > QRX_VOL_USER_MAX ||
        h->iterations < 1000 || h->payload_len > 0x70000000UL) {
        set_err(err, cap, "Volume-Header ungueltig oder beschaedigt.");
        return 0;
    }
    return 1;
}

static void derive(const struct volhdr *h, const char *user, const char *password,
                   unsigned char enc[32], unsigned char mac[32])
{
    unsigned char pass[256];
    unsigned char dk[64];
    unsigned long ul = (unsigned long)strlen(user);
    unsigned long pl = (unsigned long)strlen(password);

    if (ul > QRX_VOL_USER_MAX) ul = QRX_VOL_USER_MAX;
    if (pl > QRX_VOL_PASS_MAX) pl = QRX_VOL_PASS_MAX;
    memcpy(pass, user, ul);
    pass[ul] = 0;
    memcpy(pass + ul + 1, password, pl);
    qrx_pbkdf2_sha256(pass, ul + 1 + pl, h->salt, 16, h->iterations, dk, 64);
    memcpy(enc, dk, 32);
    memcpy(mac, dk + 32, 32);
}

/* ------------------------------------------------------------------ */
/* packing                                                             */
/* ------------------------------------------------------------------ */

static unsigned long payload_size(const char *full, const char *rel)
{
    BPTR lock = Lock((CONST_STRPTR)full, ACCESS_READ);
    struct FileInfoBlock *fib;
    unsigned long total = 0;

    if (!lock) return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) { UnLock(lock); return 0; }
    if (!Examine(lock, fib)) { FreeDosObject(DOS_FIB, fib); UnLock(lock); return 0; }
    if (fib->fib_DirEntryType > 0) {
        /* The drawer itself is one packed record, then its children. */
        total = 1 + 2 + 4 + 4 + 4 + 4 + (unsigned long)strlen(rel);
        while (ExNext(lock, fib)) {
            char child[420], child_rel[420];
            if (fib->fib_FileName[0] == '.') continue;
            snprintf(child, sizeof(child), "%s/%s", full, (char *)fib->fib_FileName);
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, (char *)fib->fib_FileName);
            total += payload_size(child, child_rel);
        }
    } else {
        total = 1 + 2 + 4 + 4 + 4 + 4 + (unsigned long)strlen(rel) +
                (unsigned long)fib->fib_Size;
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return total;
}

static int writer_open(struct writer *w, BPTR f, const unsigned char key[32],
                       const unsigned char iv[16], const unsigned char mac[32])
{
    memset(w, 0, sizeof(*w));
    w->f = f;
    qrx_ctr_init(&w->ctr, key, iv);
    qrx_hmac256_init(&w->hmac, mac, 32);
    return 1;
}

static int writer_flush(struct writer *w)
{
    if (w->used) {
        qrx_ctr_crypt(&w->ctr, w->buf, w->used);
        if (Write(w->f, w->buf, (LONG)w->used) != (LONG)w->used) return 0;
        qrx_hmac256_update(&w->hmac, w->buf, w->used);
        w->used = 0;
    }
    return 1;
}

static int writer_put(struct writer *w, const void *data, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)data;
    while (len) {
        unsigned long take = CHUNK - w->used;
        if (take > len) take = len;
        memcpy(w->buf + w->used, p, take);
        w->used += (unsigned int)take;
        p += take;
        len -= take;
        if (w->used == CHUNK && !writer_flush(w)) return 0;
    }
    return 1;
}

static void vol_temp_name(const char *image, char *out, unsigned long cap)
{
    const char *slash = strrchr(image, '/');
    const char *colon = strrchr(image, ':');
    const char *sep = NULL;

    if (slash) sep = slash;
    if (colon && (!sep || colon > sep)) sep = colon;
    if (sep)
        snprintf(out, cap, "%.*s/.qrxvol-tmp", (int)(sep - image + 1), image);
    else
        snprintf(out, cap, ".qrxvol-tmp");
}

static int pack_entry(const char *full, const char *rel, struct writer *w)
{
    BPTR lock = Lock((CONST_STRPTR)full, ACCESS_READ);
    struct FileInfoBlock *fib;
    unsigned char h[19];
    unsigned long nlen = (unsigned long)strlen(rel);
    int ok = 1;

    if (!lock) return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) { UnLock(lock); return 0; }
    if (!Examine(lock, fib)) { FreeDosObject(DOS_FIB, fib); UnLock(lock); return 0; }

    memset(h, 0, sizeof(h));
    h[0] = fib->fib_DirEntryType > 0 ? 2 : 1;
    h[1] = (unsigned char)(nlen >> 8);
    h[2] = (unsigned char)nlen;
    put32(h + 3, (unsigned long)(fib->fib_DirEntryType > 0 ? 0 : fib->fib_Size));
    put32(h + 7, (unsigned long)fib->fib_Date.ds_Days);
    put32(h + 11, (unsigned long)fib->fib_Date.ds_Minute);
    put32(h + 15, (unsigned long)fib->fib_Date.ds_Tick);
    if (!writer_put(w, h, sizeof(h)) || !writer_put(w, rel, nlen)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        return 0;
    }
    if (fib->fib_DirEntryType > 0) {
        while (ExNext(lock, fib)) {
            char child[420], child_rel[420];
            if (fib->fib_FileName[0] == '.') continue;
            snprintf(child, sizeof(child), "%s/%s", full, (char *)fib->fib_FileName);
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, (char *)fib->fib_FileName);
            if (!pack_entry(child, child_rel, w)) { ok = 0; break; }
        }
    } else {
        BPTR in = Open((CONST_STRPTR)full, MODE_OLDFILE);
        unsigned char buf[CHUNK];
        LONG got;
        unsigned long left = (unsigned long)fib->fib_Size;
        if (!in) ok = 0;
        while (ok && left > 0 && (got = Read(in, buf, (LONG)(left < CHUNK ? left : CHUNK))) > 0) {
            if (!writer_put(w, buf, (unsigned long)got)) ok = 0;
            left -= (unsigned long)got;
        }
        if (in) Close(in);
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return ok;
}

int qrx_volimage_create(const char *image, const char *source,
                        const char *user, const char *password,
                        char *err, unsigned long errcap)
{
    struct volhdr h;
    unsigned char enc[32], mac[32], hdrb[HDR_LEN], digest[32];
    struct writer w;
    BPTR out, lock;
    struct FileInfoBlock *fib;
    unsigned long psize;
    const char *base;
    char tmp[420];
    unsigned char end = 0;
    int ok = 1;

    if (!image || !source || !*user || !*password) {
        set_err(err, errcap, "Image, Quelle, Benutzer und Passwort sind Pflicht.");
        return 0;
    }
    if (!strcmp(image, source)) {
        set_err(err, errcap, "Quelle und Image duerfen nicht identisch sein.");
        return 0;
    }
    if (strlen(user) > QRX_VOL_USER_MAX || strlen(password) > QRX_VOL_PASS_MAX) {
        set_err(err, errcap, "Benutzername oder Passwort zu lang.");
        return 0;
    }
    lock = Lock((CONST_STRPTR)source, ACCESS_READ);
    if (!lock) { set_err(err, errcap, "Quelle nicht gefunden."); return 0; }
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib || !Examine(lock, fib)) {
        if (fib) FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        set_err(err, errcap, "Quelle nicht lesbar.");
        return 0;
    }
    base = source;
    {
        int i;
        for (i = (int)strlen(source) - 1; i >= 0; --i)
            if (source[i] == ':' || source[i] == '/') { base = source + i + 1; break; }
    }
    psize = 4 + payload_size(source, base) + 1;
    if (psize < 5 || psize > 0x70000000UL) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        set_err(err, errcap, "Quelle zu gross fuer ein QRX-Volume.");
        return 0;
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);

    memset(&h, 0, sizeof(h));
    h.version = QRX_VOL_VERSION;
    h.iterations = QRX_VOL_DEFAULT_ITER;
    h.user_len = (unsigned long)strlen(user);
    h.flags = 1;
    h.payload_len = psize;
    qrx_random_bytes(h.salt, 16);
    qrx_random_bytes(h.iv, 16);

    /* Build next to the destination so QRXIPFSSync never sees a partial
     * image.  The temporary name is hidden and therefore skipped by sync. */
    vol_temp_name(image, tmp, sizeof(tmp));
    out = Open((CONST_STRPTR)tmp, MODE_NEWFILE);
    if (!out) { set_err(err, errcap, "Image nicht schreibbar."); return 0; }

    memset(hdrb, 0, sizeof(hdrb));
    memcpy(hdrb, QRX_VOL_MAGIC, 7);
    put32(hdrb + 8, h.version);
    put32(hdrb + 12, h.iterations);
    put32(hdrb + 16, h.user_len);
    put32(hdrb + 20, h.flags);
    memcpy(hdrb + 24, h.salt, 16);
    memcpy(hdrb + 40, h.iv, 16);
    put32(hdrb + 56, h.payload_len);
    put32(hdrb + 60, (unsigned long)0);

    derive(&h, user, password, enc, mac);
    writer_open(&w, out, enc, h.iv, mac);
    qrx_hmac256_update(&w.hmac, hdrb, HDR_LEN);
    qrx_hmac256_update(&w.hmac, (const unsigned char *)user, h.user_len);
    if (Write(out, hdrb, HDR_LEN) != HDR_LEN ||
        Write(out, (APTR)user, (LONG)h.user_len) != (LONG)h.user_len)
        ok = 0;

    if (ok && !writer_put(&w, "QPAK", 4)) ok = 0;
    if (ok && !pack_entry(source, base, &w)) ok = 0;
    if (ok && !writer_put(&w, &end, 1)) ok = 0;
    if (ok && !writer_flush(&w)) ok = 0;
    if (ok) {
        qrx_hmac256_final(&w.hmac, digest);
        if (Write(out, digest, 32) != 32) ok = 0;
    }
    Close(out);
    if (!ok) {
        DeleteFile((CONST_STRPTR)tmp);
        set_err(err, errcap, "Schreiben des Volume-Images fehlgeschlagen.");
        return 0;
    }
    DeleteFile((CONST_STRPTR)image);
    if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)image)) {
        DeleteFile((CONST_STRPTR)tmp);
        set_err(err, errcap, "Image konnte nicht umbenannt werden.");
        return 0;
    }
    set_err(err, errcap, "");
    return 1;
}

/* ------------------------------------------------------------------ */
/* verification and extraction                                         */
/* ------------------------------------------------------------------ */

static int verify_stream(BPTR f, const struct volhdr *h, const char *user,
                         const char *password, unsigned char out_mac[32])
{
    unsigned char enc[32], mac[32];
    struct qrx_hmac256 hm;
    unsigned char buf[CHUNK], hdrb[HDR_LEN], userb[QRX_VOL_USER_MAX];
    unsigned long left, ul;

    derive(h, user, password, enc, mac);
    Seek(f, 0, OFFSET_BEGINNING);
    if (Read(f, hdrb, HDR_LEN) != HDR_LEN) return 0;
    ul = h->user_len;
    if (ul && Read(f, userb, (LONG)ul) != (LONG)ul) return 0;
    qrx_hmac256_init(&hm, mac, 32);
    qrx_hmac256_update(&hm, hdrb, HDR_LEN);
    if (ul) qrx_hmac256_update(&hm, userb, ul);
    left = h->payload_len;
    while (left) {
        LONG want = (LONG)(left < CHUNK ? left : CHUNK);
        LONG got = Read(f, buf, want);
        if (got <= 0) return 0;
        qrx_hmac256_update(&hm, buf, (unsigned long)got);
        left -= (unsigned long)got;
    }
    qrx_hmac256_final(&hm, out_mac);
    if (Read(f, buf, 32) != 32) return 0;
    return memcmp(out_mac, buf, 32) == 0;
}

int qrx_volimage_verify(const char *image, const char *user, const char *password,
                        char *err, unsigned long errcap)
{
    BPTR f;
    struct volhdr h;
    unsigned char mac[32];

    f = Open((CONST_STRPTR)image, MODE_OLDFILE);
    if (!f) { set_err(err, errcap, "Image nicht lesbar."); return 0; }
    if (!hdr_read(f, &h, err, errcap)) { Close(f); return 0; }
    if (h.user_len != strlen(user)) {
        Close(f);
        set_err(err, errcap, "Benutzername passt nicht zum Image.");
        return 0;
    }
    if (!verify_stream(f, &h, user, password, mac)) {
        Close(f);
        set_err(err, errcap, "Passwort falsch oder Image manipuliert (HMAC).");
        return 0;
    }
    Close(f);
    set_err(err, errcap, "");
    return 1;
}

static int reader_open(struct reader *r, BPTR f, const struct volhdr *h,
                       const unsigned char key[32], unsigned long offset)
{
    memset(r, 0, sizeof(*r));
    r->f = f;
    qrx_ctr_init(&r->ctr, key, h->iv);
    /* Skip offset bytes of keystream. */
    while (offset >= 16) {
        unsigned char skip[16];
        qrx_ctr_crypt(&r->ctr, skip, 16);
        offset -= 16;
    }
    if (offset) {
        unsigned char skip[16];
        qrx_ctr_crypt(&r->ctr, skip, 16);
        memcpy(r->ctr.stream, skip, 16);
        r->ctr.pos = (unsigned int)offset;
    }
    return 1;
}

static unsigned long reader_read(struct reader *r, void *out, unsigned long len)
{
    unsigned char *p = (unsigned char *)out;
    unsigned long done = 0;
    while (done < len) {
        if (r->pos >= r->len) {
            LONG got = Read(r->f, r->buf, CHUNK);
            if (got <= 0) break;
            r->len = (unsigned int)got;
            r->pos = 0;
            qrx_ctr_crypt(&r->ctr, r->buf, (unsigned long)got);
        }
        {
            unsigned long take = r->len - r->pos;
            if (take > len - done) take = len - done;
            memcpy(p + done, r->buf + r->pos, take);
            r->pos += (unsigned int)take;
            done += take;
        }
    }
    return done;
}

static int safe_name(const char *name, unsigned long len)
{
    unsigned long i = 0;

    if (!len || name[0] == '/' || name[0] == ':' || name[0] == '.' ||
        name[len - 1] == '/')
        return 0;
    while (i < len) {
        unsigned long start = i;
        unsigned long seglen;
        while (i < len && name[i] != '/') {
            if (name[i] == '\\' || name[i] == ':' || (unsigned char)name[i] < 32)
                return 0;
            ++i;
        }
        seglen = i - start;
        if (seglen == 0) return 0;
        if (seglen == 1 && name[start] == '.') return 0;
        if (seglen == 2 && name[start] == '.' && name[start + 1] == '.') return 0;
        if (i < len && name[i] == '/') ++i;
    }
    return 1;
}

int qrx_volimage_extract(const char *image, const char *dest,
                         const char *user, const char *password,
                         char *err, unsigned long errcap)
{
    BPTR f, lock;
    struct volhdr h;
    unsigned char enc[32], mac[32], check[32];
    struct reader r;
    unsigned long magic, files = 0;
    char curdir[420];

    f = Open((CONST_STRPTR)image, MODE_OLDFILE);
    if (!f) { set_err(err, errcap, "Image nicht lesbar."); return 0; }
    if (!hdr_read(f, &h, err, errcap)) { Close(f); return 0; }
    if (h.user_len != strlen(user) ||
        !verify_stream(f, &h, user, password, check)) {
        Close(f);
        set_err(err, errcap, "Passwort falsch oder Image manipuliert (HMAC).");
        return 0;
    }
    derive(&h, user, password, enc, mac);

    lock = CreateDir((CONST_STRPTR)dest);
    if (lock) UnLock(lock);
    lock = Lock((CONST_STRPTR)dest, ACCESS_READ);
    if (!lock) {
        Close(f);
        set_err(err, errcap, "Zielordner nicht anlegbar.");
        return 0;
    }
    UnLock(lock);
    strncpy(curdir, dest, sizeof(curdir) - 1);
    curdir[sizeof(curdir) - 1] = 0;

    Seek(f, HDR_LEN + (long)h.user_len, OFFSET_BEGINNING);
    reader_open(&r, f, &h, enc, 0);
    if (reader_read(&r, &magic, 4) != 4 || memcmp(&magic, "QPAK", 4) != 0) {
        Close(f);
        set_err(err, errcap, "Volume-Inhalt beschaedigt (QPAK).");
        return 0;
    }
    for (;;) {
        unsigned char eh[19];
        char name[QRX_VOL_USER_MAX + 200];
        unsigned long nlen, size, i;
        unsigned char type;
        char full[640];
        BPTR out;

        if (reader_read(&r, eh, 19) != 19) {
            Close(f);
            set_err(err, errcap, "Volume endet unerwartet.");
            return 0;
        }
        type = eh[0];
        if (type == 0) break;
        nlen = ((unsigned long)eh[1] << 8) | eh[2];
        size = get32(eh + 3);
        if ((type != 1 && type != 2) || nlen == 0 || nlen >= sizeof(name)) {
            Close(f);
            set_err(err, errcap, "Ungueltiger Eintrag im Volume.");
            return 0;
        }
        if (reader_read(&r, name, nlen) != nlen) {
            Close(f);
            set_err(err, errcap, "Eintragsname unlesbar.");
            return 0;
        }
        name[nlen] = 0;
        if (!safe_name(name, nlen)) {
            Close(f);
            set_err(err, errcap, "Unsicherer Pfad im Volume abgelehnt.");
            return 0;
        }
        snprintf(full, sizeof(full), "%s/%s", dest, name);
        if (type == 2) {
            BPTR d = CreateDir((CONST_STRPTR)full);
            if (d) UnLock(d);
        } else {
            unsigned char buf[CHUNK];
            out = Open((CONST_STRPTR)full, MODE_NEWFILE);
            if (!out) {
                Close(f);
                set_err(err, errcap, "Zieldatei nicht schreibbar.");
                return 0;
            }
            for (i = 0; i < size; ) {
                unsigned long take = size - i < CHUNK ? size - i : CHUNK;
                unsigned long got = reader_read(&r, buf, take);
                if (got != take) {
                    Close(out);
                    Close(f);
                    set_err(err, errcap, "Volume-Datenstrom beschaedigt.");
                    return 0;
                }
                if (Write(out, buf, (LONG)got) != (LONG)got) {
                    Close(out);
                    Close(f);
                    set_err(err, errcap, "Schreiben der Zieldatei fehlgeschlagen.");
                    return 0;
                }
                i += got;
            }
            Close(out);
            ++files;
        }
    }
    Close(f);
    (void)curdir;
    (void)files;
    set_err(err, errcap, "");
    return 1;
}

int qrx_volimage_info(const char *image, struct qrx_volinfo *info,
                      char *err, unsigned long errcap)
{
    BPTR f;
    struct volhdr h;

    if (!info) return 0;
    memset(info, 0, sizeof(*info));
    f = Open((CONST_STRPTR)image, MODE_OLDFILE);
    if (!f) { set_err(err, errcap, "Image nicht lesbar."); return 0; }
    if (!hdr_read(f, &h, err, errcap)) { Close(f); return 0; }
    if (Read(f, info->user, (LONG)h.user_len) != (LONG)h.user_len) {
        Close(f);
        set_err(err, errcap, "Benutzername unlesbar.");
        return 0;
    }
    info->user[h.user_len] = 0;
    info->iterations = h.iterations;
    info->payload_bytes = h.payload_len;
    Close(f);
    set_err(err, errcap, "");
    return 1;
}
