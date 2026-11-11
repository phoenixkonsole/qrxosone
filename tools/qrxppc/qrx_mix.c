/*
 * QRXPPC P1.5 - mixed-mode bridge between 68k callers and PPC code.
 *
 * ABI contract (subset of PowerOpen / MacOS 9 used by Mixed Mode glue):
 *
 *   r1   stack pointer, 16-byte aligned.  Linkage area at 0(r1):
 *          0(r1)  caller's SP (back chain)
 *          4(r1)  saved CR
 *          8(r1)  saved LR
 *          12(r1) reserved
 *          16(r1)..23(r1) reserved
 *          24(r1).. parameter area for stack arguments
 *   r2   RTOC
 *   r3..r10  integer arguments, result in r3 (and r4)
 *   f1..f8   floating-point arguments, result in f1
 *   LR   return address, set to the glue sentinel
 *
 * Argument placement follows a qrx_mix_sig_t.  Integer-class parameters
 * (`i`, `p`) take the next free r3..r10, doubles (`d`) the next free f1..f8,
 * each class with its own cursor, and `l` needs an even/odd register pair.
 * Every parameter that does not fit in its class's registers goes to the
 * parameter area at 24(r1), in argument order, 4-byte aligned (8-byte aligned
 * for `l` and `d`, high word first).  A `l` whose register cursor is odd is
 * spilled even when a later pair would still be free, because PowerOpen never
 * splits a pair.
 *
 * The 68k side enters PPC code with qrx_mix_call().  The PPC routine returns
 * by branching to the sentinel, a single `sc` word the bridge places at
 * glue_addr.  A PPC routine calls a 68k routine by putting the import index in
 * r0 and executing `sc`; the bridge marshals the arguments named by the
 * import's signature, calls the registered 68k callback and continues after
 * the `sc` with the callback result placed according to the return type.
 * Without a signature the bridge forwards r3..r10 and f1..f8 verbatim, which
 * keeps unspecified imports callable.
 *
 * Nested calls (a 68k callback that calls back into PPC) get their glue frame
 * below the current PPC stack pointer and the complete PPC state is saved and
 * restored, so the outer call continues unchanged.  This is stricter than a
 * real glue stub, which preserves only the nonvolatile registers.
 *
 * A Mixed Mode routine descriptor can serve as the entry point as well:
 * qrx_mix_rd_resolve() validates the descriptor in guest memory (trap word,
 * version, record table, flags) and resolves the PPC record's entry point,
 * which qrx_mix_rd_call() then uses with the same signature machinery.  A
 * register-based record's ProcInfo can supply the placement itself
 * (qrx_mix_rd_procinfo_sig / qrx_mix_rd_invoke_procinfo), so a caller that
 * holds only a UniversalProcPtr needs no signature of its own; a NULL
 * signature to qrx_mix_rd_invoke() takes the same path.  Imports can
 * use a descriptor too: an r0 value
 * that is not an import index is taken as the address of a registered routine
 * descriptor, whose 68k record is resolved and passed to the callback as
 * args->proc.
 *
 * Limits: no aggregates, unions or varargs in signatures, no single-precision
 * distinction (`float` is passed as a double), and an import's stack words are
 * exposed as raw big-endian words.  The return-to-68k path performs no stack
 * unwinding beyond checking that the callee restored r1.  Routine descriptor
 * support covers single-ISA and fat descriptors only: dispatched descriptors
 * (selectors), fragments that still need preparing and 68k code are rejected.
 */
#include "qrx_mix.h"

#include <string.h>

#define QRX_MIX_GLUE_WORD 0x44000002u   /* sc */
#define QRX_MIX_IMPORT_GUARD 256u       /* trap round trips per call */
#define MIX_SLOT_NONE 0xFFu

typedef struct {
    unsigned reg;   /* register index inside its class, MIX_SLOT_NONE if spilled */
    unsigned word;  /* first parameter-area word when spilled */
} mix_slot_t;

static uint64_t double_bits(double value)
{
    uint64_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double bits_double(uint64_t bits)
{
    double value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int mix_read32(qrx_mix_t *mix, uint32_t addr, uint32_t *out)
{
    qrx_ppc_t *p = mix->ppc;
    const uint8_t *m;

    if (addr > p->mem.size || p->mem.size - addr < 4u)
        return 0;
    m = p->mem.data + addr;
    *out = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) |
           ((uint32_t)m[2] << 8) | (uint32_t)m[3];
    return 1;
}

static int mix_read8(qrx_mix_t *mix, uint32_t addr, uint8_t *out)
{
    if (addr >= mix->ppc->mem.size)
        return 0;
    *out = mix->ppc->mem.data[addr];
    return 1;
}

static int mix_read16(qrx_mix_t *mix, uint32_t addr, uint16_t *out)
{
    const uint8_t *m;

    if (addr > mix->ppc->mem.size || mix->ppc->mem.size - addr < 2u)
        return 0;
    m = mix->ppc->mem.data + addr;
    *out = (uint16_t)(((uint16_t)m[0] << 8) | (uint16_t)m[1]);
    return 1;
}

static int mix_write32(qrx_mix_t *mix, uint32_t addr, uint32_t value)
{
    qrx_ppc_t *p = mix->ppc;
    uint8_t *m;

    if (addr > p->mem.size || p->mem.size - addr < 4u)
        return 0;
    m = p->mem.data + addr;
    m[0] = (uint8_t)(value >> 24);
    m[1] = (uint8_t)(value >> 16);
    m[2] = (uint8_t)(value >> 8);
    m[3] = (uint8_t)value;
    return 1;
}

/* Write the linkage area and, if present, the stack parameter area. */
static int mix_write_frame(qrx_mix_t *mix, uint32_t sp, uint32_t back_chain,
                           const qrx_mix_args_t *args)
{
    unsigned i;

    if (!mix_write32(mix, sp + 0u, back_chain) ||
        !mix_write32(mix, sp + 4u, 0u) ||
        !mix_write32(mix, sp + 8u, 0u) ||
        !mix_write32(mix, sp + 12u, 0u) ||
        !mix_write32(mix, sp + 16u, 0u) ||
        !mix_write32(mix, sp + 20u, 0u))
        return 0;
    for (i = 0; i < (args ? args->stack_count : 0u); i++) {
        if (!mix_write32(mix, sp + QRX_MIX_LINKAGE + 4u * i, args->stack[i]))
            return 0;
    }
    return 1;
}

static int mix_sig_valid(const qrx_mix_sig_t *sig)
{
    unsigned i;

    if (!sig || sig->nparams > QRX_MIX_SIG_MAX)
        return 0;
    if (sig->ret != 'v' && sig->ret != 'i' && sig->ret != 'l' &&
        sig->ret != 'f')
        return 0;
    for (i = 0; i < sig->nparams; i++) {
        char c = sig->params[i];

        if (c != 'i' && c != 'p' && c != 'l' && c != 'd')
            return 0;
    }
    return 1;
}

/* Assign registers and parameter-area words to every declared parameter. */
static int mix_layout(const qrx_mix_sig_t *sig, mix_slot_t *slots,
                      unsigned *stack_words)
{
    unsigned gpr_next = 0u;
    unsigned fpr_next = 0u;
    unsigned words = 0u;
    unsigned i;

    for (i = 0; i < sig->nparams; i++) {
        char c = sig->params[i];

        slots[i].reg = MIX_SLOT_NONE;
        slots[i].word = 0u;
        if (c == 'l') {
            if ((gpr_next & 1u) == 0u && gpr_next + 2u <= QRX_MIX_ARGS) {
                slots[i].reg = gpr_next;
                gpr_next += 2u;
            } else {
                words = (words + 1u) & ~1u;
                slots[i].word = words;
                words += 2u;
            }
        } else if (c == 'd') {
            if (fpr_next < QRX_MIX_ARGS) {
                slots[i].reg = fpr_next;
                fpr_next++;
            } else {
                words = (words + 1u) & ~1u;
                slots[i].word = words;
                words += 2u;
            }
        } else {
            if (gpr_next < QRX_MIX_ARGS) {
                slots[i].reg = gpr_next;
                gpr_next++;
            } else {
                slots[i].word = words;
                words += 1u;
            }
        }
        if (words > QRX_MIX_STACK_WORDS)
            return 0;
    }
    *stack_words = words;
    return 1;
}

/* Marshal the PPC register and parameter-area state into callback arguments. */
static qrx_mix_status_t mix_fill_import(qrx_mix_t *mix, const qrx_mix_sig_t *sig,
                                        qrx_mix_args_t *in)
{
    qrx_ppc_t *p = mix->ppc;
    mix_slot_t slots[QRX_MIX_SIG_MAX];
    unsigned words = 0u;
    unsigned gi = 0u;
    unsigned fi = 0u;
    unsigned i;
    uint32_t sp;
    uint32_t base;

    if (!mix_layout(sig, slots, &words))
        return QRX_MIX_ERR_SIG;
    sp = p->cpu.gpr[1];
    if ((sp & 0xFu) || sp > p->mem.size ||
        p->mem.size - sp < QRX_MIX_LINKAGE)
        return QRX_MIX_ERR_ARG;
    base = sp + QRX_MIX_LINKAGE;
    if (p->mem.size - base < 4u * words)
        return QRX_MIX_ERR_ARG;
    for (i = 0; i < words; i++) {
        if (!mix_read32(mix, base + 4u * i, &in->stack[i]))
            return QRX_MIX_ERR_ARG;
    }
    in->stack_count = words;
    in->sp = base;

    for (i = 0; i < sig->nparams; i++) {
        if (slots[i].reg == MIX_SLOT_NONE)
            continue;
        if (sig->params[i] == 'd') {
            in->fpr[fi++] = bits_double(p->cpu.fpr[1 + slots[i].reg]);
        } else if (sig->params[i] == 'l') {
            in->gpr[gi++] = p->cpu.gpr[3 + slots[i].reg];
            in->gpr[gi++] = p->cpu.gpr[3 + slots[i].reg + 1u];
        } else {
            in->gpr[gi++] = p->cpu.gpr[3 + slots[i].reg];
        }
    }
    in->gpr_count = gi;
    in->fpr_count = fi;
    return QRX_MIX_OK;
}

/* Load the PPC registers and parameter area named by the caller's args. */
static void mix_load_args(qrx_mix_t *mix, const qrx_mix_sig_t *sig,
                          const qrx_mix_args_t *args)
{
    qrx_ppc_t *p = mix->ppc;
    mix_slot_t slots[QRX_MIX_SIG_MAX];
    unsigned words = 0u;
    unsigned gi = 0u;
    unsigned fi = 0u;
    unsigned i;

    (void)mix_layout(sig, slots, &words);
    for (i = 0; i < sig->nparams; i++) {
        if (slots[i].reg == MIX_SLOT_NONE)
            continue;
        if (sig->params[i] == 'd') {
            p->cpu.fpr[1 + slots[i].reg] = double_bits(args->fpr[fi++]);
        } else if (sig->params[i] == 'l') {
            p->cpu.gpr[3 + slots[i].reg] = args->gpr[gi++];
            p->cpu.gpr[3 + slots[i].reg + 1u] = args->gpr[gi++];
        } else {
            p->cpu.gpr[3 + slots[i].reg] = args->gpr[gi++];
        }
    }
}

/* Derive the placement of a descriptor import.  A Mixed Mode descriptor
 * describes both sides of the boundary, and the PPC record's ProcInfo is the
 * one that shapes the call, so it wins when the descriptor has such a record;
 * a descriptor that carries only a 68k record falls back to that record.  The
 * descriptor is read again on every call, like the record itself. */
static qrx_mix_status_t mix_proc_import_sig(qrx_mix_t *mix,
                                            uint32_t descriptor,
                                            const qrx_mix_rd_t *rd68,
                                            qrx_mix_sig_t *sig)
{
    qrx_mix_rd_t rd;
    qrx_mix_status_t rc =
        qrx_mix_rd_resolve(mix, descriptor, QRX_MIX_RD_ISA_PPC, &rd);

    if (rc == QRX_MIX_OK)
        return qrx_mix_rd_procinfo_sig(&rd, sig);
    return qrx_mix_rd_procinfo_sig(rd68, sig);
}

static qrx_mix_status_t mix_run(qrx_mix_t *mix, uint32_t budget,
                                qrx_mix_result_t *out)
{
    qrx_ppc_t *p = mix->ppc;
    uint32_t left = budget;
    unsigned imports = 0;

    memset(out, 0, sizeof(*out));
    for (;;) {
        qrx_ppc_status_t st = qrx_ppc_run(p, left);
        qrx_mix_args_t in;
        qrx_mix_result_t extra;
        qrx_mix_import_fn fn;
        const qrx_mix_sig_t *sig;
        void *ictx;
        uint32_t proc_entry;
        uint32_t sc_pc;
        unsigned index;
        unsigned i;
        qrx_mix_rd_t rd;
        qrx_mix_status_t rd_rc;
        qrx_mix_sig_t derived;

        if (st == QRX_PPC_OK)
            return QRX_MIX_ERR_LIMIT;
        if (st != QRX_PPC_TRAP)
            return QRX_MIX_ERR_RUN;
        if (p->cpu.srr0 == mix->glue_addr) {
            out->gpr[0] = p->cpu.gpr[3];
            out->gpr[1] = p->cpu.gpr[4];
            out->fpr = bits_double(p->cpu.fpr[1]);
            return QRX_MIX_OK;
        }

        index = p->cpu.gpr[0];
        sig = 0;
        ictx = 0;
        proc_entry = 0u;
        if (index < QRX_MIX_IMPORTS) {
            if (!mix->imports[index])
                return QRX_MIX_ERR_IMPORT;
            fn = mix->imports[index];
            ictx = mix->import_ctx[index];
            if (mix->import_desc_used[index]) {
                /* The index carries no ProcInfo; the bound descriptor does,
                 * and it is read again on every call. */
                rd_rc = qrx_mix_rd_resolve(mix, mix->import_desc[index],
                                           QRX_MIX_RD_ISA_68K, &rd);
                if (rd_rc != QRX_MIX_OK)
                    return rd_rc;
                rd_rc = mix_proc_import_sig(mix, mix->import_desc[index], &rd,
                                            &derived);
                if (rd_rc != QRX_MIX_OK)
                    return rd_rc;
                sig = &derived;
            } else if (mix->sig_used[index]) {
                sig = &mix->sigs[index];
            }
        } else {
            unsigned slot;

            for (slot = 0; slot < QRX_MIX_PROCS; slot++) {
                if (mix->proc_used[slot] && mix->proc_desc[slot] == index)
                    break;
            }
            if (slot == QRX_MIX_PROCS)
                return QRX_MIX_ERR_IMPORT;
            rd_rc = qrx_mix_rd_resolve(mix, index, QRX_MIX_RD_ISA_68K, &rd);
            if (rd_rc != QRX_MIX_OK)
                return rd_rc;
            proc_entry = rd.entry;
            fn = mix->proc_fn[slot];
            ictx = mix->proc_ctx[slot];
            if (mix->proc_sig_derived[slot]) {
                /* ProcInfo decides the placement; it is read again on every
                 * call so a changed record is not served from a cache. */
                rd_rc = mix_proc_import_sig(mix, index, &rd, &derived);
                if (rd_rc != QRX_MIX_OK)
                    return rd_rc;
                sig = &derived;
            } else if (mix->proc_sig_used[slot]) {
                sig = &mix->proc_sigs[slot];
            }
        }
        if (++imports > QRX_MIX_IMPORT_GUARD)
            return QRX_MIX_ERR_TRAP;

        sc_pc = p->cpu.srr0;
        memset(&in, 0, sizeof(in));
        in.proc = proc_entry;
        if (sig) {
            qrx_mix_status_t mrc = mix_fill_import(mix, sig, &in);

            if (mrc != QRX_MIX_OK)
                return mrc;
        } else {
            for (i = 0; i < QRX_MIX_ARGS; i++) {
                in.gpr[i] = p->cpu.gpr[3 + i];
                in.fpr[i] = bits_double(p->cpu.fpr[1 + i]);
            }
            in.gpr_count = QRX_MIX_ARGS;
            in.fpr_count = QRX_MIX_ARGS;
        }

        memset(&extra, 0, sizeof(extra));
        p->cpu.gpr[3] = fn(ictx, &in, &extra);               /* D0 */
        if (sig) {
            switch (sig->ret) {
            case 'i':
                break;                          /* return value is r3 */
            case 'l':
                p->cpu.gpr[4] = extra.gpr[1];   /* low half of r3:r4 */
                break;
            case 'f':
                p->cpu.fpr[1] = double_bits(extra.fpr);
                break;
            default:
                break;                          /* void: keep the registers */
            }
        } else {
            p->cpu.gpr[4] = extra.gpr[1];       /* D1 */
            p->cpu.fpr[1] = double_bits(extra.fpr);
        }
        p->cpu.pc = sc_pc + 4u;

        left -= p->insns;
        if (left == 0u)
            return QRX_MIX_ERR_LIMIT;
    }
}

qrx_mix_status_t qrx_mix_sig_parse(qrx_mix_sig_t *sig, const char *params,
                                   char ret)
{
    unsigned n = 0u;

    if (!sig || (!params && ret != 'v'))
        return QRX_MIX_ERR_SIG;
    memset(sig, 0, sizeof(*sig));
    while (params && params[n] != '\0') {
        char c = params[n];

        if (n >= QRX_MIX_SIG_MAX)
            return QRX_MIX_ERR_SIG;
        if (c != 'i' && c != 'p' && c != 'l' && c != 'd')
            return QRX_MIX_ERR_SIG;
        sig->params[n] = c;
        n++;
    }
    sig->nparams = n;
    sig->ret = ret;
    if (!mix_sig_valid(sig))
        return QRX_MIX_ERR_SIG;
    {
        mix_slot_t slots[QRX_MIX_SIG_MAX];
        unsigned words = 0u;

        if (!mix_layout(sig, slots, &words))
            return QRX_MIX_ERR_SIG;
    }
    return QRX_MIX_OK;
}

qrx_mix_status_t qrx_mix_init(qrx_mix_t *mix, qrx_ppc_t *ppc,
                              uint32_t glue_addr, uint32_t sp_top,
                              uint32_t sp_limit)
{
    if (!mix || !ppc || !ppc->mem.data)
        return QRX_MIX_ERR_ARG;
    if ((glue_addr & 3u) || (sp_top & 0xFu))
        return QRX_MIX_ERR_ARG;
    if (sp_limit >= sp_top || sp_top > ppc->mem.size)
        return QRX_MIX_ERR_ARG;

    memset(mix, 0, sizeof(*mix));
    mix->ppc = ppc;
    mix->glue_addr = glue_addr;
    mix->sp_top = sp_top;
    mix->sp_limit = sp_limit;
    if (!mix_write32(mix, glue_addr, QRX_MIX_GLUE_WORD))
        return QRX_MIX_ERR_ARG;
    return QRX_MIX_OK;
}

qrx_mix_status_t qrx_mix_register(qrx_mix_t *mix, unsigned index,
                                  uint32_t descriptor,
                                  qrx_mix_import_fn fn, void *ctx,
                                  const qrx_mix_sig_t *sig)
{
    qrx_mix_rd_t info;
    qrx_mix_sig_t derived;

    if (!mix || !mix->ppc || !fn || index >= QRX_MIX_IMPORTS)
        return QRX_MIX_ERR_ARG;
    if (sig) {
        mix_slot_t slots[QRX_MIX_SIG_MAX];
        unsigned words = 0u;

        if (!mix_sig_valid(sig) || !mix_layout(sig, slots, &words))
            return QRX_MIX_ERR_SIG;
    } else if (descriptor) {
        qrx_mix_status_t rc;

        /* An index carries no ProcInfo of its own, so the descriptor the
         * caller passes is the only source for the placement.  Reject what
         * would be read as an index, and fail here rather than on the first
         * call, like the descriptor import. */
        if (descriptor < QRX_MIX_IMPORTS)
            return QRX_MIX_ERR_ARG;
        rc = qrx_mix_rd_resolve(mix, descriptor, QRX_MIX_RD_ISA_68K, &info);
        if (rc != QRX_MIX_OK)
            return rc;
        rc = mix_proc_import_sig(mix, descriptor, &info, &derived);
        if (rc != QRX_MIX_OK)
            return rc;
    }
    mix->imports[index] = fn;
    mix->import_ctx[index] = ctx;
    memset(&mix->sigs[index], 0, sizeof(mix->sigs[index]));
    if (sig) {
        mix->sigs[index] = *sig;
        mix->sig_used[index] = 1u;
        mix->import_desc[index] = 0u;
        mix->import_desc_used[index] = 0u;
    } else if (descriptor) {
        mix->sig_used[index] = 0u;
        mix->import_desc[index] = descriptor;
        mix->import_desc_used[index] = 1u;
    } else {
        mix->sig_used[index] = 0u;
        mix->import_desc[index] = 0u;
        mix->import_desc_used[index] = 0u;
    }
    return QRX_MIX_OK;
}

qrx_mix_status_t qrx_mix_register_proc(qrx_mix_t *mix, uint32_t descriptor,
                                       qrx_mix_import_fn fn, void *ctx,
                                       const qrx_mix_sig_t *sig)
{
    qrx_mix_rd_t info;
    qrx_mix_status_t rc;
    unsigned slot;

    if (!mix || !mix->ppc || !fn || descriptor < QRX_MIX_IMPORTS)
        return QRX_MIX_ERR_ARG;
    rc = qrx_mix_rd_resolve(mix, descriptor, QRX_MIX_RD_ISA_68K, &info);
    if (rc != QRX_MIX_OK)
        return rc;
    if (sig) {
        mix_slot_t slots[QRX_MIX_SIG_MAX];
        unsigned words = 0u;

        if (!mix_sig_valid(sig) || !mix_layout(sig, slots, &words))
            return QRX_MIX_ERR_SIG;
    } else {
        qrx_mix_sig_t derived;

        /* Fail here rather than on the first call: an import whose ProcInfo
         * cannot describe the placement is not registrable. */
        rc = mix_proc_import_sig(mix, descriptor, &info, &derived);
        if (rc != QRX_MIX_OK)
            return rc;
    }
    for (slot = 0; slot < QRX_MIX_PROCS; slot++) {
        if (mix->proc_used[slot] && mix->proc_desc[slot] == descriptor)
            return QRX_MIX_ERR_ARG;     /* already registered */
    }
    for (slot = 0; slot < QRX_MIX_PROCS; slot++) {
        if (!mix->proc_used[slot])
            break;
    }
    if (slot == QRX_MIX_PROCS)
        return QRX_MIX_ERR_ARG;
    mix->proc_desc[slot] = descriptor;
    mix->proc_fn[slot] = fn;
    mix->proc_ctx[slot] = ctx;
    if (sig) {
        mix->proc_sigs[slot] = *sig;
        mix->proc_sig_used[slot] = 1u;
        mix->proc_sig_derived[slot] = 0u;
    } else {
        memset(&mix->proc_sigs[slot], 0, sizeof(mix->proc_sigs[slot]));
        mix->proc_sig_used[slot] = 0u;
        mix->proc_sig_derived[slot] = 1u;
    }
    mix->proc_used[slot] = 1u;
    return QRX_MIX_OK;
}

qrx_mix_status_t qrx_mix_call(qrx_mix_t *mix, uint32_t entry,
                              const qrx_mix_sig_t *sig,
                              const qrx_mix_args_t *args,
                              qrx_mix_result_t *result, uint32_t budget)
{
    qrx_ppc_t *p;
    qrx_ppc_state_t saved;
    qrx_ppc_status_t saved_status;
    qrx_mix_args_t empty;
    uint32_t saved_fault;
    uint32_t saved_insns;
    uint32_t sp;
    uint32_t top;
    uint32_t words = 0u;
    unsigned nested;
    unsigned i;
    qrx_mix_status_t rc;
    qrx_mix_result_t out;

    if (!args) {
        memset(&empty, 0, sizeof(empty));
        args = &empty;
    }
    if (!mix || !(p = mix->ppc) || !p->mem.data || entry == 0u)
        return QRX_MIX_ERR_ARG;
    if (sig) {
        mix_slot_t slots[QRX_MIX_SIG_MAX];
        unsigned want_gpr = 0u;
        unsigned want_fpr = 0u;

        if (!mix_sig_valid(sig) || !mix_layout(sig, slots, &words))
            return QRX_MIX_ERR_SIG;
        /* Only arguments that actually live in a register are counted; the
         * spilled ones must appear in args->stack in layout order. */
        for (i = 0; i < sig->nparams; i++) {
            if (slots[i].reg == MIX_SLOT_NONE)
                continue;
            if (sig->params[i] == 'd')
                want_fpr++;
            else if (sig->params[i] == 'l')
                want_gpr += 2u;
            else
                want_gpr++;
        }
        if (args->gpr_count != want_gpr || args->fpr_count != want_fpr ||
            args->stack_count != words)
            return QRX_MIX_ERR_ARG;
    } else {
        if (args->gpr_count > QRX_MIX_ARGS ||
            args->fpr_count > QRX_MIX_ARGS ||
            args->stack_count > QRX_MIX_STACK_WORDS)
            return QRX_MIX_ERR_ARG;
        words = args->stack_count;
    }
    if (mix->depth >= QRX_MIX_DEPTH_MAX)
        return QRX_MIX_ERR_ARG;

    nested = mix->depth != 0u;
    top = nested ? p->cpu.gpr[1] : mix->sp_top;
    if (top < QRX_MIX_LINKAGE + 4u * words)
        return QRX_MIX_ERR_ARG;
    sp = (top - QRX_MIX_LINKAGE - 4u * words) & ~0xFu;
    if (sp < mix->sp_limit ||
        sp + QRX_MIX_LINKAGE + 4u * words > top)
        return QRX_MIX_ERR_ARG;
    if (!mix_write_frame(mix, sp, nested ? top : 0u, args))
        return QRX_MIX_ERR_ARG;

    saved = p->cpu;
    saved_status = p->status;
    saved_fault = p->fault_addr;
    saved_insns = p->insns;

    p->cpu.gpr[2] = args->rtoc;
    if (sig) {
        mix_load_args(mix, sig, args);
    } else {
        for (i = 0; i < QRX_MIX_ARGS; i++) {
            p->cpu.gpr[3 + i] = (i < args->gpr_count) ? args->gpr[i] : 0u;
            p->cpu.fpr[1 + i] = (i < args->fpr_count)
                                    ? double_bits(args->fpr[i]) : 0u;
        }
    }
    p->cpu.gpr[1] = sp;
    p->cpu.lr = mix->glue_addr;
    p->cpu.pc = entry;
    p->status = QRX_PPC_OK;

    mix->depth++;
    rc = mix_run(mix, budget, &out);
    mix->depth--;

    if (rc == QRX_MIX_OK && p->cpu.gpr[1] != sp)
        rc = QRX_MIX_ERR_SP;
    if (rc == QRX_MIX_OK && sig) {
        switch (sig->ret) {
        case 'v':
            memset(&out, 0, sizeof(out));
            break;
        case 'i':
            out.gpr[1] = 0u;
            out.fpr = 0.0;
            break;
        case 'l':
            out.fpr = 0.0;
            break;
        default:                        /* 'f' */
            out.gpr[0] = 0u;
            out.gpr[1] = 0u;
            break;
        }
    }
    if (rc == QRX_MIX_OK && result)
        *result = out;

    if (nested) {
        p->cpu = saved;
        p->status = saved_status;
        p->fault_addr = saved_fault;
        p->insns = saved_insns;
    }
    return rc;
}

qrx_mix_status_t qrx_mix_rd_resolve(qrx_mix_t *mix, uint32_t descriptor,
                                    int isa, qrx_mix_rd_t *out)
{
    qrx_ppc_t *p;
    uint16_t trap;
    uint16_t count;
    uint16_t flags;
    uint8_t byte;
    uint32_t proc_info;
    uint32_t proc_desc;
    uint32_t record = 0u;
    uint32_t entry;
    int found = 0;
    unsigned i;

    if (!mix || !(p = mix->ppc) || !p->mem.data || !out)
        return QRX_MIX_ERR_ARG;
    if (isa != QRX_MIX_RD_ISA_68K && isa != QRX_MIX_RD_ISA_PPC)
        return QRX_MIX_ERR_RD;
    memset(out, 0, sizeof(*out));
    if (descriptor > p->mem.size ||
        p->mem.size - descriptor < QRX_MIX_RD_HEADER)
        return QRX_MIX_ERR_RD;
    if (!mix_read16(mix, descriptor, &trap) || trap != QRX_MIX_RD_TRAP)
        return QRX_MIX_ERR_RD;
    if (!mix_read8(mix, descriptor + 2u, &byte) || byte != QRX_MIX_RD_VERSION)
        return QRX_MIX_ERR_RD;
    /* Dispatched routine descriptors carry selector information. */
    if (!mix_read8(mix, descriptor + 3u, &byte) || byte != 0u)
        return QRX_MIX_ERR_RD;
    if (!mix_read16(mix, descriptor + 10u, &count) ||
        count >= QRX_MIX_RD_MAX_RECORDS)
        return QRX_MIX_ERR_RD;
    if (p->mem.size - descriptor <
        QRX_MIX_RD_HEADER + (uint32_t)(count + 1u) * QRX_MIX_RD_RECORD)
        return QRX_MIX_ERR_RD;
    for (i = 0; i <= (unsigned)count; i++) {
        uint32_t rr = descriptor + QRX_MIX_RD_HEADER +
                      (uint32_t)i * QRX_MIX_RD_RECORD;
        uint8_t this_isa;

        if (!mix_read8(mix, rr + 5u, &this_isa))
            return QRX_MIX_ERR_RD;
        if (!found && (int)this_isa == isa) {
            record = rr;
            found = 1;
        }
    }
    if (!found)
        return QRX_MIX_ERR_RD;
    if (!mix_read16(mix, record + 6u, &flags))
        return QRX_MIX_ERR_RD;
    if (flags & ~(QRX_MIX_RD_FLAG_RELATIVE | QRX_MIX_RD_FLAG_NEEDS_PREP |
                  QRX_MIX_RD_FLAG_NATIVE | QRX_MIX_RD_FLAG_SELECTOR |
                  QRX_MIX_RD_FLAG_INDEXABLE))
        return QRX_MIX_ERR_RD;
    if (flags & (QRX_MIX_RD_FLAG_SELECTOR | QRX_MIX_RD_FLAG_INDEXABLE))
        return QRX_MIX_ERR_RD;
    if (flags & QRX_MIX_RD_FLAG_NEEDS_PREP)
        return QRX_MIX_ERR_RD;      /* no Code Fragment Manager here */
    if (!mix_read32(mix, record, &proc_info) ||
        !mix_read32(mix, record + 8u, &proc_desc))
        return QRX_MIX_ERR_RD;
    if (flags & QRX_MIX_RD_FLAG_RELATIVE) {
        entry = descriptor + proc_desc;
        if (entry < descriptor)
            return QRX_MIX_ERR_RD;
    } else {
        entry = proc_desc;
    }
    if (entry == 0u || (entry & 3u) || entry >= p->mem.size)
        return QRX_MIX_ERR_RD;

    out->descriptor = descriptor;
    out->record = record;
    out->entry = entry;
    out->proc_info = proc_info;
    out->routine_flags = flags;
    out->record_count = (unsigned)count + 1u;
    out->isa = isa;
    return QRX_MIX_OK;
}

/* Decode the register-based ProcInfo of a resolved record into a signature. */
qrx_mix_status_t qrx_mix_rd_procinfo_sig(const qrx_mix_rd_t *rd,
                                         qrx_mix_sig_t *sig)
{
    qrx_mix_sig_t out;
    uint32_t result;
    unsigned i;

    if (!rd || !sig)
        return QRX_MIX_ERR_ARG;
    if ((rd->proc_info & QRX_MIX_PI_CONVENTION_MASK) !=
        QRX_MIX_PI_REGISTER_BASED)
        return QRX_MIX_ERR_RD;
    memset(&out, 0, sizeof(out));
    result = (rd->proc_info & QRX_MIX_PI_RESULT_MASK) >>
             QRX_MIX_PI_RESULT_PHASE;
    /* Size code 0 means no result; 1, 2 and 4 bytes arrive in r3. */
    out.ret = (result == 0u) ? 'v' : 'i';
    for (i = 0; i < QRX_MIX_PI_PARAM_MAX; i++) {
        uint32_t shift = QRX_MIX_PI_PARAM_PHASE + QRX_MIX_PI_PARAM_WIDTH * i;
        uint32_t field = (rd->proc_info >> shift) & 0x1Fu;

        if (field == 0u)
            break;                  /* kNoByteCode pads the unused fields */
        if ((field & QRX_MIX_PI_PARAM_SIZE_MASK) == 0u)
            return QRX_MIX_ERR_RD;  /* register hint without a size */
        out.params[out.nparams++] = 'i';
    }
    /* Set bits above the last parameter field are a fifth parameter, which
     * the record cannot describe, not padding. */
    if ((rd->proc_info >>
         (QRX_MIX_PI_PARAM_PHASE + QRX_MIX_PI_PARAM_WIDTH * out.nparams)) != 0u)
        return QRX_MIX_ERR_RD;
    if (!mix_sig_valid(&out))
        return QRX_MIX_ERR_RD;
    *sig = out;
    return QRX_MIX_OK;
}

qrx_mix_status_t qrx_mix_rd_call(qrx_mix_t *mix, uint32_t descriptor, int isa,
                                 const qrx_mix_sig_t *sig,
                                 const qrx_mix_args_t *args,
                                 qrx_mix_result_t *result, uint32_t budget)
{
    qrx_mix_rd_t info;
    qrx_mix_sig_t derived;
    qrx_mix_status_t rc;

    if (isa != QRX_MIX_RD_ISA_PPC)
        return QRX_MIX_ERR_RD;      /* this core executes PPC code only */
    rc = qrx_mix_rd_resolve(mix, descriptor, isa, &info);
    if (rc != QRX_MIX_OK)
        return rc;
    if (!sig) {
        /* NULL means "take the placement from the record", not "forward the
         * raw registers"; the ISA gate above already ruled out 68k code. */
        rc = qrx_mix_rd_procinfo_sig(&info, &derived);
        if (rc != QRX_MIX_OK)
            return rc;
        sig = &derived;
    }
    return qrx_mix_call(mix, info.entry, sig, args, result, budget);
}

qrx_mix_status_t qrx_mix_rd_invoke(qrx_mix_t *mix, uint32_t descriptor,
                                   const qrx_mix_sig_t *sig,
                                   const qrx_mix_args_t *args,
                                   qrx_mix_result_t *result, uint32_t budget)
{
    qrx_mix_rd_t info;
    qrx_mix_sig_t derived;
    qrx_mix_status_t rc;

    /* A 68k caller cannot name an ISA: it holds the descriptor address only.
     * Select the record this core executes; a descriptor that carries just a
     * 68k record describes the caller side and has no PPC entry to run. */
    rc = qrx_mix_rd_resolve(mix, descriptor, QRX_MIX_RD_ISA_PPC, &info);
    if (rc != QRX_MIX_OK)
        return rc;
    if (!sig) {
        /* No signature: the record's ProcInfo describes the placement.  A
         * ProcInfo that cannot describe it is an error, not a licence to
         * forward the raw registers. */
        rc = qrx_mix_rd_procinfo_sig(&info, &derived);
        if (rc != QRX_MIX_OK)
            return rc;
        sig = &derived;
    }
    return qrx_mix_call(mix, info.entry, sig, args, result, budget);
}

qrx_mix_status_t qrx_mix_rd_invoke_procinfo(qrx_mix_t *mix,
                                            uint32_t descriptor,
                                            const qrx_mix_args_t *args,
                                            qrx_mix_result_t *result,
                                            uint32_t budget)
{
    /* One code path for both spellings. */
    return qrx_mix_rd_invoke(mix, descriptor, 0, args, result, budget);
}
