/*
 * Host test: the 68k MacOS runtime must keep everything the Machine reports
 * inside the block that was actually allocated.
 *
 * The runtime falls back from 16 to 8 and 4 MiB because the guest has only a
 * few MiB free, and refuses the program when even 4 MiB are not enough.  MemTop (low memory 0x0108), BufPtr (0x010C), the zone base
 * (GetZone) and the NewPtr limit all have to follow that size; a hard-coded
 * 16 MiB would hand out addresses outside the block.
 *
 * The test builds a bare 'CODE' resource fork whose segment calls NewPtr and
 * GetZone, then loads it with a fixed memory budget.  Run it with
 * tools/qrxppc/test_m68k_heap.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "qrx_m68k.h"

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* CODE 0 is the jump table; this program calls no segment, so it is a NOP. */
static const uint8_t code0[] = { 0x4E, 0x71 };

/* CODE 1, the segment the runtime starts in. */
static const uint8_t code1[] = {
    0x20, 0x3C, 0x00, 0x00, 0x10, 0x00, /* move.l #$1000,d0  */
    0xA1, 0x1E,                         /* _NewPtr           */
    0xA1, 0x1A,                         /* _GetZone          */
    0x20, 0x3C, 0x00, 0x0F, 0xFF, 0xF0, /* move.l #$FFFF0,d0 */
    0xA1, 0x1E,                         /* _NewPtr           */
    0xA1, 0x1A,                         /* _GetZone          */
    0xA9, 0xF4                          /* _ExitToShell      */
};

/* Build a resource fork with CODE 0 and the given segment as CODE 1. */
/* Extra resource the test image can carry beside the 'CODE' segments. */
typedef struct {
    uint32_t type;
    int16_t id;
    const uint8_t *data;
    uint32_t len;
} test_res_t;

/* Build a resource fork with CODE 0, CODE 1 and the resources in extra[]. */
static uint32_t build_fork_ex(uint8_t *out, uint32_t cap, const uint8_t *seg,
                              uint32_t seg_len, const test_res_t *extra,
                              unsigned nextra)
{
    const uint32_t data_off = 0x0100u;
    uint32_t off0 = 0u;
    uint32_t off1 = 4u + (uint32_t)sizeof(code0);
    uint32_t data_len = off1 + 4u + seg_len;
    uint32_t extra_off[8];
    unsigned num_types = 1u + nextra;
    uint32_t refs_at, type_list_len, map_off, map_len, total, i;
    uint8_t *map, *type_list, *ref;

    if (nextra > 8u)
        return 0u;
    for (i = 0u; i < nextra; i++) {
        extra_off[i] = data_len;
        data_len += 4u + extra[i].len;
    }
    refs_at = 2u + num_types * 8u;
    type_list_len = refs_at + (2u + nextra) * 12u;
    map_off = data_off + data_len;
    map_len = 28u + type_list_len;
    total = map_off + map_len;
    if (total > cap)
        return 0u;
    memset(out, 0, total);

    /* Header of the fork. */
    wr32(out, data_off);
    wr32(out + 4u, map_off);
    wr32(out + 8u, data_len);
    wr32(out + 12u, map_len);

    /* Data area: every resource is a 4-byte length followed by its bytes. */
    wr32(out + data_off + off0, (uint32_t)sizeof(code0));
    memcpy(out + data_off + off0 + 4u, code0, sizeof(code0));
    wr32(out + data_off + off1, seg_len);
    memcpy(out + data_off + off1 + 4u, seg, seg_len);
    for (i = 0u; i < nextra; i++) {
        wr32(out + data_off + extra_off[i], extra[i].len);
        memcpy(out + data_off + extra_off[i] + 4u, extra[i].data, extra[i].len);
    }

    /* Resource map: header copy, one type per resource, then the references. */
    map = out + map_off;
    wr32(map, data_off);
    wr32(map + 4u, map_off);
    wr32(map + 8u, data_len);
    wr32(map + 12u, map_len);
    wr16(map + 24u, 28u);
    wr16(map + 26u, (uint16_t)(28u + type_list_len));

    type_list = map + 28u;
    wr16(type_list, (uint16_t)(num_types - 1u));
    wr32(type_list + 2u, 0x434F4445u);          /* 'CODE' */
    wr16(type_list + 6u, 1u);                   /* numRes - 1 */
    wr16(type_list + 8u, (uint16_t)refs_at);
    for (i = 0u; i < nextra; i++) {
        uint8_t *entry = type_list + 10u + i * 8u;

        wr32(entry, extra[i].type);
        wr16(entry + 4u, 0u);
        wr16(entry + 6u, (uint16_t)(refs_at + (2u + i) * 12u));
    }

    ref = type_list + refs_at;                  /* CODE 0, CODE 1 */
    wr16(ref, 0u);
    wr16(ref + 2u, 0xFFFFu);
    ref[5] = (uint8_t)(off0 >> 16);
    ref[6] = (uint8_t)(off0 >> 8);
    ref[7] = (uint8_t)off0;
    ref += 12u;
    wr16(ref, 1u);
    wr16(ref + 2u, 0xFFFFu);
    ref[5] = (uint8_t)(off1 >> 16);
    ref[6] = (uint8_t)(off1 >> 8);
    ref[7] = (uint8_t)off1;
    for (i = 0u; i < nextra; i++) {
        uint32_t off = extra_off[i];

        ref += 12u;
        wr16(ref, (uint16_t)extra[i].id);
        wr16(ref + 2u, 0xFFFFu);
        ref[5] = (uint8_t)(off >> 16);
        ref[6] = (uint8_t)(off >> 8);
        ref[7] = (uint8_t)off;
    }

    return total;
}

static uint32_t build_fork(uint8_t *out, uint32_t cap, const uint8_t *seg,
                           uint32_t seg_len)
{
    return build_fork_ex(out, cap, seg, seg_len, 0, 0u);
}

static int failures;

static void fail(uint32_t limit, const char *what, uint32_t got, uint32_t want)
{
    printf("FAIL %u KiB: %s = 0x%08X, expected 0x%08X\n", limit / 1024u, what,
           got, want);
    failures++;
}

/* Load the program with a fixed budget and check the whole layout.  With
 * use_default the runtime sizes itself (qrx_m68k_load), which has to end at
 * 16 MiB. */
static void one_case(const uint8_t *fork, uint32_t fork_len, uint32_t limit,
                     int use_default)
{
    static qrx_m68k_t m;
    uint32_t heap0, heap_end, memtop, bufptr;
    uint32_t p1, zone1, p2, zone2, want_heap;
    int fits, i, before = failures;
    qrx_m68k_status_t st;

    qrx_m68k_unload(&m);
    st = use_default ? qrx_m68k_load(&m, fork, fork_len)
                     : qrx_m68k_load_limited(&m, fork, fork_len, limit);
    if (st != QRX_M68K_OK) {
        printf("FAIL %u KiB: load = %s\n", limit / 1024u,
               qrx_m68k_status_name(st));
        failures++;
        return;
    }
    if (m.mem_size != limit)
        fail(limit, "mem_size", m.mem_size, limit);

    memtop = rd32(m.mem + 0x0108u);
    bufptr = rd32(m.mem + 0x010Cu);
    heap0 = m.heap;
    heap_end = m.heap_end;

    /* Nothing the Machine reports may point past the allocated block. */
    if (memtop != m.mem_size)
        fail(limit, "MemTop", memtop, m.mem_size);
    if (bufptr >= m.mem_size)
        fail(limit, "BufPtr", bufptr, m.mem_size - 0x1000u);
    if (heap0 != m.mem_size / 2u)
        fail(limit, "zone base", heap0, m.mem_size / 2u);
    if (heap_end != m.mem_size - 0x00100000u)
        fail(limit, "heap end", heap_end, m.mem_size - 0x00100000u);
    if (heap0 >= m.mem_size || heap_end > m.mem_size || heap_end <= heap0) {
        printf("FAIL %u KiB: heap 0x%08X..0x%08X outside the block\n",
               limit / 1024u, heap0, heap_end);
        failures++;
    }

    st = qrx_m68k_start(&m, 2000000u);
    if (st != QRX_M68K_EXITED) {
        printf("FAIL %u KiB: run = %s (%s)\n", limit / 1024u,
               qrx_m68k_status_name(st), m.detail);
        failures++;
        return;
    }

    /* NewPtr(0x1000) hands out the zone base; the second GetZone has moved. */
    fits = heap0 + 0x1000u + 0x000FFFF0u <= heap_end;
    p1 = 0u;
    zone1 = 0u;
    p2 = 0u;
    zone2 = 0u;
    for (i = 0; i < (int)m.ntrace; i++) {
        uint32_t trap = 0xA000u | m.trace[i].trap;

        if (trap == 0xA11Eu && m.trace[i].d0 == 0x1000u)
            p1 = m.trace[i].a0;
        else if (trap == 0xA11Eu && m.trace[i].d0 == 0x000FFFF0u)
            p2 = m.trace[i].a0;
        else if (trap == 0xA11Au && zone1 == 0u)
            zone1 = m.trace[i].a0;
        else if (trap == 0xA11Au)
            zone2 = m.trace[i].a0;
    }

    if (p1 != heap0)
        fail(limit, "NewPtr(1000)", p1, heap0);
    if (p1 != 0u && p1 + 0x1000u > m.mem_size)
        fail(limit, "NewPtr(1000) end", p1 + 0x1000u, m.mem_size);
    if (zone1 != heap0 + 0x1000u)
        fail(limit, "GetZone base", zone1, heap0 + 0x1000u);

    /* The 0xFFFF0 block only fits on a large machine; when it does not, NewPtr
     * must answer NULL instead of an address beyond the heap. */
    want_heap = fits ? p1 + 0x1000u + 0x000FFFF0u : heap0 + 0x1000u;
    if (fits && p2 == 0u)
        fail(limit, "NewPtr(FFFF0)", p2, want_heap);
    if (!fits && p2 != 0u) {
        printf("FAIL %u KiB: NewPtr(FFFF0) = 0x%08X above the heap\n",
               limit / 1024u, p2);
        failures++;
    }
    if (p2 != 0u && p2 + 0x000FFFF0u > heap_end) {
        printf("FAIL %u KiB: NewPtr(FFFF0) ends above the heap\n", limit / 1024u);
        failures++;
    }
    if (zone2 != want_heap)
        fail(limit, "GetZone after NewPtr", zone2, want_heap);

    printf("%5u KiB: MemTop=%08X zone=%08X heap end=%08X NewPtr(1000)=%08X "
           "NewPtr(FFFF0)=%08X %s\n",
           limit / 1024u, memtop, heap0, heap_end, p1, p2,
           failures == before ? "ok" : "FAILED");
}

/* A machine smaller than the segments need has to be refused, not silently
 * truncated. */
static void refuse_case(const uint8_t *fork, uint32_t fork_len, uint32_t limit)
{
    static qrx_m68k_t m;
    qrx_m68k_status_t st;

    qrx_m68k_unload(&m);
    st = qrx_m68k_load_limited(&m, fork, fork_len, limit);
    if (st != QRX_M68K_ERR_MEM) {
        printf("FAIL %u KiB: load = %s, expected ERR_MEM\n", limit / 1024u,
               qrx_m68k_status_name(st));
        failures++;
        return;
    }
    if (m.mem != 0) {
        printf("FAIL %u KiB: refused but still allocated a block\n",
               limit / 1024u);
        failures++;
        return;
    }
    printf("%5u KiB: refused with ERR_MEM ok\n", limit / 1024u);
}

/* CODE 1 that asks for a newer SysEnvirons than exists, then for version 2 and
 * Gestalt 'addr', and stores the version error where the test can read it. */
static const uint8_t code_sysenv[] = {
    0x20, 0x3C, 0x00, 0x00, 0x00, 0x03, /* move.l #3,d0        */
    0xA0, 0x90,                         /* _SysEnvirons        */
    0x21, 0xC0, 0x40, 0x30,             /* move.l d0,$4030.w   */
    0x20, 0x3C, 0x00, 0x00, 0x00, 0x02, /* move.l #2,d0        */
    0x41, 0xF8, 0x40, 0x00,             /* lea $4000.w,a0      */
    0xA0, 0x90,                         /* _SysEnvirons        */
    0x20, 0x3C, 0x61, 0x64, 0x64, 0x72, /* move.l #'addr',d0   */
    0x41, 0xF8, 0x40, 0x20,             /* lea $4020.w,a0      */
    0xA1, 0xAD,                         /* _Gestalt            */
    0xA9, 0xF4                          /* _ExitToShell        */
};

/* SysEnvRec has to describe the machine the runtime really is, and Gestalt
 * 'addr' has to switch with MemTop: 0 up to 16 MiB, 1 above. */
static void sysenv_case(uint32_t limit)
{
    static uint8_t img[1024];
    static qrx_m68k_t m;
    uint32_t len = build_fork(img, (uint32_t)sizeof(img), code_sysenv,
                              (uint32_t)sizeof(code_sysenv));
    uint32_t addr32, err;
    int before = failures;
    qrx_m68k_status_t st;

    qrx_m68k_unload(&m);
    st = qrx_m68k_load_limited(&m, img, len, limit);
    if (st != QRX_M68K_OK) {
        printf("FAIL %u KiB sysenv: load = %s\n", limit / 1024u,
               qrx_m68k_status_name(st));
        failures++;
        return;
    }
    st = qrx_m68k_start(&m, 2000000u);
    if (st != QRX_M68K_EXITED) {
        printf("FAIL %u KiB sysenv: run = %s (%s)\n", limit / 1024u,
               qrx_m68k_status_name(st), m.detail);
        failures++;
        return;
    }
    err = rd32(m.mem + 0x4030u);
    addr32 = rd32(m.mem + 0x4020u);
    if (err != 0xFFFFEA81u)
        fail(limit, "envVersTooBig", err, 0xFFFFEA81u);
    if (rd16(m.mem + 0x4000u) != 2u)
        fail(limit, "environsVersion", rd16(m.mem + 0x4000u), 2u);
    if (rd16(m.mem + 0x4002u) != QRX_M68K_MACHINE_TYPE)
        fail(limit, "machineType", rd16(m.mem + 0x4002u), QRX_M68K_MACHINE_TYPE);
    if (rd16(m.mem + 0x4004u) != 0x0810u)
        fail(limit, "systemVersion", rd16(m.mem + 0x4004u), 0x0810u);
    if (rd16(m.mem + 0x4006u) != 2u)
        fail(limit, "processor", rd16(m.mem + 0x4006u), 2u);
    if (m.mem[0x4008u] != 1u || m.mem[0x4009u] != 1u) {
        printf("FAIL %u KiB sysenv: hasFPU/hasColorQD = %u/%u\n", limit / 1024u,
               (unsigned)m.mem[0x4008u], (unsigned)m.mem[0x4009u]);
        failures++;
    }
    if (rd16(m.mem + 0x400Au) != 0u || rd16(m.mem + 0x400Cu) != 0u)
        fail(limit, "atDrvrVersNum", rd16(m.mem + 0x400Au), 0u);
    if (addr32 != (limit > 0x01000000u ? 1u : 0u))
        fail(limit, "Gestalt 'addr'", addr32,
             limit > 0x01000000u ? 1u : 0u);
    printf("%5u KiB: SysEnvirons 2, System 8.1.0, 68020, 'addr'=%u %s\n",
           limit / 1024u, addr32, failures == before ? "ok" : "FAILED");
}

/* CODE 1 that opens the application's resource fork, fetches 'CODE' 1 as a
 * resource, and exercises the handle allocators.  Results land in low memory so
 * the test can read them after the run. */
static const uint8_t code_res[0xC8] = {
    0x41, 0xF9, 0x00, 0x10, 0x00, 0xC0, /* 00 lea $1000C0.l,a0   */
    0xA9, 0x97,                         /* 06 _OpenResFile       */
    0x23, 0xC0, 0x00, 0x00, 0x41, 0x00, /* 08 move.l d0,$4100.l  */
    0x20, 0x3C, 0x43, 0x4F, 0x44, 0x45, /* 0E move.l #'CODE',d0  */
    0x3F, 0x3C, 0x00, 0x01,             /* 14 move.w #1,-(sp)    */
    0xA9, 0xA0,                         /* 18 _GetResource       */
    0x23, 0xC8, 0x00, 0x00, 0x41, 0x04, /* 1A move.l a0,$4104.l  */
    0x2F, 0x39, 0x00, 0x00, 0x41, 0x04, /* 20 move.l $4104.l,-(sp) */
    0xA0, 0x25,                         /* 26 _GetHandleSize     */
    0x23, 0xC0, 0x00, 0x00, 0x41, 0x08, /* 28 move.l d0,$4108.l  */
    0x2F, 0x3C, 0x00, 0x00, 0x01, 0x00, /* 2E move.l #$100,-(sp) */
    0xA1, 0x22,                         /* 34 _NewHandle         */
    0x23, 0xC8, 0x00, 0x00, 0x41, 0x0C, /* 36 move.l a0,$410C.l  */
    0x2F, 0x39, 0x00, 0x00, 0x41, 0x0C, /* 3C move.l $410C.l,-(sp) */
    0xA0, 0x25,                         /* 42 _GetHandleSize     */
    0x23, 0xC0, 0x00, 0x00, 0x41, 0x10, /* 44 move.l d0,$4110.l  */
    0x20, 0x79, 0x00, 0x00, 0x41, 0x0C, /* 4A movea.l $410C.l,a0 */
    0x22, 0x50,                         /* 50 movea.l (a0),a1    */
    0x12, 0xFC, 0x00, 0x5A,             /* 52 move.b #$5A,(a1)   */
    0x2F, 0x3C, 0x00, 0x00, 0x02, 0x00, /* 56 move.l #$200,-(sp) */
    0x2F, 0x39, 0x00, 0x00, 0x41, 0x0C, /* 5C move.l $410C.l,-(sp) */
    0xA0, 0x24,                         /* 62 _SetHandleSize     */
    0x2F, 0x39, 0x00, 0x00, 0x41, 0x0C, /* 64 move.l $410C.l,-(sp) */
    0xA0, 0x25,                         /* 6A _GetHandleSize     */
    0x23, 0xC0, 0x00, 0x00, 0x41, 0x14, /* 6C move.l d0,$4114.l  */
    0x20, 0x79, 0x00, 0x00, 0x41, 0x0C, /* 72 movea.l $410C.l,a0 */
    0x22, 0x50,                         /* 78 movea.l (a0),a1    */
    0x10, 0x11,                         /* 7A move.b (a1),d0     */
    0x23, 0xC0, 0x00, 0x00, 0x41, 0x18, /* 7C move.l d0,$4118.l  */
    0x2F, 0x39, 0x00, 0x00, 0x41, 0x0C, /* 82 move.l $410C.l,-(sp) */
    0xA0, 0x23,                         /* 88 _DisposeHandle     */
    0x20, 0x79, 0x00, 0x00, 0x41, 0x0C, /* 8A movea.l $410C.l,a0 */
    0x20, 0x10,                         /* 90 move.l (a0),d0     */
    0x23, 0xC0, 0x00, 0x00, 0x41, 0x1C, /* 92 move.l d0,$411C.l  */
    0xA9, 0xF4,                         /* 98 _ExitToShell       */
    [0xC0] = 0x07, 'T', 'e', 's', 't', 'R', 'e', 's'
};

/* Resource Manager and handle allocators: one resource handle per resource,
 * handle sizes follow NewHandle/SetHandleSize, data survives the resize and
 * DisposeHandle clears the master pointer. */
static void resmem_case(void)
{
    static uint8_t img[1024];
    static qrx_m68k_t m;
    uint32_t len = build_fork(img, (uint32_t)sizeof(img), code_res,
                              (uint32_t)sizeof(code_res));
    uint32_t res, handle;
    int before = failures;
    qrx_m68k_status_t st;

    qrx_m68k_unload(&m);
    st = qrx_m68k_load_limited(&m, img, len, 16u * 1024u * 1024u);
    if (st != QRX_M68K_OK) {
        printf("FAIL resource manager: load = %s\n", qrx_m68k_status_name(st));
        failures++;
        return;
    }
    st = qrx_m68k_start(&m, 2000000u);
    if (st != QRX_M68K_EXITED) {
        printf("FAIL resource manager: run = %s (%s)\n",
               qrx_m68k_status_name(st), m.detail);
        failures++;
        return;
    }
    res = rd32(m.mem + 0x4104u);
    handle = rd32(m.mem + 0x410Cu);
    if (rd32(m.mem + 0x4100u) != 0xFFFFFFFFu)
        fail(16u * 1024u * 1024u, "OpenResFile", rd32(m.mem + 0x4100u),
             0xFFFFFFFFu);
    if (res == 0u) {
        printf("FAIL resource manager: GetResource returned no handle\n");
        failures++;
    } else if (memcmp(m.mem + rd32(m.mem + res), code_res,
                      sizeof(code_res)) != 0) {
        printf("FAIL resource manager: resource handle has wrong contents\n");
        failures++;
    }
    if (rd32(m.mem + 0x4108u) != (uint32_t)sizeof(code_res))
        fail(16u * 1024u * 1024u, "resource size", rd32(m.mem + 0x4108u),
             (uint32_t)sizeof(code_res));
    if (handle == 0u) {
        printf("FAIL resource manager: NewHandle returned NULL\n");
        failures++;
    }
    if (rd32(m.mem + 0x4110u) != 0x100u)
        fail(16u * 1024u * 1024u, "NewHandle size", rd32(m.mem + 0x4110u), 0x100u);
    if (rd32(m.mem + 0x4114u) != 0x200u)
        fail(16u * 1024u * 1024u, "SetHandleSize", rd32(m.mem + 0x4114u), 0x200u);
    if ((rd32(m.mem + 0x4118u) & 0xFFu) != 0x5Au) {
        printf("FAIL resource manager: SetHandleSize lost the data\n");
        failures++;
    }
    if (rd32(m.mem + 0x411Cu) != 0u) {
        printf("FAIL resource manager: DisposeHandle left the master pointer\n");
        failures++;
    }
    printf("resource manager: OpenResFile -1, GetResource %08X (%u bytes), "
           "handles ok %s\n",
           res, rd32(m.mem + 0x4108u), failures == before ? "ok" : "FAILED");
}

/* ------------------------------------------------------- file manager (HLE) */

#define FS_NAME_OFF 0x140u
#define FS_DATA_OFF 0x150u
#define FS_BUF_OFF 0x160u
#define FS_PB 0x4200u
#define FS_SEG_LEN 0x170u

#define ABS32(a) (uint8_t)((a) >> 24), (uint8_t)((a) >> 16), \
                 (uint8_t)((a) >> 8), (uint8_t)(a)

/* CODE 1 that drives the File Manager traps: create, write, close, reopen,
 * read, GetEOF/GetFPos, seek and read again.  Every answer is stored in low
 * memory so the test can check it; the strings sit at fixed offsets. */
static uint32_t fs_program(uint8_t *out, uint32_t cap)
{
    uint32_t n = 0u;

#define E(...) do { \
        static const uint8_t bytes_[] = { __VA_ARGS__ }; \
        if (n + sizeof bytes_ > cap) return 0u; \
        memcpy(out + n, bytes_, sizeof bytes_); \
        n += (uint32_t)sizeof bytes_; \
    } while (0)

    E(0x41, 0xF9, ABS32(FS_PB));                 /* lea $4200.l,a0       */
    E(0x20, 0x3C, ABS32(0x100000u + FS_NAME_OFF)); /* move.l #name,d0    */
    E(0x21, 0x40, 0x00, 0x12);                   /* move.l d0,18(a0)     */
    E(0x20, 0x3C, 0x00, 0x00, 0x00, 0x02);       /* move.l #2,d0 (write) */
    E(0x2F, 0x08, 0xA0, 0x08);                   /* push a0; _Create     */
    E(0x70, 0x00);                               /* moveq #0,d0          */
    E(0x30, 0x28, 0x00, 0x10);                   /* move.w 16(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x00);       /* move.l d0,$4100.l    */
    E(0x70, 0x00);                               /* moveq #0,d0          */
    E(0x30, 0x28, 0x00, 0x18);                   /* move.w 24(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x04);       /* move.l d0,$4104.l    */
    E(0x20, 0x3C, ABS32(0x100000u + FS_DATA_OFF)); /* move.l #data,d0    */
    E(0x21, 0x40, 0x00, 0x20);                   /* move.l d0,32(a0)     */
    E(0x21, 0x7C, 0x00, 0x00, 0x00, 0x06, 0x00, 0x24); /* move.l #6,36(a0) */
    E(0x2F, 0x08, 0xA0, 0x03);                   /* push a0; _Write      */
    E(0x70, 0x00);                               /* moveq #0,d0          */
    E(0x30, 0x28, 0x00, 0x10);                   /* move.w 16(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x08);       /* move.l d0,$4108.l    */
    E(0x20, 0x28, 0x00, 0x28);                   /* move.l 40(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x0C);       /* move.l d0,$410C.l    */
    E(0x2F, 0x08, 0xA0, 0x01);                   /* push a0; _Close      */
    E(0x20, 0x3C, 0x00, 0x00, 0x00, 0x01);       /* move.l #1,d0 (read)  */
    E(0x2F, 0x08, 0xA0, 0x00);                   /* push a0; _Open       */
    E(0x70, 0x00);                               /* moveq #0,d0          */
    E(0x30, 0x28, 0x00, 0x10);                   /* move.w 16(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x10);       /* move.l d0,$4110.l    */
    E(0x70, 0x00);                               /* moveq #0,d0          */
    E(0x30, 0x28, 0x00, 0x18);                   /* move.w 24(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x14);       /* move.l d0,$4114.l    */
    E(0x20, 0x3C, ABS32(0x100000u + FS_BUF_OFF)); /* move.l #buf,d0      */
    E(0x21, 0x40, 0x00, 0x20);                   /* move.l d0,32(a0)     */
    E(0x21, 0x7C, 0x00, 0x00, 0x00, 0x10, 0x00, 0x24); /* move.l #16,36(a0) */
    E(0x2F, 0x08, 0xA0, 0x02);                   /* push a0; _Read       */
    E(0x20, 0x28, 0x00, 0x28);                   /* move.l 40(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x18);       /* move.l d0,$4118.l    */
    E(0x2F, 0x08, 0xA0, 0x16);                   /* push a0; _GetEOF     */
    E(0x70, 0x00);                               /* moveq #0,d0          */
    E(0x30, 0x28, 0x00, 0x10);                   /* move.w 16(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x1C);       /* move.l d0,$411C.l    */
    E(0x20, 0x28, 0x00, 0x20);                   /* move.l 32(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x20);       /* move.l d0,$4120.l    */
    E(0x2F, 0x08, 0xA0, 0x10);                   /* push a0; _GetFPos    */
    E(0x20, 0x28, 0x00, 0x24);                   /* move.l 36(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x24);       /* move.l d0,$4124.l    */
    E(0x31, 0x7C, 0x00, 0x01, 0x00, 0x2C);       /* move.w #1,44(a0)     */
    E(0x21, 0x7C, 0x00, 0x00, 0x00, 0x02, 0x00, 0x2E); /* move.l #2,46(a0) */
    E(0x2F, 0x08, 0xA0, 0x11);                   /* push a0; _SetFPos    */
    E(0x70, 0x00);                               /* moveq #0,d0          */
    E(0x30, 0x28, 0x00, 0x10);                   /* move.w 16(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x28);       /* move.l d0,$4128.l    */
    E(0x20, 0x3C, ABS32(0x100000u + FS_BUF_OFF)); /* move.l #buf,d0      */
    E(0x21, 0x40, 0x00, 0x20);                   /* move.l d0,32(a0)     */
    E(0x21, 0x7C, 0x00, 0x00, 0x00, 0x04, 0x00, 0x24); /* move.l #4,36(a0) */
    E(0x2F, 0x08, 0xA0, 0x02);                   /* push a0; _Read       */
    E(0x20, 0x28, 0x00, 0x28);                   /* move.l 40(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x2C);       /* move.l d0,$412C.l    */
    E(0x2F, 0x08, 0xA0, 0x10);                   /* push a0; _GetFPos    */
    E(0x20, 0x28, 0x00, 0x24);                   /* move.l 36(a0),d0     */
    E(0x23, 0xC0, 0x00, 0x00, 0x41, 0x30);       /* move.l d0,$4130.l    */
    E(0x2F, 0x08, 0xA0, 0x01);                   /* push a0; _Close      */
    E(0xA9, 0xF4);                               /* _ExitToShell         */
#undef E

    if (n > FS_NAME_OFF)
        return 0u;
    memset(out + n, 0, FS_SEG_LEN - n);
    out[FS_NAME_OFF] = 7;
    memcpy(out + FS_NAME_OFF + 1u, "out.txt", 7);
    memcpy(out + FS_DATA_OFF, "Hallo\n", 6);
    return FS_SEG_LEN;
}

/* stdio backend: the hooks the runtime expects, rooted at a temp directory. */
struct host_fs {
    char root[256];
    FILE *file[8];
};

static void host_path(struct host_fs *f, const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", f->root, path);
}

static long host_open(void *ctx, const char *path, int create)
{
    struct host_fs *f = (struct host_fs *)ctx;
    char full[512];
    FILE *fp;
    int i;

    host_path(f, path, full, sizeof full);
    fp = fopen(full, create ? "w+b" : "r+b");
    if (!fp && !create)
        fp = fopen(full, "rb");
    if (!fp)
        return -1;
    for (i = 0; i < 8; i++)
        if (!f->file[i]) {
            f->file[i] = fp;
            return i;
        }
    fclose(fp);
    return -1;
}

static long host_read(void *ctx, long fd, void *buf, uint32_t len)
{
    struct host_fs *f = (struct host_fs *)ctx;

    return (long)fread(buf, 1, len, f->file[fd]);
}

static long host_write(void *ctx, long fd, const void *buf, uint32_t len)
{
    struct host_fs *f = (struct host_fs *)ctx;

    return (long)fwrite(buf, 1, len, f->file[fd]);
}

static long host_seek(void *ctx, long fd, int32_t offset, int whence)
{
    struct host_fs *f = (struct host_fs *)ctx;
    int mode = whence == 0 ? SEEK_SET : whence == 2 ? SEEK_END : SEEK_CUR;

    if (fseek(f->file[fd], offset, mode) != 0)
        return -1;
    return (long)ftell(f->file[fd]);
}

static void host_close(void *ctx, long fd)
{
    struct host_fs *f = (struct host_fs *)ctx;

    fclose(f->file[fd]);
    f->file[fd] = 0;
}

/* Create, write, reopen, read, seek: the guest sees a file, the host sees the
 * bytes on disk. */
static void fs_case(void)
{
    static uint8_t seg[FS_SEG_LEN];
    static uint8_t img[1024];
    static qrx_m68k_t m;
    static struct host_fs f;
    qrx_m68k_fs_t hooks;
    uint32_t len;
    char file[512], on_disk[16];
    FILE *fp;
    size_t got = 0u;
    int before = failures;
    qrx_m68k_status_t st;

    memset(&f, 0, sizeof f);
    snprintf(f.root, sizeof f.root, "/tmp/qrx-m68k-fs-%ld", (long)getpid());
    mkdir(f.root, 0700);

    if (fs_program(seg, (uint32_t)sizeof seg) == 0u) {
        printf("FAIL file manager: the segment does not fit\n");
        failures++;
        return;
    }
    len = build_fork(img, (uint32_t)sizeof(img), seg, (uint32_t)sizeof seg);
    if (len == 0u) {
        printf("FAIL file manager: could not build the image\n");
        failures++;
        return;
    }
    hooks.ctx = &f;
    hooks.open = host_open;
    hooks.read = host_read;
    hooks.write = host_write;
    hooks.seek = host_seek;
    hooks.close = host_close;

    qrx_m68k_unload(&m);
    st = qrx_m68k_load_limited(&m, img, len, 16u * 1024u * 1024u);
    if (st != QRX_M68K_OK) {
        printf("FAIL file manager: load = %s\n", qrx_m68k_status_name(st));
        failures++;
        return;
    }
    qrx_m68k_set_fs(&m, &hooks);
    st = qrx_m68k_start(&m, 2000000u);
    if (st != QRX_M68K_EXITED) {
        printf("FAIL file manager: run = %s (%s)\n",
               qrx_m68k_status_name(st), m.detail);
        failures++;
        return;
    }

    if (rd32(m.mem + 0x4100u) != 0u)
        fail(16u * 1024u * 1024u, "Create ioResult", rd32(m.mem + 0x4100u), 0u);
    if (rd32(m.mem + 0x4104u) != 1u)
        fail(16u * 1024u * 1024u, "Create refNum", rd32(m.mem + 0x4104u), 1u);
    if (rd32(m.mem + 0x4108u) != 0u)
        fail(16u * 1024u * 1024u, "Write ioResult", rd32(m.mem + 0x4108u), 0u);
    if (rd32(m.mem + 0x410Cu) != 6u)
        fail(16u * 1024u * 1024u, "Write ioActCount", rd32(m.mem + 0x410Cu), 6u);
    if (rd32(m.mem + 0x4110u) != 0u)
        fail(16u * 1024u * 1024u, "Open ioResult", rd32(m.mem + 0x4110u), 0u);
    if (rd32(m.mem + 0x4114u) != 1u)
        fail(16u * 1024u * 1024u, "Open refNum", rd32(m.mem + 0x4114u), 1u);
    if (rd32(m.mem + 0x4118u) != 6u)
        fail(16u * 1024u * 1024u, "Read ioActCount", rd32(m.mem + 0x4118u), 6u);
    if (rd32(m.mem + 0x411Cu) != 0u)
        fail(16u * 1024u * 1024u, "GetEOF ioResult", rd32(m.mem + 0x411Cu), 0u);
    if (rd32(m.mem + 0x4120u) != 6u)
        fail(16u * 1024u * 1024u, "GetEOF ioEOF", rd32(m.mem + 0x4120u), 6u);
    if (rd32(m.mem + 0x4124u) != 6u)
        fail(16u * 1024u * 1024u, "GetFPos", rd32(m.mem + 0x4124u), 6u);
    if (rd32(m.mem + 0x4128u) != 0u)
        fail(16u * 1024u * 1024u, "SetFPos ioResult", rd32(m.mem + 0x4128u), 0u);
    if (rd32(m.mem + 0x412Cu) != 4u)
        fail(16u * 1024u * 1024u, "Read after SetFPos", rd32(m.mem + 0x412Cu), 4u);
    if (rd32(m.mem + 0x4130u) != 6u)
        fail(16u * 1024u * 1024u, "GetFPos after read", rd32(m.mem + 0x4130u), 6u);
    if (memcmp(m.mem + 0x100160u, "llo\n", 4) != 0) {
        printf("FAIL file manager: second read gave %02X %02X %02X %02X at 0x100160\n",
               m.mem[0x100160u], m.mem[0x100161u], m.mem[0x100162u],
               m.mem[0x100163u]);
        failures++;
    }

    snprintf(file, sizeof file, "%s/out.txt", f.root);
    fp = fopen(file, "rb");
    if (fp) {
        got = fread(on_disk, 1, sizeof on_disk, fp);
        fclose(fp);
    }
    if (got != 6u || memcmp(on_disk, "Hallo\n", 6) != 0) {
        printf("FAIL file manager: %s holds %u bytes, expected 6\n", file,
               (unsigned)got);
        failures++;
    }
    remove(file);

    printf("file manager: create/write/open/read/seek on %s %s\n", f.root,
           failures == before ? "ok" : "FAILED");
}

/* ------------------------------------------------- QuickDraw, dialog, event */

/* A DLOG and the DITL it names: a text line and a button. */
static const uint8_t dlog_res[] = {
    0x00, 0x00,             /* procID: no WDEF */
    0x00, 0x01,             /* visible */
    0x00,                   /* filler */
    0x00,                   /* goAway flag */
    0x00, 0x00, 0x00, 0x00, /* refCon */
    0x00, 0x80,             /* itemsID 128 */
    0x00, 0x00              /* titleID: none */
};

static const uint8_t ditl_res[] = {
    0x00, 0x01,                                     /* numItems - 1 */
    0x00, 0x00, 0x00, 0x00,                         /* handle */
    0x00, 0x0A, 0x00, 0x0A, 0x00, 0x14, 0x00, 0x64, /* 10,10,20,100 */
    0x08, 0x05, 'R', 'e', 'a', 'd', 'y',            /* statText */
    0x00, 0x00, 0x00, 0x00,                         /* handle */
    0x00, 0x1E, 0x00, 0x0A, 0x00, 0x2D, 0x00, 0x3C, /* 30,10,45,60 */
    0x04, 0x02, 'G', 'o'                            /* button */
};

#define QD_PT1 0x290u
#define QD_PT2 0x294u
#define QD_RECT1 0x298u
#define QD_PAT 0x2A0u
#define QD_STR 0x2A8u
#define QD_RECT2 0x2ACu
#define QD_ALERT 0x2B4u
#define QD_MODAL 0x2B6u
#define QD_EV_NONE 0x2B8u
#define QD_EV_POST 0x2BAu
#define QD_TICKS 0x2BCu
#define QD_BUTTON 0x2BEu
#define QD_BUTTON2 0x2C0u
#define QD_HIT 0x2C2u
#define QD_DIALOG 0x2C4u
#define QD_EVT 0x2C8u
#define QD_PTOUT 0x2D8u
#define QD_HIT2 0x2DCu
#define QD_CPART 0x2DEu
#define QD_ISEV 0x2E0u
#define QD_CPT 0x2E4u
#define QD_CFIND 0x2E8u
#define QD_DLG2 0x2ECu
#define QD_TAB2 0x2F0u
#define QD_SEG_LEN 0x300u

/* CODE 1 that drives the QuickDraw, Dialog and Event traps and stores every
 * answer in low memory. */
static uint32_t qd_program(uint8_t *out, uint32_t cap)
{
    uint32_t n = 0u;

#define E(...) do { \
        static const uint8_t bytes_[] = { __VA_ARGS__ }; \
        if (n + sizeof bytes_ > cap) return 0u; \
        memcpy(out + n, bytes_, sizeof bytes_); \
        n += (uint32_t)sizeof bytes_; \
    } while (0)

    E(0x41, 0xF9, ABS32(0x100000u + QD_PT1));    /* lea pt1,a0        */
    E(0xA8, 0x93);                               /* _MoveTo           */
    E(0x41, 0xF9, ABS32(0x100000u + QD_PT2));    /* lea pt2,a0        */
    E(0xA8, 0x91);                               /* _LineTo           */
    E(0x30, 0x3C, 0x00, 0x02);                   /* move.w #2,d0       */
    E(0x32, 0x3C, 0x00, 0x03);                   /* move.w #3,d1       */
    E(0xA8, 0x9B);                               /* _PenSize          */
    E(0x41, 0xF9, ABS32(0x100000u + QD_RECT1));  /* lea rect1,a0      */
    E(0x22, 0x7C, ABS32(0x100000u + QD_PAT));    /* move.l #pat,a1    */
    E(0xA8, 0xA5);                               /* _FillRect         */
    E(0x41, 0xF9, ABS32(0x100000u + QD_RECT1));
    E(0xA8, 0xA1);                               /* _FrameRect        */
    E(0x30, 0x3C, 0x00, 0x0C);                   /* move.w #12,d0     */
    E(0xA8, 0x8A);                               /* _TextSize         */
    E(0x30, 0x3C, 0x00, 0x04);                   /* move.w #4,d0      */
    E(0xA8, 0x87);                               /* _TextFont         */
    E(0x41, 0xF9, ABS32(0x100000u + QD_STR));    /* lea str,a0        */
    E(0xA8, 0x84);                               /* _DrawString       */
    E(0x20, 0x7C, 0x00, 0x00, 0x00, 0x00);       /* move.l #0,a0      */
    E(0xA8, 0x96);                               /* _HidePen          */
    E(0xA8, 0x97);                               /* _ShowPen          */
    E(0x41, 0xF9, ABS32(0x100000u + QD_RECT2));
    E(0x30, 0x3C, 0x00, 0x01);                   /* move.w #1,d0      */
    E(0x32, 0x3C, 0x00, 0x02);                   /* move.w #2,d1      */
    E(0xA8, 0xA8);                               /* _OffsetRect       */
    E(0x41, 0xF9, ABS32(0x100000u + QD_RECT2));
    E(0xA8, 0xA3);                               /* _EraseRect        */
    E(0x30, 0x3C, 0x00, 0x80);                   /* move.w #128,d0    */
    E(0x20, 0x7C, 0x00, 0x00, 0x00, 0x00);
    E(0xA9, 0x85);                               /* _Alert            */
    E(0x41, 0xF9, ABS32(0x100000u + QD_ALERT));
    E(0x30, 0x80);                               /* move.w d0,(a0)    */
    E(0x30, 0x3C, 0x00, 0x80);                   /* move.w #128,d0    */
    E(0x20, 0x7C, 0x00, 0x00, 0x00, 0x00);
    E(0x22, 0x7C, 0x00, 0x00, 0x00, 0x00);
    E(0xA9, 0x7C);                               /* _GetNewDialog     */
    E(0x41, 0xF9, ABS32(0x100000u + QD_DIALOG));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x20, 0x40);                               /* move.l d0,a0      */
    E(0xA9, 0x81);                               /* _DrawDialog       */
    /* The DITL entries are controls now: a click on the "Go" button has to
     * come back as item 2, and FindControl sees the same button. */
    E(0x70, 0x00);                               /* moveq #0,d0       */
    E(0x41, 0xF9, ABS32(0x100000u + QD_HIT));
    E(0x22, 0x48);                               /* move.l a0,a1      */
    E(0xA9, 0x91);                               /* _ModalDialog      */
    E(0x30, 0x3C, 0x00, 0x01);                   /* move.w #1,d0      */
    E(0x41, 0xF9, ABS32(0x100000u + QD_EVT));
    E(0xA0, 0x2F);                               /* _PostEvent        */
    E(0x41, 0xF9, ABS32(0x100000u + QD_EVT));
    E(0x22, 0x79, ABS32(0x100000u + QD_DIALOG)); /* movea.l dialog,a1 */
    E(0xA9, 0x7F);                               /* _IsDialogEvent    */
    E(0x41, 0xF9, ABS32(0x100000u + QD_ISEV));
    E(0x30, 0x80);                               /* move.w d0,(a0)    */
    E(0x41, 0xF9, ABS32(0x100000u + QD_CPT));
    E(0x22, 0x79, ABS32(0x100000u + QD_DIALOG));
    E(0x45, 0xF9, ABS32(0x100000u + QD_CFIND));
    E(0xA9, 0x6C);                               /* _FindControl      */
    E(0x41, 0xF9, ABS32(0x100000u + QD_CPART));
    E(0x30, 0x80);                               /* move.w d0,(a0)    */
    E(0x20, 0x79, ABS32(0x100000u + QD_CFIND));  /* movea.l cfind,a0  */
    E(0xA9, 0x6D);                               /* _Draw1Control     */
    E(0x70, 0x00);                               /* moveq #0,d0       */
    E(0x41, 0xF9, ABS32(0x100000u + QD_HIT2));
    E(0x22, 0x48);                               /* move.l a0,a1      */
    E(0xA9, 0x91);                               /* _ModalDialog      */
    E(0x20, 0x7C, 0x00, 0x00, 0x00, 0x00);
    E(0x41, 0xF9, ABS32(0x100000u + QD_HIT));    /* lea itemHit,a1    */
    E(0x22, 0x48);                               /* move.l a0,a1? no: a1 already set */
    E(0xA9, 0x91);                               /* _ModalDialog      */
    E(0x41, 0xF9, ABS32(0x100000u + QD_MODAL));
    E(0x30, 0x80);                               /* move.w d0,(a0)    */
    E(0x30, 0x3C, 0xFF, 0xFF);                   /* move.w #-1,d0     */
    E(0x41, 0xF9, ABS32(0x100000u + QD_EVT));
    E(0xA9, 0x70);                               /* _GetNextEvent     */
    E(0x41, 0xF9, ABS32(0x100000u + QD_EV_NONE));
    E(0x30, 0x80);
    E(0x30, 0x3C, 0x00, 0x01);                   /* move.w #1,d0      */
    E(0x20, 0x7C, 0x00, 0x00, 0x00, 0x00);
    E(0xA0, 0x2F);                               /* _PostEvent        */
    E(0x30, 0x3C, 0xFF, 0xFF);
    E(0x41, 0xF9, ABS32(0x100000u + QD_EVT));
    E(0xA9, 0x70);                               /* _GetNextEvent     */
    E(0x41, 0xF9, ABS32(0x100000u + QD_EV_POST));
    E(0x30, 0x80);
    E(0xA9, 0x74);                               /* _Button           */
    E(0x41, 0xF9, ABS32(0x100000u + QD_BUTTON2));
    E(0x30, 0x80);
    E(0xA9, 0x75);                               /* _TickCount        */
    E(0x41, 0xF9, ABS32(0x100000u + QD_TICKS));
    E(0x30, 0x80);
    E(0x41, 0xF9, ABS32(0x100000u + QD_PTOUT));
    E(0xA9, 0x72);                               /* _GetMouse         */
    E(0x30, 0x3C, 0x00, 0x01);                   /* move.w #1,d0      */
    E(0xA9, 0xC8);                               /* _SysBeep          */
    E(0x41, 0xF9, ABS32(0x100000u + QD_DIALOG));
    E(0x20, 0x50);                               /* move.l (a0),a0    */
    E(0xA9, 0x83);                               /* _DisposeDialog    */
    /* A second dialog has to land on the same record and table: the first one
     * gave its controls, its table and the record back. */
    E(0x30, 0x3C, 0x00, 0x80);                   /* move.w #128,d0    */
    E(0x20, 0x7C, 0x00, 0x00, 0x00, 0x00);       /* move.l #0,a0      */
    E(0x22, 0x7C, 0x00, 0x00, 0x00, 0x00);       /* move.l #0,a1      */
    E(0xA9, 0x7C);                               /* _GetNewDialog     */
    E(0x41, 0xF9, ABS32(0x100000u + QD_DLG2));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x20, 0x40);                               /* move.l d0,a0      */
    E(0x20, 0x28, 0x00, 0x10);                   /* move.l 16(a0),d0  */
    E(0x41, 0xF9, ABS32(0x100000u + QD_TAB2));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x20, 0x79, ABS32(0x100000u + QD_DLG2));   /* movea.l dlg2,a0   */
    E(0xA9, 0x82);                               /* _CloseDialog      */
    E(0xA9, 0xF4);                               /* _ExitToShell      */
#undef E

    if (n > QD_PT1)
        return 0u;
    memset(out + n, 0, QD_SEG_LEN - n);
    wr16(out + QD_PT1, 10u);            /* MoveTo: v = 10, h = 20 */
    wr16(out + QD_PT1 + 2u, 20u);
    wr16(out + QD_PT2, 50u);            /* LineTo: v = 50, h = 20 */
    wr16(out + QD_PT2 + 2u, 20u);
    wr16(out + QD_RECT1, 10u);
    wr16(out + QD_RECT1 + 2u, 20u);
    wr16(out + QD_RECT1 + 4u, 30u);
    wr16(out + QD_RECT1 + 6u, 40u);
    memset(out + QD_PAT, 0xFF, 8u);
    out[QD_STR] = 2u;
    out[QD_STR + 1u] = 'H';
    out[QD_STR + 2u] = 'i';
    wr16(out + QD_EVT, 1u);             /* mouseDown in the record */
    wr16(out + QD_CPT, 37u);            /* button centre 37,35 */
    wr16(out + QD_CPT + 2u, 35u);
    wr16(out + QD_EVT + 10u, 37u);
    wr16(out + QD_EVT + 12u, 35u);
    wr16(out + QD_RECT2, 1u);
    wr16(out + QD_RECT2 + 2u, 1u);
    wr16(out + QD_RECT2 + 4u, 5u);
    wr16(out + QD_RECT2 + 6u, 5u);
    return QD_SEG_LEN;
}

/* Captured drawing calls: kind 0 rect, 1 line, 2 text. */
struct qd_rec {
    int kind;
    int op;
    int32_t a, b, c, d, e, f;
    char text[32];
};
static struct qd_rec qd_log[32];
static unsigned qd_count;

static struct qd_rec *qd_slot(void)
{
    struct qd_rec *r = &qd_log[qd_count < 32u ? qd_count : 31u];

    if (qd_count < 32u)
        qd_count++;
    memset(r, 0, sizeof *r);
    return r;
}

static void rec_rect(void *ctx, int32_t top, int32_t left, int32_t bottom,
                     int32_t right, int op, const uint8_t *pattern)
{
    struct qd_rec *r;

    (void)ctx;
    r = qd_slot();
    r->kind = 0;
    r->op = op;
    r->a = top;
    r->b = left;
    r->c = bottom;
    r->d = right;
    r->e = pattern ? (int32_t)pattern[0] : -1;
}

static void rec_line(void *ctx, int32_t from_y, int32_t from_x, int32_t to_y,
                     int32_t to_x, uint16_t mode, uint16_t width,
                     uint16_t height)
{
    struct qd_rec *r;

    (void)ctx;
    r = qd_slot();
    r->kind = 1;
    r->a = from_y;
    r->b = from_x;
    r->c = to_y;
    r->d = to_x;
    r->e = (int32_t)mode;
    r->f = ((int32_t)width << 16) | (int32_t)height;
}

static void rec_text(void *ctx, int32_t y, int32_t x, const char *text,
                     uint32_t len, uint16_t font, uint16_t size, uint16_t face)
{
    struct qd_rec *r;

    (void)ctx;
    r = qd_slot();
    r->kind = 2;
    r->a = y;
    r->b = x;
    r->e = (int32_t)font;
    r->f = ((int32_t)size << 16) | (int32_t)face;
    if (len > sizeof r->text - 1u)
        len = (uint32_t)sizeof r->text - 1u;
    memcpy(r->text, text, len);
}

static const struct qd_rec *qd_find(int kind, int op)
{
    unsigned i;

    for (i = 0u; i < qd_count; i++)
        if (qd_log[i].kind == kind && (kind != 0 || qd_log[i].op == op))
            return &qd_log[i];
    return 0;
}

static const struct qd_rec *qd_find_text(const char *text)
{
    unsigned i;

    for (i = 0u; i < qd_count; i++)
        if (qd_log[i].kind == 2 && strcmp(qd_log[i].text, text) == 0)
            return &qd_log[i];
    return 0;
}

static void qd_case(void)
{
    static uint8_t seg[QD_SEG_LEN];
    static uint8_t img[2048];
    static qrx_m68k_t m;
    test_res_t extra[2];
    qrx_m68k_gfx_t gfx;
    const struct qd_rec *r;
    uint32_t len, dialog;
    unsigned i;
    int before = failures;
    qrx_m68k_status_t st;

    if (qd_program(seg, (uint32_t)sizeof seg) == 0u) {
        printf("FAIL qd: the segment does not fit\n");
        failures++;
        return;
    }
    extra[0].type = 0x444C4F47u;            /* 'DLOG' */
    extra[0].id = 128;
    extra[0].data = dlog_res;
    extra[0].len = (uint32_t)sizeof dlog_res;
    extra[1].type = 0x4449544Cu;            /* 'DITL' */
    extra[1].id = 128;
    extra[1].data = ditl_res;
    extra[1].len = (uint32_t)sizeof ditl_res;
    len = build_fork_ex(img, (uint32_t)sizeof img, seg, (uint32_t)sizeof seg,
                        extra, 2u);
    if (len == 0u) {
        printf("FAIL qd: could not build the image\n");
        failures++;
        return;
    }

    memset(&gfx, 0, sizeof gfx);
    gfx.rect = rec_rect;
    gfx.line = rec_line;
    gfx.text = rec_text;
    qd_count = 0u;

    qrx_m68k_unload(&m);
    st = qrx_m68k_load_limited(&m, img, len, 16u * 1024u * 1024u);
    if (st != QRX_M68K_OK) {
        printf("FAIL qd: load = %s\n", qrx_m68k_status_name(st));
        failures++;
        return;
    }
    qrx_m68k_set_gfx(&m, &gfx);
    st = qrx_m68k_start(&m, 2000000u);
    if (st != QRX_M68K_EXITED) {
        printf("FAIL qd: run = %s (%s), stopped at trap 0x%04X\n",
               qrx_m68k_status_name(st), m.detail, 0xA000u | m.stop_trap);
        failures++;
        return;
    }

    if (getenv("QRX_TEST_TRACE")) {
        for (i = 0u; i < m.ntrace; i++)
            printf("  trap 0x%04X pc=0x%06X d0=0x%08X a0=0x%08X -> 0x%08X\n",
                   0xA000u | m.trace[i].trap, m.trace[i].pc, m.trace[i].d0,
                   m.trace[i].a0, m.trace[i].result);
        for (i = 0u; i < qd_count; i++)
            printf("  gfx %d op %d %d,%d,%d,%d e=%d f=%d text=%s\n",
                   qd_log[i].kind, qd_log[i].op, qd_log[i].a, qd_log[i].b,
                   qd_log[i].c, qd_log[i].d, qd_log[i].e, qd_log[i].f,
                   qd_log[i].text);
    }
    r = qd_find(1, 0);
    if (!r || r->a != 10 || r->b != 20 || r->c != 50 || r->d != 20) {
        printf("FAIL qd: LineTo reached the hook as %d,%d -> %d,%d\n",
               r ? r->a : -1, r ? r->b : -1, r ? r->c : -1, r ? r->d : -1);
        failures++;
    }
    r = qd_find(0, QRX_M68K_RECT_FILL);
    if (!r || r->a != 10 || r->b != 20 || r->c != 30 || r->d != 40 ||
        r->e != 0xFF) {
        printf("FAIL qd: FillRect did not arrive with rect 10,20,30,40\n");
        failures++;
    }
    r = qd_find(0, QRX_M68K_RECT_FRAME);
    if (!r || r->a != 10 || r->b != 20 || r->c != 30 || r->d != 40) {
        printf("FAIL qd: FrameRect did not arrive\n");
        failures++;
    }
    r = qd_find_text("Hi");
    if (!r || r->a != 50 || r->b != 20 || r->e != 4 || (r->f >> 16) != 12) {
        printf("FAIL qd: DrawString did not arrive as \"Hi\" at 50,20 in font 4/12\n");
        failures++;
    }
    r = qd_find(0, QRX_M68K_RECT_ERASE);
    if (!r || r->a != 3 || r->b != 2 || r->c != 7 || r->d != 6) {
        printf("FAIL qd: OffsetRect moved the rectangle to %d,%d,%d,%d\n",
               r ? r->a : -1, r ? r->b : -1, r ? r->c : -1, r ? r->d : -1);
        failures++;
    }
    if (rd16(m.mem + 0x100000u + QD_ALERT) != 1u)
        fail(16u * 1024u * 1024u, "Alert itemHit", rd16(m.mem + 0x100000u + QD_ALERT), 1u);
    if (rd16(m.mem + 0x100000u + QD_MODAL) != 0u)
        fail(16u * 1024u * 1024u, "ModalDialog", rd16(m.mem + 0x100000u + QD_MODAL), 0u);
    if (rd16(m.mem + 0x100000u + QD_HIT) != 1u)
        fail(16u * 1024u * 1024u, "ModalDialog itemHit", rd16(m.mem + 0x100000u + QD_HIT), 1u);
    if (rd16(m.mem + 0x100000u + QD_HIT2) != 2u)
        fail(16u * 1024u * 1024u, "ModalDialog on the DITL button",
             rd16(m.mem + 0x100000u + QD_HIT2), 2u);
    if (rd16(m.mem + 0x100000u + QD_ISEV) != 1u)
        fail(16u * 1024u * 1024u, "IsDialogEvent", rd16(m.mem + 0x100000u + QD_ISEV), 1u);
    if (rd16(m.mem + 0x100000u + QD_CPART) != 10u)
        fail(16u * 1024u * 1024u, "FindControl on a DITL button",
             rd16(m.mem + 0x100000u + QD_CPART), 10u);
    if (rd16(m.mem + 0x100000u + QD_EV_NONE) != 0u)
        fail(16u * 1024u * 1024u, "GetNextEvent (empty)",
             rd16(m.mem + 0x100000u + QD_EV_NONE), 0u);
    if (rd16(m.mem + 0x100000u + QD_EV_POST) != 1u)
        fail(16u * 1024u * 1024u, "GetNextEvent (posted)",
             rd16(m.mem + 0x100000u + QD_EV_POST), 1u);
    if (rd16(m.mem + 0x100000u + QD_BUTTON2) != 1u)
        fail(16u * 1024u * 1024u, "Button after mouseDown",
             rd16(m.mem + 0x100000u + QD_BUTTON2), 1u);
    if (rd16(m.mem + 0x100000u + QD_TICKS) == 0u) {
        printf("FAIL qd: TickCount stayed at zero\n");
        failures++;
    }
    dialog = rd32(m.mem + 0x100000u + QD_DIALOG);
    if (dialog == 0u || dialog + QRX_M68K_DLG_RECORD > m.mem_size) {
        printf("FAIL qd: GetNewDialog answered nil\n");
        failures++;
    } else if (rd16(m.mem + dialog + QRX_M68K_DLG_ITEMS) != 2u) {
        printf("FAIL qd: the dialog has %u items, the DITL has 2\n",
               (unsigned)rd16(m.mem + dialog + QRX_M68K_DLG_ITEMS));
        failures++;
    }
    if (dialog != 0u) {
        /* Only the button is a control; the statText stays text. */
        unsigned k, found = 0u;
        int bad = 0;

        /* The run ends after DisposeDialog, so `used` is already cleared; the
         * fields stay readable and mark the slot as a DITL control. */
        for (k = 0u; k < QRX_M68K_CONTROL_MAX; k++) {
            if (m.ctrl[k].item == 0)
                continue;
            found++;
            if (m.ctrl[k].owner != dialog || m.ctrl[k].item != 2 ||
                m.ctrl[k].top != 30 ||
                m.ctrl[k].left != 10 || m.ctrl[k].bottom != 45 ||
                m.ctrl[k].right != 60 || strcmp(m.ctrl[k].title, "Go") != 0)
                bad = 1;
        }
        if (found != 1u || bad) {
            printf("FAIL qd: the DITL button did not become one control "
                   "(found %u, item %d, %d,%d,%d,%d \"%s\")\n",
                   found, (int)m.ctrl[0].item, (int)m.ctrl[0].top,
                   (int)m.ctrl[0].left, (int)m.ctrl[0].bottom,
                   (int)m.ctrl[0].right, m.ctrl[0].title);
            failures++;
        }
    }
    if (dialog != 0u) {
        uint32_t dlg2 = rd32(m.mem + 0x100000u + QD_DLG2);
        uint32_t tab2 = rd32(m.mem + 0x100000u + QD_TAB2);
        uint32_t ch = rd32(m.mem + 0x100000u + QD_CFIND);
        unsigned k;
        int live = 0;

        if (dlg2 != dialog)
            fail(16u * 1024u * 1024u, "GetNewDialog after DisposeDialog",
                 dlg2, dialog);
        if (tab2 != dialog + QRX_M68K_DLG_RECORD)
            fail(16u * 1024u * 1024u, "the item table after DisposeDialog",
                 tab2, dialog + QRX_M68K_DLG_RECORD);
        if (ch == 0u || (m.ctrl[0].item == 2 && m.ctrl[0].handle != ch))
            fail(16u * 1024u * 1024u, "the control record after DisposeDialog",
                 m.ctrl[0].handle, ch);
        for (k = 0u; k < QRX_M68K_CONTROL_MAX; k++)
            if (m.ctrl[k].used)
                live++;
        if (live != 0) {
            printf("FAIL qd: %d control(s) survived CloseDialog\n", live);
            failures++;
        }
        if (m.nheap_free != 0u || m.heap != dialog)
            fail(16u * 1024u * 1024u, "heap after CloseDialog", m.heap,
                 dialog);
        if (m.ndialog != 0u)
            fail(16u * 1024u * 1024u, "dialogs left open", m.ndialog, 0u);
    }
    if (!qd_find_text("Ready") || !qd_find_text("Go")) {
        printf("FAIL qd: DrawDialog did not draw the DITL texts\n");
        failures++;
    }
    {
        int covered = 0;

        for (i = 0u; i < qd_count; i++)
            if (qd_log[i].kind == 0 && qd_log[i].op == QRX_M68K_RECT_FRAME &&
                qd_log[i].a <= 10 && qd_log[i].b <= 10 &&
                qd_log[i].c >= 45 && qd_log[i].d >= 100)
                covered = 1;
        if (!covered) {
            printf("FAIL qd: the dialog frame does not cover the items\n");
            failures++;
        }
    }

    printf("quickdraw/dialog/event: %u draw call(s), alert 1, modal item 1, "
           "posted event 1 %s\n", qd_count,
           failures == before ? "ok" : "FAILED");
}

/* ------------------------------------------------------- windows and menus */


#define QW_BOUNDS 0x4E0u
#define QW_TITLE 0x4E8u
#define QW_INVAL 0x4F0u
#define QW_MTITLE 0x4F8u
#define QW_ITEM1 0x500u
#define QW_ITEM2 0x508u
#define QW_TBUF 0x510u
#define QW_WIN 0x530u
#define QW_FRONT 0x534u
#define QW_WREF 0x538u
#define QW_COUNT 0x53Cu
#define QW_MENU 0x540u
#define QW_SCRAP 0x548u
#define QW_OFF 0x550u
#define QW_OFF2 0x554u
#define QW_SLEN 0x558u
#define QW_HANDLE 0x55Cu
#define QW_EV1 0x560u
#define QW_EV2 0x570u
#define QW_EV3 0x580u
#define QW_BOUNDS2 0x590u
#define QW_TITLE2 0x598u
#define QW_WIN2 0x5A8u
#define QW_TBUF2 0x5B0u
#define QW_MSEL 0x5C0u
#define QW_MKEY 0x5C4u
#define QW_MKEY2 0x5C8u
#define QW_CMDREAD 0x5CCu
#define QW_TBUF3 0x5BCu
#define QW_CRECT 0x5D0u
#define QW_CPT 0x5D8u
#define QW_CTRL 0x5DCu
#define QW_CFIND 0x5E0u
#define QW_CTITLE 0x5E4u
#define QW_CTBUF 0x5E8u
#define QW_CVAL 0x5ECu
#define QW_CHIT 0x5EEu
#define QW_CHIT2 0x5F0u
#define QW_CTRL2 0x5F4u
#define QW_CTRL3 0x5F8u
#define QW_CTRL4 0x5FCu
#define QW_CVAL2 0x600u
#define QW_WIN3 0x604u
#define QW_SEG_LEN 0x610u

/* CODE 1 for the Window and Menu Manager: one window with a title, refCon,
 * an update cycle and an invalid rectangle, plus a menu with two items. */
static uint32_t win_program(uint8_t *out, uint32_t cap)
{
    uint32_t n = 0u;

#define E(...) do { \
        static const uint8_t bytes_[] = { __VA_ARGS__ }; \
        if (n + sizeof bytes_ > cap) return 0u; \
        memcpy(out + n, bytes_, sizeof bytes_); \
        n += (uint32_t)sizeof bytes_; \
    } while (0)

    E(0x20, 0x7C, 0x00, 0x00, 0x00, 0x00);       /* move.l #0,a0 (storage) */
    E(0x43, 0xF9, ABS32(0x100000u + QW_BOUNDS)); /* lea bounds,a1     */
    E(0x45, 0xF9, ABS32(0x100000u + QW_TITLE));  /* lea title,a2      */
    E(0x26, 0x7C, 0x00, 0x00, 0x00, 0x00);       /* move.l #0,a3      */
    E(0x30, 0x3C, 0x00, 0x01);                   /* move.w #1,d0      */
    E(0x32, 0x3C, 0x00, 0x00);                   /* move.w #0,d1      */
    E(0x34, 0x3C, 0x00, 0x00);                   /* move.w #0,d2      */
    E(0x26, 0x3C, 0x00, 0x00, 0x12, 0x34);       /* move.l #$1234,d3  */
    E(0xA9, 0x13);                               /* _NewWindow        */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN));
    E(0x20, 0x50);                               /* move.l (a0),a0    */
    E(0xA9, 0x15);                               /* _ShowWindow       */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN));
    E(0x20, 0x50);
    E(0xA9, 0x1F);                               /* _SelectWindow     */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN));
    E(0x20, 0x50);
    E(0xA9, 0x22);                               /* _BeginUpdate      */
    E(0x41, 0xF9, ABS32(0x100000u + QW_INVAL));
    E(0xA9, 0x28);                               /* _InvalRect        */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN));
    E(0x20, 0x50);
    E(0xA9, 0x23);                               /* _EndUpdate        */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN));
    E(0x20, 0x50);
    E(0xA9, 0x17);                               /* _GetWRefCon       */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WREF));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0xA9, 0x24);                               /* _FrontWindow      */
    E(0x41, 0xF9, ABS32(0x100000u + QW_FRONT));
    E(0x20, 0x80);
    E(0xA9, 0x30);                               /* _InitMenus        */
    E(0x30, 0x3C, 0x00, 0x80);                   /* move.w #128,d0    */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MTITLE));
    E(0xA9, 0x31);                               /* _NewMenu          */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MENU));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MENU));
    E(0x20, 0x50);
    E(0x43, 0xF9, ABS32(0x100000u + QW_ITEM1));
    E(0xA9, 0x33);                               /* _AppendMenu       */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MENU));
    E(0x20, 0x50);
    E(0x43, 0xF9, ABS32(0x100000u + QW_ITEM2));
    E(0xA9, 0x33);                               /* _AppendMenu       */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MENU));
    E(0x20, 0x50);
    E(0xA9, 0x50);                               /* _CountMItems      */
    E(0x41, 0xF9, ABS32(0x100000u + QW_COUNT));
    E(0x30, 0x80);                               /* move.w d0,(a0)    */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MENU));
    E(0x20, 0x50);
    E(0x30, 0x3C, 0x00, 0x01);                   /* move.w #1,d0      */
    E(0x43, 0xF9, ABS32(0x100000u + QW_TBUF));
    E(0xA9, 0x46);                               /* _GetMenuItemText  */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MENU));
    E(0x20, 0x50);
    E(0x30, 0x3C, 0x00, 0x00);
    E(0xA9, 0x35);                               /* _InsertMenu       */
    E(0xA9, 0x37);                               /* _DrawMenuBar      */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MENU));
    E(0x20, 0x50);                               /* move.l (a0),a0    */
    E(0x30, 0x3C, 0x00, 0x01);                   /* move.w #1,d0      */
    E(0x32, 0x3C, 0x00, 0x01);                   /* move.w #1,d1      */
    E(0xA9, 0x45);                               /* _CheckItem        */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MENU));
    E(0x20, 0x50);
    E(0x30, 0x3C, 0x00, 0x01);
    E(0xA9, 0x3A);                               /* _DisableItem      */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MENU));
    E(0x20, 0x50);
    E(0x30, 0x3C, 0x00, 0x01);
    E(0x43, 0xF9, ABS32(0x100000u + QW_TBUF2));
    E(0xA9, 0x47);                               /* _SetMenuItemText  */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN));
    E(0x20, 0x50);
    E(0xA8, 0x73);                               /* _SetPort          */
    E(0x20, 0x7C, 0x00, 0x00, 0x00, 0x00);       /* move.l #0,a0      */
    E(0x43, 0xF9, ABS32(0x100000u + QW_BOUNDS2));
    E(0x45, 0xF9, ABS32(0x100000u + QW_TITLE2));
    E(0x26, 0x7C, 0x00, 0x00, 0x00, 0x00);       /* move.l #0,a3      */
    E(0x30, 0x3C, 0x00, 0x01);                   /* move.w #1,d0      */
    E(0x32, 0x3C, 0x00, 0x00);
    E(0x34, 0x3C, 0x00, 0x00);
    E(0x26, 0x3C, 0x00, 0x00, 0x00, 0x00);
    E(0xA9, 0x13);                               /* _NewWindow        */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN2));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN2));
    E(0x20, 0x50);
    E(0xA9, 0x15);                               /* _ShowWindow       */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN2));
    E(0x20, 0x50);
    E(0xA9, 0x14);                               /* _DisposeWindow    */
    E(0xA9, 0xFC);                               /* _ZeroScrap        */
    E(0x20, 0x3C, 0x00, 0x00, 0x00, 0x05);       /* move.l #5,d0      */
    E(0x22, 0x3C, 0x54, 0x45, 0x58, 0x54);       /* move.l #'TEXT',d1 */
    E(0x41, 0xF9, ABS32(0x100000u + QW_SCRAP));  /* lea scrap,a0      */
    E(0xA9, 0xFE);                               /* _PutScrap         */
    E(0x20, 0x7C, 0x00, 0x00, 0x00, 0x00);       /* move.l #0,a0      */
    E(0x20, 0x3C, 0x54, 0x45, 0x58, 0x54);       /* move.l #'TEXT',d0 */
    E(0x43, 0xF9, ABS32(0x100000u + QW_OFF));
    E(0xA9, 0xFD);                               /* _GetScrap         */
    E(0x41, 0xF9, ABS32(0x100000u + QW_SLEN));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x20, 0x3C, 0x00, 0x00, 0x00, 0x10);       /* move.l #16,d0     */
    E(0xA1, 0x22);                               /* _NewHandle: in a0 */
    E(0x43, 0xF9, ABS32(0x100000u + QW_HANDLE));
    E(0x22, 0x88);                               /* move.l a0,(a1)    */
    E(0x41, 0xF9, ABS32(0x100000u + QW_HANDLE));
    E(0x20, 0x50);
    E(0x20, 0x3C, 0x54, 0x45, 0x58, 0x54);       /* move.l #'TEXT',d0 */
    E(0x43, 0xF9, ABS32(0x100000u + QW_OFF2));
    E(0xA9, 0xFD);                               /* _GetScrap         */
    E(0xA9, 0x75);                               /* _TickCount: pumps  */
    E(0x41, 0xF9, ABS32(0x100000u + QW_EV1));
    E(0x70, 0x00);                               /* moveq #0,d0       */
    E(0xA9, 0x70);                               /* _GetNextEvent     */
    E(0x41, 0xF9, ABS32(0x100000u + QW_EV2));
    E(0x70, 0x00);
    E(0xA9, 0x70);                               /* _GetNextEvent     */
    E(0x41, 0xF9, ABS32(0x100000u + QW_EV3));
    E(0x70, 0x00);
    E(0xA9, 0x70);                               /* _GetNextEvent     */
    E(0x3F, 0x3C, 0x00, 0x14);                   /* move.w #20,-(a7) h */
    E(0x3F, 0x3C, 0x00, 0x08);                   /* move.w #8,-(a7)  v */
    E(0xA9, 0x3D);                               /* _MenuSelect       */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MSEL));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x32, 0x3C, 0x00, 0x4F);                   /* move.w #'O',d1    */
    E(0x70, 0x01);                               /* moveq #1,d0       */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0xA8, 0x4F);                               /* _SetItemCmd(,1,'O') */
    E(0x32, 0x3C, 0x00, 0x51);                   /* move.w #'Q',d1    */
    E(0x70, 0x02);                               /* moveq #2,d0       */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0xA8, 0x4F);                               /* _SetItemCmd(,2,'Q') */
    E(0x22, 0x7C, ABS32(0x100000u + QW_CMDREAD));   /* movea.l #buf,a1 */
    E(0x70, 0x02);                               /* moveq #2,d0       */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));   /* movea.l menu,a0   */
    E(0xA8, 0x4E);                               /* _GetItemCmd(,2)   */
    E(0x12, 0x39, ABS32(0x100000u + QW_CMDREAD));   /* move.b buf,d1  */
    E(0x70, 0x01);                               /* moveq #1,d0       */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0xA8, 0x4F);                               /* _SetItemCmd(,1,read) */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0x70, 0x02);                               /* moveq #2,d0       */
    E(0x32, 0x3C, 0x00, 0x01);                   /* move.w #1,d1      */
    E(0xA9, 0x42);                               /* _SetItmStyle(,2,1)*/
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0x70, 0x01);                               /* moveq #1,d0       */
    E(0x32, 0x3C, 0x00, 0x03);                   /* move.w #3,d1      */
    E(0xA9, 0x40);                               /* _SetItmIcon(,1,3) */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0x70, 0x02);                               /* moveq #2,d0       */
    E(0x32, 0x3C, 0x00, 0x58);                   /* move.w #'X',d1    */
    E(0xA9, 0x44);                               /* _SetItemMark(,2)  */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0x70, 0x01);                               /* moveq #1,d0       */
    E(0x43, 0xF9, ABS32(0x100000u + QW_TBUF2 + 8u));
    E(0xA9, 0x47);                               /* _SetMenuItemText  */
    E(0x30, 0x3C, 0x00, 0x71);                   /* move.w #'q',d0    */
    E(0xA9, 0x3E);                               /* _MenuKey('q')     */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MKEY));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x30, 0x3C, 0x00, 0x4F);                   /* move.w #'O',d0    */
    E(0xA9, 0x3E);                               /* _MenuKey('O')     */
    E(0x41, 0xF9, ABS32(0x100000u + QW_MKEY2));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x70, 0x00);                               /* moveq #0,d0       */
    E(0xA9, 0x38);                               /* _HiliteMenu(0)    */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0x70, 0x02);                               /* moveq #2,d0       */
    E(0x32, 0x3C, 0x00, 0x05);                   /* move.w #5,d1      */
    E(0xA9, 0x40);                               /* _SetItmIcon(,2,5) */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0x70, 0x02);                               /* moveq #2,d0       */
    E(0xA9, 0x52);                               /* _DeleteMenuItem   */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0x43, 0xF9, ABS32(0x100000u + QW_TBUF3));
    E(0xA9, 0x33);                               /* _AppendMenu "Ab"  */
    E(0x20, 0x79, ABS32(0x100000u + QW_MENU));
    E(0xA9, 0x32);                               /* _DisposeMenu      */
    /* Control Manager: a push button on the window, drawn and tracked. */
    E(0x20, 0x79, ABS32(0x100000u + QW_WIN));    /* movea.l win,a0    */
    E(0x43, 0xF9, ABS32(0x100000u + QW_CRECT));  /* lea crect,a1      */
    E(0x45, 0xF9, ABS32(0x100000u + QW_CTITLE)); /* lea ctitle,a2     */
    E(0x70, 0x01);                               /* moveq #1,d0 vis   */
    E(0x72, 0x01);                               /* moveq #1,d1 value */
    E(0x74, 0x00);                               /* moveq #0,d2 min   */
    E(0x76, 0x01);                               /* moveq #1,d3 max   */
    E(0xA9, 0x54);                               /* _NewControl       */
    E(0x41, 0xF9, ABS32(0x100000u + QW_CTRL));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x20, 0x79, ABS32(0x100000u + QW_WIN));
    E(0xA9, 0x69);                               /* _DrawControls     */
    E(0x20, 0x79, ABS32(0x100000u + QW_CTRL));   /* movea.l ctrl,a0   */
    E(0xA9, 0x60);                               /* _GetControlValue  */
    E(0x41, 0xF9, ABS32(0x100000u + QW_CVAL));
    E(0x30, 0x80);                               /* move.w d0,(a0)    */
    E(0x20, 0x79, ABS32(0x100000u + QW_CTRL));   /* movea.l ctrl,a0   */
    E(0x70, 0x02);                               /* moveq #2,d0       */
    E(0xA9, 0x63);                               /* _SetControlValue  */
    E(0x20, 0x79, ABS32(0x100000u + QW_CTRL));   /* movea.l ctrl,a0   */
    E(0xA9, 0x60);                               /* _GetControlValue  */
    E(0x41, 0xF9, ABS32(0x100000u + QW_CVAL2));
    E(0x30, 0x80);                               /* move.w d0,(a0)    */
    E(0x20, 0x79, ABS32(0x100000u + QW_CTRL));   /* movea.l ctrl,a0   */
    E(0x43, 0xF9, ABS32(0x100000u + QW_CTBUF));  /* lea cbuf,a1       */
    E(0xA9, 0x5E);                               /* _GetControlTitle  */
    E(0x41, 0xF9, ABS32(0x100000u + QW_CPT));    /* lea cpt,a0        */
    E(0x22, 0x79, ABS32(0x100000u + QW_WIN));    /* movea.l win,a1    */
    E(0x45, 0xF9, ABS32(0x100000u + QW_CFIND));  /* lea cfind,a2      */
    E(0xA9, 0x6C);                               /* _FindControl      */
    E(0x41, 0xF9, ABS32(0x100000u + QW_CHIT));
    E(0x30, 0x80);                               /* move.w d0,(a0)    */
    E(0x20, 0x79, ABS32(0x100000u + QW_CTRL));   /* movea.l ctrl,a0   */
    E(0x43, 0xF9, ABS32(0x100000u + QW_CPT));    /* lea cpt,a1        */
    E(0xA9, 0x68);                               /* _TrackControl     */
    E(0x41, 0xF9, ABS32(0x100000u + QW_CHIT2));
    E(0x30, 0x80);                               /* move.w d0,(a0)    */
    /* _DisposeControl gives the record back, so the next _NewControl
     * lands on the same address; _KillControls frees a whole owner. */
    E(0x20, 0x79, ABS32(0x100000u + QW_CTRL));   /* movea.l ctrl,a0   */
    E(0xA9, 0x55);                               /* _DisposeControl   */
    E(0x20, 0x79, ABS32(0x100000u + QW_WIN));
    E(0x43, 0xF9, ABS32(0x100000u + QW_CRECT));
    E(0x45, 0xF9, ABS32(0x100000u + QW_CTITLE));
    E(0x70, 0x01);                               /* moveq #1,d0       */
    E(0x72, 0x01);                               /* moveq #1,d1       */
    E(0x74, 0x00);                               /* moveq #0,d2       */
    E(0x76, 0x01);                               /* moveq #1,d3       */
    E(0xA9, 0x54);                               /* _NewControl       */
    E(0x41, 0xF9, ABS32(0x100000u + QW_CTRL2));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x20, 0x79, ABS32(0x100000u + QW_WIN));
    E(0x43, 0xF9, ABS32(0x100000u + QW_CRECT));
    E(0x45, 0xF9, ABS32(0x100000u + QW_CTITLE));
    E(0x70, 0x01);                               /* moveq #1,d0       */
    E(0x72, 0x01);                               /* moveq #1,d1       */
    E(0x74, 0x00);                               /* moveq #0,d2       */
    E(0x76, 0x01);                               /* moveq #1,d3       */
    E(0xA9, 0x54);                               /* _NewControl       */
    E(0x41, 0xF9, ABS32(0x100000u + QW_CTRL3));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x20, 0x79, ABS32(0x100000u + QW_WIN));
    E(0xA9, 0x56);                               /* _KillControls     */
    E(0x20, 0x79, ABS32(0x100000u + QW_WIN));
    E(0x43, 0xF9, ABS32(0x100000u + QW_CRECT));
    E(0x45, 0xF9, ABS32(0x100000u + QW_CTITLE));
    E(0x70, 0x01);                               /* moveq #1,d0       */
    E(0x72, 0x01);                               /* moveq #1,d1       */
    E(0x74, 0x00);                               /* moveq #0,d2       */
    E(0x76, 0x01);                               /* moveq #1,d3       */
    E(0xA9, 0x54);                               /* _NewControl       */
    E(0x41, 0xF9, ABS32(0x100000u + QW_CTRL4));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x20, 0x79, ABS32(0x100000u + QW_CTRL4));
    E(0xA9, 0x55);                               /* _DisposeControl   */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN));
    E(0x20, 0x50);
    E(0xA9, 0x14);                               /* _DisposeWindow    */
    /* The record went back: a new window has to land on its address, and
     * _CloseWindow gives it away again. */
    E(0x20, 0x7C, 0x00, 0x00, 0x00, 0x00);       /* move.l #0,a0      */
    E(0x43, 0xF9, ABS32(0x100000u + QW_BOUNDS2));
    E(0x45, 0xF9, ABS32(0x100000u + QW_TITLE2));
    E(0x26, 0x7C, 0x00, 0x00, 0x00, 0x00);       /* move.l #0,a3      */
    E(0x30, 0x3C, 0x00, 0x01);                   /* move.w #1,d0      */
    E(0x32, 0x3C, 0x00, 0x00);
    E(0x34, 0x3C, 0x00, 0x00);
    E(0x26, 0x3C, 0x00, 0x00, 0x00, 0x00);
    E(0xA9, 0x13);                               /* _NewWindow        */
    E(0x41, 0xF9, ABS32(0x100000u + QW_WIN3));
    E(0x20, 0x80);                               /* move.l d0,(a0)    */
    E(0x20, 0x79, ABS32(0x100000u + QW_WIN3));   /* movea.l win3,a0   */
    E(0xA9, 0x2D);                               /* _CloseWindow      */
    E(0xA9, 0xF4);                               /* _ExitToShell      */
#undef E

    if (n > QW_BOUNDS)
        return 0u;
    memset(out + n, 0, QW_SEG_LEN - n);
    wr16(out + QW_BOUNDS, 10u);          /* bounds 10,10,60,200 */
    wr16(out + QW_BOUNDS + 2u, 10u);
    wr16(out + QW_BOUNDS + 4u, 60u);
    wr16(out + QW_BOUNDS + 6u, 200u);
    out[QW_TITLE] = 4u;
    memcpy(out + QW_TITLE + 1u, "Test", 4);
    wr16(out + QW_INVAL, 12u);
    wr16(out + QW_INVAL + 2u, 14u);
    wr16(out + QW_INVAL + 4u, 30u);
    wr16(out + QW_INVAL + 6u, 40u);
    out[QW_MTITLE] = 5u;
    memcpy(out + QW_MTITLE + 1u, "Datei", 5);
    out[QW_ITEM1] = 4u;
    memcpy(out + QW_ITEM1 + 1u, "Open", 4);
    out[QW_ITEM2] = 4u;
    memcpy(out + QW_ITEM2 + 1u, "Quit", 4);
    memcpy(out + QW_SCRAP, "Hallo", 5);
    wr16(out + QW_BOUNDS2, 70u);         /* bounds 70,10,160,260 */
    wr16(out + QW_BOUNDS2 + 2u, 10u);
    wr16(out + QW_BOUNDS2 + 4u, 160u);
    wr16(out + QW_BOUNDS2 + 6u, 260u);
    out[QW_TITLE2] = 6u;
    memcpy(out + QW_TITLE2 + 1u, "Zweite", 6);
    out[QW_TBUF2] = 7u;
    memcpy(out + QW_TBUF2 + 1u, "Oeffnen", 7);
    out[QW_TBUF2 + 8u] = 3u;
    memcpy(out + QW_TBUF2 + 9u, "Neu", 3);
    out[QW_TBUF3] = 2u;
    memcpy(out + QW_TBUF3 + 1u, "Ab", 2);
    wr16(out + QW_CRECT, 100u);          /* control 100,20,120,60 */
    wr16(out + QW_CRECT + 2u, 20u);
    wr16(out + QW_CRECT + 4u, 120u);
    wr16(out + QW_CRECT + 6u, 60u);
    wr16(out + QW_CPT, 110u);            /* point 110,30          */
    wr16(out + QW_CPT + 2u, 30u);
    out[QW_CTITLE] = 2u;
    memcpy(out + QW_CTITLE + 1u, "OK", 2);
    return QW_SEG_LEN;
}

struct win_rec {
    int op;
    int32_t a, b, c, d;
    uint32_t id;
    char title[32];
};
static struct win_rec win_log[24];
static unsigned win_count;

struct menu_rec {
    int op;
    int32_t item, value;
    uint32_t id;
    char title[32];
    char text[32];
};
static struct menu_rec menu_log[32];
static unsigned menu_count;

static void rec_window(void *ctx, int op, int32_t top, int32_t left,
                       int32_t bottom, int32_t right, const char *title,
                       uint32_t len, uint32_t id)
{
    struct win_rec *r;

    (void)ctx;
    if (win_count >= 24u)
        return;
    r = &win_log[win_count++];
    memset(r, 0, sizeof *r);
    r->op = op;
    r->id = id;
    r->a = top;
    r->b = left;
    r->c = bottom;
    r->d = right;
    if (len > sizeof r->title - 1u)
        len = (uint32_t)sizeof r->title - 1u;
    memcpy(r->title, title, len);
}

static void rec_menu(void *ctx, int op, const char *title, uint32_t title_len,
                     int32_t item, const char *text, uint32_t text_len,
                     int32_t value, uint32_t id)
{
    struct menu_rec *r;

    (void)ctx;
    if (menu_count >= 32u)
        return;
    r = &menu_log[menu_count++];
    memset(r, 0, sizeof *r);
    r->op = op;
    r->id = id;
    r->item = item;
    r->value = value;
    if (title_len > sizeof r->title - 1u)
        title_len = (uint32_t)sizeof r->title - 1u;
    memcpy(r->title, title, title_len);
    if (text != 0) {
        if (text_len > sizeof r->text - 1u)
            text_len = (uint32_t)sizeof r->text - 1u;
        memcpy(r->text, text, text_len);
    }
}

/* The guest's input pump: two events are waiting when the program ticks. */
static int rec_polled, rec_targeted;
static uint32_t rec_target_id;
static qrx_m68k_t *rec_machine;

static void rec_poll(void *ctx)
{
    (void)ctx;
    if (rec_polled++)
        return;
    (void)qrx_m68k_post_event(rec_machine, QRX_M68K_EVENT_MENU,
                              (128u << 16) | 1u, 8, 20, 0u);      /* menuEvt */
    (void)qrx_m68k_post_event(rec_machine, 1u, 0u, 20, 30, 0u);   /* mouseDown */
    (void)qrx_m68k_post_event(rec_machine, 3u, 0x41u, 0, 0, 0u);  /* keyDown A */
}

static void rec_target(void *ctx, uint32_t id)
{
    (void)ctx;
    rec_targeted++;
    rec_target_id = id;
}

/* The shared clipboard, kept in memory: the guest backs it with
 * clipboard.device, this test with two variables. */
static uint8_t clip_mem[256];
static uint32_t clip_mem_len;

static void rec_clip_put(void *ctx, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    if (len > sizeof clip_mem)
        len = (uint32_t)sizeof clip_mem;
    if (len != 0u)
        memcpy(clip_mem, data, len);
    clip_mem_len = len;
}

static int32_t rec_clip_get(void *ctx, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (len > clip_mem_len)
        len = clip_mem_len;
    if (len != 0u)
        memcpy(buf, clip_mem, len);
    return (int32_t)len;
}

static int32_t rec_clip_size(void *ctx)
{
    (void)ctx;
    return (int32_t)clip_mem_len;
}

static const qrx_m68k_clip_t rec_clip = {
    0, rec_clip_put, rec_clip_get, rec_clip_size
};

static const struct win_rec *win_find(int op)
{
    unsigned i;

    for (i = 0u; i < win_count; i++)
        if (win_log[i].op == op)
            return &win_log[i];
    return 0;
}

static const struct menu_rec *menu_find(int op, const char *text)
{
    unsigned i;

    for (i = 0u; i < menu_count; i++)
        if (menu_log[i].op == op &&
            (text == 0 || strcmp(menu_log[i].text, text) == 0))
            return &menu_log[i];
    return 0;
}

static void win_case(void)
{
    static uint8_t seg[QW_SEG_LEN];
    static uint8_t img[4096];
    static qrx_m68k_t m;
    qrx_m68k_gfx_t gfx;
    const struct win_rec *w;
    const struct menu_rec *mr;
    uint32_t len, wr;
    int before = failures;
    qrx_m68k_status_t st;

    if (win_program(seg, (uint32_t)sizeof seg) == 0u) {
        printf("FAIL win: the segment does not fit\n");
        failures++;
        return;
    }
    len = build_fork(img, (uint32_t)sizeof img, seg, (uint32_t)sizeof seg);
    if (len == 0u) {
        printf("FAIL win: could not build the image\n");
        failures++;
        return;
    }
    memset(&gfx, 0, sizeof gfx);
    gfx.rect = rec_rect;
    gfx.line = rec_line;
    gfx.text = rec_text;
    gfx.window = rec_window;
    gfx.menu = rec_menu;
    gfx.poll = rec_poll;
    gfx.target = rec_target;
    win_count = 0u;
    menu_count = 0u;
    rec_polled = rec_targeted = 0;
    rec_target_id = 0u;
    clip_mem_len = 0u;

    qrx_m68k_unload(&m);
    st = qrx_m68k_load_limited(&m, img, len, 16u * 1024u * 1024u);
    if (st != QRX_M68K_OK) {
        printf("FAIL win: load = %s\n", qrx_m68k_status_name(st));
        failures++;
        return;
    }
    qrx_m68k_set_gfx(&m, &gfx);
    qrx_m68k_set_clip(&m, &rec_clip);
    rec_machine = &m;
    st = qrx_m68k_start(&m, 2000000u);
    if (st != QRX_M68K_EXITED) {
        printf("FAIL win: run = %s (%s), stopped at trap 0x%04X\n",
               qrx_m68k_status_name(st), m.detail, 0xA000u | m.stop_trap);
        failures++;
        return;
    }

    w = win_find(QRX_M68K_WIN_CREATE);
    if (!w || w->a != 10 || w->b != 10 || w->c != 60 || w->d != 200 ||
        strcmp(w->title, "Test") != 0) {
        printf("FAIL win: NewWindow did not arrive as 10,10,60,200 \"Test\"\n");
        failures++;
    }
    if (!win_find(QRX_M68K_WIN_SHOW) || !win_find(QRX_M68K_WIN_SELECT)) {
        printf("FAIL win: ShowWindow or SelectWindow did not reach the hook\n");
        failures++;
    }
    if (!win_find(QRX_M68K_WIN_UPDATE_BEGIN) ||
        !win_find(QRX_M68K_WIN_UPDATE_END)) {
        printf("FAIL win: the update cycle did not reach the hook\n");
        failures++;
    }
    w = win_find(QRX_M68K_WIN_INVAL);
    if (!w || w->a != 12 || w->b != 14 || w->c != 30 || w->d != 40) {
        printf("FAIL win: InvalRect did not arrive as 12,14,30,40\n");
        failures++;
    }
    if (!win_find(QRX_M68K_WIN_DISPOSE)) {
        printf("FAIL win: DisposeWindow did not reach the hook\n");
        failures++;
    }
    wr = rd32(m.mem + 0x100000u + QW_WIN);
    if (wr == 0u || wr + QRX_M68K_WINDOW_RECORD > m.mem_size) {
        printf("FAIL win: NewWindow answered nil\n");
        failures++;
    } else {
        /* The record goes back on DisposeWindow, so only the value the guest
         * read while the window lived can be checked here. */
        if (rd32(m.mem + 0x100000u + QW_WREF) != 0x1234u)
            fail(16u * 1024u * 1024u, "GetWRefCon",
                 rd32(m.mem + 0x100000u + QW_WREF), 0x1234u);
        if (rd32(m.mem + 0x100000u + QW_FRONT) != wr)
            fail(16u * 1024u * 1024u, "FrontWindow",
                 rd32(m.mem + 0x100000u + QW_FRONT), wr);
    }

    mr = menu_find(QRX_M68K_MENU_NEW, 0);
    if (!mr || strcmp(mr->title, "Datei") != 0) {
        printf("FAIL win: NewMenu did not arrive as \"Datei\"\n");
        failures++;
    }
    if (mr && (mr->id == 0u ||
               mr->id != rd32(m.mem + 0x100000u + QW_MENU))) {
        printf("FAIL win: the menu hook did not carry the handle\n");
        failures++;
    }
    if (!menu_find(QRX_M68K_MENU_ITEM, "Open") ||
        !menu_find(QRX_M68K_MENU_ITEM, "Quit")) {
        printf("FAIL win: AppendMenu did not deliver both items\n");
        failures++;
    }
    if (rd16(m.mem + 0x100000u + QW_COUNT) != 2u)
        fail(16u * 1024u * 1024u, "CountMItems",
             rd16(m.mem + 0x100000u + QW_COUNT), 2u);
    if (m.mem[0x100000u + QW_TBUF] != 4u ||
        memcmp(m.mem + 0x100000u + QW_TBUF + 1u, "Open", 4) != 0) {
        printf("FAIL win: GetMenuItemText did not return \"Open\"\n");
        failures++;
    }
    if (!menu_find(QRX_M68K_MENU_BAR, 0)) {
        printf("FAIL win: DrawMenuBar drew no menu bar\n");
        failures++;
    }
    if (rd32(m.mem + 0x100000u + QW_MENU) == 0u) {
        printf("FAIL win: NewMenu answered nil\n");
        failures++;
    }
    {
        unsigned i;
        int saw_checked = 0, saw_disabled = 0, saw_text = 0;
        int saw_lit = 0, saw_cleared = 0, saw_cmd_o = 0, saw_cmd_q = 0;
        int saw_style = 0, saw_icon = 0, saw_mark = 0;

        for (i = 0u; i < menu_count; i++) {
            if (menu_log[i].op == QRX_M68K_MENU_ITEM_STATE) {
                if (menu_log[i].value == 3)
                    saw_checked = 1;
                if (menu_log[i].value == 2)
                    saw_disabled = 1;
            }
            if (menu_log[i].op == QRX_M68K_MENU_ITEM_TEXT &&
                strcmp(menu_log[i].text, "Oeffnen") == 0)
                saw_text = 1;
            if (menu_log[i].op == QRX_M68K_MENU_ITEM_STYLE &&
                menu_log[i].value == 1)
                saw_style = 1;
            if (menu_log[i].op == QRX_M68K_MENU_ITEM_ICON &&
                menu_log[i].value == 3)
                saw_icon = 1;
            if (menu_log[i].op == QRX_M68K_MENU_ITEM_MARK &&
                menu_log[i].value == 'X')
                saw_mark = 1;
            if (menu_log[i].op == QRX_M68K_MENU_ITEM_CMD) {
                if (menu_log[i].value == 'O')
                    saw_cmd_o = 1;
                if (menu_log[i].value == 'Q')
                    saw_cmd_q = 1;
            }
            if (menu_log[i].op == QRX_M68K_MENU_HILITE) {
                if (menu_log[i].value == 128)
                    saw_lit = 1;
                if (menu_log[i].value == 0)
                    saw_cleared = 1;
            }
        }
        if (!saw_style || !saw_icon || !saw_mark) {
            printf("FAIL win: SetItmStyle/SetItmIcon/SetItemMark did not reach"
                   " the hook\n");
            failures++;
        }
        if (!saw_cmd_o || !saw_cmd_q) {
            printf("FAIL win: SetItemCmd did not reach the hook\n");
            failures++;
        }
        if (!saw_lit || !saw_cleared) {
            printf("FAIL win: MenuSelect/HiliteMenu did not hilite then clear\n");
            failures++;
        }
        if (m.menu_hilite != 0) {
            printf("FAIL win: HiliteMenu(0) left menu %ld lit\n",
                   (long)m.menu_hilite);
            failures++;
        }
        if (!saw_checked || !saw_disabled) {
            printf("FAIL win: CheckItem/DisableItem did not reach the hook\n");
            failures++;
        }
        if (!saw_text) {
            printf("FAIL win: SetMenuItemText did not reach the hook\n");
            failures++;
        }
    }
    {
        unsigned i, late = 0u;

        for (i = 0u; i < win_count; i++)
            if (win_log[i].op == QRX_M68K_WIN_CREATE &&
                strcmp(win_log[i].title, "Zweite") == 0)
                late = 1u;
        if (!late) {
            printf("FAIL win: the window after DrawMenuBar never arrived\n");
            failures++;
        }
    }
    if (w && w->id != wr) {
        printf("FAIL win: the window hook did not carry the record\n");
        failures++;
    }
    if (!rec_targeted || rec_target_id != wr) {
        printf("FAIL win: SetPort did not name the window record\n");
        failures++;
    }
    if (clip_mem_len != 5u || memcmp(clip_mem, "Hallo", 5) != 0) {
        printf("FAIL win: PutScrap did not reach the clipboard\n");
        failures++;
    }
    if (rd32(m.mem + 0x100000u + QW_SLEN) != 5u) {
        fail(16u * 1024u * 1024u, "GetScrap size",
             rd32(m.mem + 0x100000u + QW_SLEN), 5u);
    }
    {
        uint32_t h = rd32(m.mem + 0x100000u + QW_HANDLE);

        if (h == 0u) {
            printf("FAIL win: NewHandle answered nil\n");
            failures++;
        } else {
            uint32_t block = rd32(m.mem + h);

            if (memcmp(m.mem + block, "Hallo", 5) != 0) {
                printf("FAIL win: GetScrap did not fill the handle "
                       "(h=%08X block=%08X bytes %02X %02X %02X %02X %02X)\n",
                       h, block, m.mem[block], m.mem[block + 1],
                       m.mem[block + 2], m.mem[block + 3], m.mem[block + 4]);
                failures++;
            }
        }
    }
    if (rd32(m.mem + 0x100000u + QW_MSEL) != 0x00800001u) {
        fail(16u * 1024u * 1024u, "MenuSelect",
             rd32(m.mem + 0x100000u + QW_MSEL), 0x00800001u);
    }
    if (rd32(m.mem + 0x100000u + QW_MKEY) != 0x00800002u) {
        fail(16u * 1024u * 1024u, "MenuKey('q')",
             rd32(m.mem + 0x100000u + QW_MKEY), 0x00800002u);
    }
    {
        uint32_t w1 = rd32(m.mem + 0x100000u + QW_WIN);
        uint32_t w3 = rd32(m.mem + 0x100000u + QW_WIN3);

        if (w1 == 0u || w3 != w1)
            fail(16u * 1024u * 1024u, "NewWindow after DisposeWindow", w3,
                 w1);
        {
            unsigned k;
            int live = 0;

            for (k = 0u; k < QRX_M68K_WINDOW_MAX; k++)
                if (m.win[k].used)
                    live++;
            if (live != 0) {
                printf("FAIL win: %d window(s) left open\n", live);
                failures++;
            }
        }
        if (m.front != 0u)
            fail(16u * 1024u * 1024u, "front after DisposeWindow", m.front,
                 0u);
        /* Only the record floats: menus above it were never freed, so it
         * cannot be merged into the bump pointer. */
        if (m.nheap_free != 1u || m.heap_free[0].addr != w1)
            fail(16u * 1024u * 1024u, "free blocks after CloseWindow",
                 m.nheap_free, 1u);
    }
    if (m.mem[0x100000u + QW_CMDREAD] != (uint8_t)'Q') {
        fail(16u * 1024u * 1024u, "GetItemCmd",
             (uint32_t)m.mem[0x100000u + QW_CMDREAD], (uint32_t)'Q');
    }
    {
        uint32_t ch = rd32(m.mem + 0x100000u + QW_CTRL);

        /* The run ends after the dispose checks below, so `used` is already
         * cleared here; the fields of the record are still readable. */
        if (ch == 0u) {
            printf("FAIL win: NewControl answered nil\n");
            failures++;
        } else {
            if (m.ctrl[0].owner != wr || !m.ctrl[0].visible) {
                printf("FAIL win: the control did not keep owner or visible\n");
                failures++;
            }
            if (m.ctrl[0].top != 100 || m.ctrl[0].left != 20 ||
                m.ctrl[0].bottom != 120 || m.ctrl[0].right != 60) {
                printf("FAIL win: the control bounds are not 100,20,120,60\n");
                failures++;
            }
            if (strcmp(m.ctrl[0].title, "OK") != 0 || m.ctrl[0].min != 0 ||
                m.ctrl[0].max != 1) {
                printf("FAIL win: title/min/max did not arrive (\"%s\" %d %d)\n",
                       m.ctrl[0].title, (int)m.ctrl[0].min,
                       (int)m.ctrl[0].max);
                failures++;
            }
            if (rd16(m.mem + 0x100000u + QW_CVAL2) != 2u)
                fail(16u * 1024u * 1024u, "SetControlValue",
                     rd16(m.mem + 0x100000u + QW_CVAL2), 2u);
            if (rd16(m.mem + 0x100000u + QW_CVAL) != 1u)
                fail(16u * 1024u * 1024u, "GetControlValue",
                     rd16(m.mem + 0x100000u + QW_CVAL), 1u);
            if (rd32(m.mem + 0x100000u + QW_CFIND) != ch) {
                fail(16u * 1024u * 1024u, "FindControl handle",
                     rd32(m.mem + 0x100000u + QW_CFIND), ch);
            }
            if (rd16(m.mem + 0x100000u + QW_CHIT) != 10u)
                fail(16u * 1024u * 1024u, "FindControl part",
                     rd16(m.mem + 0x100000u + QW_CHIT), 10u);
            if (rd16(m.mem + 0x100000u + QW_CHIT2) != 10u)
                fail(16u * 1024u * 1024u, "TrackControl part",
                     rd16(m.mem + 0x100000u + QW_CHIT2), 10u);
            if (m.mem[0x100000u + QW_CTBUF] != 2u ||
                memcmp(m.mem + 0x100000u + QW_CTBUF + 1u, "OK", 2) != 0) {
                printf("FAIL win: GetControlTitle did not return \"OK\"\n");
                failures++;
            }
        }
    }
    {
        uint32_t c1 = rd32(m.mem + 0x100000u + QW_CTRL);
        uint32_t c2 = rd32(m.mem + 0x100000u + QW_CTRL2);
        uint32_t c3 = rd32(m.mem + 0x100000u + QW_CTRL3);
        uint32_t c4 = rd32(m.mem + 0x100000u + QW_CTRL4);

        if (c1 == 0u || c2 != c1)
            fail(16u * 1024u * 1024u, "NewControl after DisposeControl", c2,
                 c1);
        if (c3 != c1 + 8u)
            fail(16u * 1024u * 1024u, "second NewControl", c3, c1 + 8u);
        if (c4 != c1)
            fail(16u * 1024u * 1024u, "NewControl after KillControls", c4,
                 c1);
        if (m.heap != c1)
            fail(16u * 1024u * 1024u, "heap top after DisposeControl", m.heap,
                 c1);
        if (m.ctrl[0].used) {
            printf("FAIL win: DisposeControl left the record marked used\n");
            failures++;
        }
    }
    if (rd32(m.mem + 0x100000u + QW_MKEY2) != 0u) {
        fail(16u * 1024u * 1024u, "MenuKey('O') on a disabled item",
             rd32(m.mem + 0x100000u + QW_MKEY2), 0u);
    }
    if (m.menu_point_y != 8 || m.menu_point_x != 20) {
        printf("FAIL win: MenuSelect did not read its startPt (%ld,%ld)\n",
               (long)m.menu_point_y, (long)m.menu_point_x);
        failures++;
    }
    if (rd16(m.mem + 0x100000u + QW_EV1) != 15u ||
        rd16(m.mem + 0x100000u + QW_EV2) != 1u ||
        rd16(m.mem + 0x100000u + QW_EV3) != 3u) {
        printf("FAIL win: the pump events did not arrive in order\n");
        failures++;
    }

    printf("window/menu: %u window call(s), %u menu call(s) %s\n", win_count,
           menu_count, failures == before ? "ok" : "FAILED");
}

/* Packed segments have to be reported instead of being run as if the packer
 * header were code; ordinary 68k code must not be mistaken for a header. */
static void packed_case(uint8_t *fork, uint32_t len)
{
    static const uint32_t off1 = 0x0100u + 4u + (uint32_t)sizeof(code0) + 4u;
    static qrx_m68k_t m;
    qrx_m68k_status_t st;

    /* VISE/InstallerMaker: A89F 000C, CRC, unpacked size, packed size. */
    wr32(fork + off1, 0xA89F000Cu);
    wr32(fork + off1 + 4u, 0x11223344u);
    wr32(fork + off1 + 8u, 2u * (uint32_t)sizeof(code1));
    wr32(fork + off1 + 12u, (uint32_t)sizeof(code1));
    qrx_m68k_unload(&m);
    st = qrx_m68k_load(&m, fork, len);
    if (st != QRX_M68K_ERR_PACKED) {
        printf("FAIL packed VISE: load = %s\n", qrx_m68k_status_name(st));
        failures++;
    } else {
        printf("packed VISE header: refused with ERR_PACKED ok\n");
    }

    /* Aladdin ADC: 'ADCR' in front of the segment. */
    wr32(fork + off1, 0x41444352u);
    st = qrx_m68k_load(&m, fork, len);
    if (st != QRX_M68K_ERR_PACKED) {
        printf("FAIL packed ADC: load = %s\n", qrx_m68k_status_name(st));
        failures++;
    } else {
        printf("packed ADC header: refused with ERR_PACKED ok\n");
    }
}

int main(int argc, char **argv)
{
    static uint8_t fork[8192];
    uint32_t len = build_fork(fork, (uint32_t)sizeof(fork), code1,
                              (uint32_t)sizeof(code1));

    if (len == 0u) {
        printf("FAIL: could not build the test image\n");
        return 1;
    }
    if (argc > 1) {
        /* Write the File Manager program as a MacBinary for guest runs. */
        static uint8_t seg[FS_SEG_LEN];
        static uint8_t mb[128 + 1024];
        uint32_t seg_len = fs_program(seg, (uint32_t)sizeof seg);
        uint32_t fk = seg_len ? build_fork(fork, (uint32_t)sizeof(fork), seg,
                                           seg_len) : 0u;
        char path[512];
        FILE *out;

        if (fk == 0u) {
            printf("FAIL: could not build the file manager image\n");
            return 1;
        }
        memset(mb, 0, sizeof mb);
        mb[1] = 6;
        memcpy(mb + 2, "TestFS", 6);
        memcpy(mb + 65, "APPL", 4);
        memcpy(mb + 69, "qRXA", 4);
        wr32(mb + 87, fk);
        memcpy(mb + 128, fork, fk);
        snprintf(path, sizeof path, "%s/TestFS.mb", argv[1]);
        out = fopen(path, "wb");
        if (!out) {
            printf("FAIL: cannot write %s\n", path);
            return 1;
        }
        fwrite(mb, 1, 128u + fk, out);
        fclose(out);
        printf("wrote %s (%u bytes)\n", path, 128u + fk);

        /* The QuickDraw program travels with its 'DLOG' and 'DITL'. */
        {
            static uint8_t qseg[QD_SEG_LEN];
            static uint8_t qmb[256 + 2048];
            test_res_t extras[2];
            uint32_t qlen = qd_program(qseg, (uint32_t)sizeof qseg);

            extras[0].type = 0x444C4F47u;
            extras[0].id = 128;
            extras[0].data = dlog_res;
            extras[0].len = (uint32_t)sizeof dlog_res;
            extras[1].type = 0x4449544Cu;
            extras[1].id = 128;
            extras[1].data = ditl_res;
            extras[1].len = (uint32_t)sizeof ditl_res;
            qlen = qlen ? build_fork_ex(fork, (uint32_t)sizeof fork, qseg,
                                        (uint32_t)sizeof qseg, extras, 2u)
                        : 0u;
            if (qlen == 0u || 128u + qlen > sizeof qmb) {
                printf("FAIL: could not build the QuickDraw image\n");
                return 1;
            }
            memset(qmb, 0, sizeof qmb);
            qmb[1] = 6;
            memcpy(qmb + 2, "TestQD", 6);
            memcpy(qmb + 65, "APPL", 4);
            memcpy(qmb + 69, "qRXA", 4);
            wr32(qmb + 87, qlen);
            memcpy(qmb + 128, fork, qlen);
            snprintf(path, sizeof path, "%s/TestQD.mb", argv[1]);
            out = fopen(path, "wb");
            if (!out) {
                printf("FAIL: cannot write %s\n", path);
                return 1;
            }
            fwrite(qmb, 1, 128u + qlen, out);
            fclose(out);
            printf("wrote %s (%u bytes)\n", path, 128u + qlen);
        }

        /* The Window/Menu program needs no resources of its own. */
        {
            static uint8_t wseg[QW_SEG_LEN];
            static uint8_t wmb[256 + 4096];
            uint32_t wlen = win_program(wseg, (uint32_t)sizeof wseg);

            wlen = wlen ? build_fork(fork, (uint32_t)sizeof fork, wseg,
                                     (uint32_t)sizeof wseg)
                        : 0u;
            if (wlen == 0u || 128u + wlen > sizeof wmb) {
                printf("FAIL: could not build the window image\n");
                return 1;
            }
            memset(wmb, 0, sizeof wmb);
            wmb[1] = 7;
            memcpy(wmb + 2, "TestWin", 7);
            memcpy(wmb + 65, "APPL", 4);
            memcpy(wmb + 69, "qRXA", 4);
            wr32(wmb + 87, wlen);
            memcpy(wmb + 128, fork, wlen);
            snprintf(path, sizeof path, "%s/TestWin.mb", argv[1]);
            out = fopen(path, "wb");
            if (!out) {
                printf("FAIL: cannot write %s\n", path);
                return 1;
            }
            fwrite(wmb, 1, 128u + wlen, out);
            fclose(out);
            printf("wrote %s (%u bytes)\n", path, 128u + wlen);
        }
        return 0;
    }
    one_case(fork, len, 16u * 1024u * 1024u, 1);
    /* The guest is 32-bit, so a larger request must not be clamped to 16 MiB. */
    one_case(fork, len, 64u * 1024u * 1024u, 0);
    one_case(fork, len, 8u * 1024u * 1024u, 0);
    one_case(fork, len, 4u * 1024u * 1024u, 0);
    refuse_case(fork, len, 2u * 1024u * 1024u);
    packed_case(fork, len);
    sysenv_case(16u * 1024u * 1024u);
    sysenv_case(64u * 1024u * 1024u);
    resmem_case();
    fs_case();
    qd_case();
    win_case();
    if (failures) {
        printf("m68k heap test: %d failure(s)\n", failures);
        return 1;
    }
    printf("m68k heap test: 0 failure(s)\n");
    return 0;
}
