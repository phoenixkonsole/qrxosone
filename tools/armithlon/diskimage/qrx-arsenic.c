/*
 * Method 15 ("Arsenic") decoder for StuffIt 5 resource/data forks.
 * Ported from XADMaster's LGPL XADStuffItArsenicHandle.m / BWT.c.
 * The caller owns the compressed and output buffers.
 */
#include <exec/exec.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <stdint.h>
#include <string.h>
#include "qrx-arsenic.h"

#define A_MAX_SYMBOLS 128

typedef struct {
    int symbol[A_MAX_SYMBOLS];
    int frequency[A_MAX_SYMBOLS];
    int totalfrequency;
    int increment;
    int frequencylimit;
    int numsymbols;
} AModel;

typedef struct {
    const uint8_t *data;
    uint32_t length;
    uint32_t bitpos;
} ABits;

typedef struct {
    ABits input;
    int range;
    int code;
} ADecoder;

typedef struct {
    int table[256];
} MTFState;

static const uint16_t RandomizationTable[256] = {
    0x0ee, 0x056, 0x0f8, 0x0c3, 0x09d, 0x09f, 0x0ae, 0x02c,
    0x0ad, 0x0cd, 0x024, 0x09d, 0x0a6, 0x101, 0x018, 0x0b9,
    0x0a1, 0x082, 0x075, 0x0e9, 0x09f, 0x055, 0x066, 0x06a,
    0x086, 0x071, 0x0dc, 0x084, 0x056, 0x096, 0x056, 0x0a1,
    0x084, 0x078, 0x0b7, 0x032, 0x06a, 0x003, 0x0e3, 0x002,
    0x011, 0x101, 0x008, 0x044, 0x083, 0x100, 0x043, 0x0e3,
    0x01c, 0x0f0, 0x086, 0x06a, 0x06b, 0x00f, 0x003, 0x02d,
    0x086, 0x017, 0x07b, 0x010, 0x0f6, 0x080, 0x078, 0x07a,
    0x0a1, 0x0e1, 0x0ef, 0x08c, 0x0f6, 0x087, 0x04b, 0x0a7,
    0x0e2, 0x077, 0x0fa, 0x0b8, 0x081, 0x0ee, 0x077, 0x0c0,
    0x09d, 0x029, 0x020, 0x027, 0x071, 0x012, 0x0e0, 0x06b,
    0x0d1, 0x07c, 0x00a, 0x089, 0x07d, 0x087, 0x0c4, 0x101,
    0x0c1, 0x031, 0x0af, 0x038, 0x003, 0x068, 0x01b, 0x076,
    0x079, 0x03f, 0x0db, 0x0c7, 0x01b, 0x036, 0x07b, 0x0e2,
    0x063, 0x081, 0x0ee, 0x00c, 0x063, 0x08b, 0x078, 0x038,
    0x097, 0x09b, 0x0d7, 0x08f, 0x0dd, 0x0f2, 0x0a3, 0x077,
    0x08c, 0x0c3, 0x039, 0x020, 0x0b3, 0x012, 0x011, 0x00e,
    0x017, 0x042, 0x080, 0x02c, 0x0c4, 0x092, 0x059, 0x0c8,
    0x0db, 0x040, 0x076, 0x064, 0x0b4, 0x055, 0x01a, 0x09e,
    0x0fe, 0x05f, 0x006, 0x03c, 0x041, 0x0ef, 0x0d4, 0x0aa,
    0x098, 0x029, 0x0cd, 0x01f, 0x002, 0x0a8, 0x087, 0x0d2,
    0x0a0, 0x093, 0x098, 0x0ef, 0x00c, 0x043, 0x0ed, 0x09d,
    0x0c2, 0x0eb, 0x081, 0x0e9, 0x064, 0x023, 0x068, 0x01e,
    0x025, 0x057, 0x0de, 0x09a, 0x0cf, 0x07f, 0x0e5, 0x0ba,
    0x041, 0x0ea, 0x0ea, 0x036, 0x01a, 0x028, 0x079, 0x020,
    0x05e, 0x018, 0x04e, 0x07c, 0x08e, 0x058, 0x07a, 0x0ef,
    0x091, 0x002, 0x093, 0x0bb, 0x056, 0x0a1, 0x049, 0x01b,
    0x079, 0x092, 0x0f3, 0x058, 0x04f, 0x052, 0x09c, 0x002,
    0x077, 0x0af, 0x02a, 0x08f, 0x049, 0x0d0, 0x099, 0x04d,
    0x098, 0x101, 0x060, 0x093, 0x100, 0x075, 0x031, 0x0ce,
    0x049, 0x020, 0x056, 0x057, 0x0e2, 0x0f5, 0x026, 0x02b,
    0x08a, 0x0bf, 0x0de, 0x0d0, 0x083, 0x034, 0x0f4, 0x017,
};

static int abits_get(ABits *in, int count)
{
    int value = 0;
    int i;
    if (count < 0 || count > 26)
        return -1;
    if (in->bitpos + (uint32_t)count > in->length * 8u)
        return -1;
    for (i = 0; i < count; i++) {
        uint32_t p = in->bitpos++;
        value = (value << 1) |
                ((in->data[p >> 3] >> (7 - (p & 7))) & 1);
    }
    return value;
}

static void amodel_init(AModel *m, int first, int last,
                        int increment, int limit)
{
    int i;
    m->increment = increment;
    m->frequencylimit = limit;
    m->numsymbols = last - first + 1;
    for (i = 0; i < m->numsymbols; i++)
        m->symbol[i] = first + i;
    m->totalfrequency = increment * m->numsymbols;
    for (i = 0; i < m->numsymbols; i++)
        m->frequency[i] = increment;
}

static void amodel_reset(AModel *m)
{
    int i;
    m->totalfrequency = m->increment * m->numsymbols;
    for (i = 0; i < m->numsymbols; i++)
        m->frequency[i] = m->increment;
}

static void amodel_increase(AModel *m, int index)
{
    int i;
    m->frequency[index] += m->increment;
    m->totalfrequency += m->increment;
    if (m->totalfrequency > m->frequencylimit) {
        m->totalfrequency = 0;
        for (i = 0; i < m->numsymbols; i++) {
            m->frequency[i]++;
            m->frequency[i] >>= 1;
            m->totalfrequency += m->frequency[i];
        }
    }
}

static void adec_init(ADecoder *d, ABits *bits)
{
    d->input = *bits;
    d->range = 1 << 25;
    d->code = abits_get(&d->input, 26);
    *bits = d->input;
}

static void adec_update(ADecoder *d, int symlow, int symsize, int symtot)
{
    int factor = d->range / symtot;
    int lowincr = factor * symlow;
    d->code -= lowincr;
    if (symlow + symsize == symtot)
        d->range -= lowincr;
    else
        d->range = symsize * factor;
    while (d->range <= (1 << 24)) {
        d->range <<= 1;
        d->code = (d->code << 1) | abits_get(&d->input, 1);
    }
}

static int asym(ADecoder *d, AModel *m)
{
    int frequency;
    int cumulative = 0;
    int n;
    int divisor;
    if (m->totalfrequency <= 0)
        return -1;
    divisor = d->range / m->totalfrequency;
    if (divisor <= 0)
        return -1;
    frequency = d->code / divisor;
    for (n = 0; n < m->numsymbols - 1; n++) {
        if (cumulative + m->frequency[n] > frequency)
            break;
        cumulative += m->frequency[n];
    }
    adec_update(d, cumulative, m->frequency[n], m->totalfrequency);
    amodel_increase(m, n);
    return m->symbol[n];
}

static int abits_symbol(ADecoder *d, AModel *m, int count)
{
    int result = 0;
    int i;
    for (i = 0; i < count; i++) {
        int bit = asym(d, m);
        if (bit < 0)
            return -1;
        result |= bit << i;
    }
    return result;
}

static void mtf_reset(MTFState *m)
{
    int i;
    for (i = 0; i < 256; i++)
        m->table[i] = i;
}

static int mtf_decode(MTFState *m, int symbol)
{
    int result;
    int i;
    if (symbol < 0 || symbol > 255)
        return 0;
    result = m->table[symbol];
    for (i = symbol; i > 0; i--)
        m->table[i] = m->table[i - 1];
    m->table[0] = result;
    return result;
}

static void inverse_bwt(uint32_t *transform, uint8_t *block, int blocklen)
{
    int counts[256];
    int cumulative[256];
    int total = 0;
    int i;
    memset(counts, 0, sizeof(counts));
    for (i = 0; i < blocklen; i++)
        counts[block[i]]++;
    for (i = 0; i < 256; i++) {
        cumulative[i] = total;
        total += counts[i];
        counts[i] = 0;
    }
    for (i = 0; i < blocklen; i++) {
        transform[cumulative[block[i]] + counts[block[i]]] = (uint32_t)i;
        counts[block[i]]++;
    }
}

typedef struct {
    ADecoder decoder;
    AModel initial;
    AModel selector;
    AModel mtf[7];
    MTFState mtfstate;
    int blockbits;
    int blocksize;
    uint8_t *block;
    uint32_t *transform;
    int numbytes;
    int transformindex;
    int randomized;
    int endofblocks;
} AState;

static int arsenic_read_block(AState *s)
{
    int sel;
    int symbol;
    int i;

    mtf_reset(&s->mtfstate);
    s->randomized = asym(&s->decoder, &s->initial);
    if (s->randomized < 0)
        return 0;
    s->transformindex = abits_symbol(&s->decoder, &s->initial, s->blockbits);
    if (s->transformindex < 0)
        return 0;
    s->numbytes = 0;

    for (;;) {
        sel = asym(&s->decoder, &s->selector);
        if (sel < 0)
            return 0;
        if (sel == 0 || sel == 1) {
            int zerostate = 1;
            int zerocount = 0;
            while (sel < 2) {
                if (sel == 0)
                    zerocount += zerostate;
                else
                    zerocount += 2 * zerostate;
                zerostate *= 2;
                sel = asym(&s->decoder, &s->selector);
                if (sel < 0)
                    return 0;
            }
            if (s->numbytes + zerocount > s->blocksize)
                return 0;
            memset(s->block + s->numbytes, mtf_decode(&s->mtfstate, 0),
                   (size_t)zerocount);
            s->numbytes += zerocount;
        }
        if (sel == 10)
            break;
        if (sel == 2)
            symbol = 1;
        else if (sel >= 3 && sel <= 9)
            symbol = asym(&s->decoder, &s->mtf[sel - 3]);
        else
            return 0;
        if (symbol < 0 || s->numbytes >= s->blocksize)
            return 0;
        s->block[s->numbytes++] = (uint8_t)mtf_decode(&s->mtfstate, symbol);
    }

    if (s->transformindex >= s->numbytes)
        return 0;

    amodel_reset(&s->selector);
    for (i = 0; i < 7; i++)
        amodel_reset(&s->mtf[i]);

    if (asym(&s->decoder, &s->initial)) {
        if (abits_symbol(&s->decoder, &s->initial, 32) < 0)
            return 0;
        s->endofblocks = 1;
    }
    inverse_bwt(s->transform, s->block, s->numbytes);
    return 1;
}

int qrx_arsenic_decode(const uint8_t *src, uint32_t srclen,
                       uint8_t *dst, uint32_t dstlen)
{
    static AState s;
    ABits bits;
    uint32_t out = 0;
    int bytecount = 0;
    int count = 0;
    int last = 0;
    int repeat = 0;
    int randindex = 0;
    int randcount = 0;

    memset(&s, 0, sizeof(s));
    bits.data = src;
    bits.length = srclen;
    bits.bitpos = 0;
    adec_init(&s.decoder, &bits);
    if (s.decoder.code < 0)
        return 0;

    amodel_init(&s.initial, 0, 1, 1, 256);
    amodel_init(&s.selector, 0, 10, 8, 1024);
    amodel_init(&s.mtf[0], 2, 3, 8, 1024);
    amodel_init(&s.mtf[1], 4, 7, 4, 1024);
    amodel_init(&s.mtf[2], 8, 15, 4, 1024);
    amodel_init(&s.mtf[3], 16, 31, 4, 1024);
    amodel_init(&s.mtf[4], 32, 63, 2, 1024);
    amodel_init(&s.mtf[5], 64, 127, 2, 1024);
    amodel_init(&s.mtf[6], 128, 255, 1, 1024);

    if (abits_symbol(&s.decoder, &s.initial, 8) != 'A')
        return 0;
    if (abits_symbol(&s.decoder, &s.initial, 8) != 's')
        return 0;
    s.blockbits = abits_symbol(&s.decoder, &s.initial, 4);
    if (s.blockbits < 0)
        return 0;
    s.blockbits += 9;
    s.blocksize = 1 << s.blockbits;
    if (s.blocksize <= 0)
        return 0;
    s.block = (uint8_t *)AllocMem((uint32_t)s.blocksize, MEMF_ANY);
    s.transform = (uint32_t *)AllocMem((uint32_t)sizeof(uint32_t) *
                                      (uint32_t)s.blocksize, MEMF_ANY);
    if (!s.block || !s.transform) {
        if (s.block) FreeMem(s.block, (uint32_t)s.blocksize);
        if (s.transform)
            FreeMem(s.transform, (uint32_t)sizeof(uint32_t) *
                    (uint32_t)s.blocksize);
        return 0;
    }
    s.numbytes = 0;
    s.endofblocks = asym(&s.decoder, &s.initial);
    if (s.endofblocks < 0)
        goto fail;

    while (out < dstlen) {
        int outbyte;
        if (repeat) {
            repeat--;
            outbyte = last;
        } else {
retry:
            if (bytecount >= s.numbytes) {
                if (s.endofblocks)
                    goto fail;
                if (!arsenic_read_block(&s))
                    goto fail;
                bytecount = 0;
                count = 0;
                last = 0;
                randindex = 0;
                randcount = RandomizationTable[0];
            }
            s.transformindex = (int)s.transform[s.transformindex];
            outbyte = s.block[s.transformindex];
            if (s.randomized && randcount == bytecount) {
                outbyte ^= 1;
                randindex = (randindex + 1) & 255;
                randcount += RandomizationTable[randindex];
            }
            bytecount++;
            if (count == 4) {
                count = 0;
                if (outbyte == 0)
                    goto retry;
                repeat = outbyte - 1;
                outbyte = last;
            } else {
                if (outbyte == last)
                    count++;
                else {
                    count = 1;
                    last = outbyte;
                }
            }
        }
        dst[out++] = (uint8_t)outbyte;
    }
    FreeMem(s.block, (uint32_t)s.blocksize);
    FreeMem(s.transform, (uint32_t)sizeof(uint32_t) * (uint32_t)s.blocksize);
    return 1;

fail:
    if (s.block) FreeMem(s.block, (uint32_t)s.blocksize);
    if (s.transform)
        FreeMem(s.transform, (uint32_t)sizeof(uint32_t) *
                (uint32_t)s.blocksize);
    return 0;
}
