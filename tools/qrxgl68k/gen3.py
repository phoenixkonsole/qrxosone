#!/usr/bin/env python3
"""Generates the GL 3 door for QRX OS One 64 (68100EC64 host service 13) from the host's glcorearb.h.

    gen3.py [glcorearb.h]   writes guest/qgl3_gen.c, guest/qgl3_ids.h and the engine's full68k/qrx_gl3_gen.inc

The stream is the one of the GL 2.1 door (gen.py: big-endian words, a header per call, scalars, inline
arrays swapped by element size), but the host runs it on an OpenGL 3.3 core context and passes buffers
through: buffer data goes over as bytes (the program brings vertex data in the host's byte order;
WebRender does on QRX OS One 64), vertex attribute and index pointers are offsets into the bound
buffers, pixel data is swapped by its type on the host (full68k/qrx_gl3.c). Framebuffer 0 is a host
framebuffer object of the size the program asks for, which it reads back into its window.

The functions are those of OpenGL 1.0 to 3.3 core. One the rules cannot carry is still exported, but
only logs its name once and records GL_INVALID_OPERATION."""
import re, sys
from pathlib import Path
import gen as g2

HERE = Path(__file__).resolve().parent
ENGINE = HERE.parents[1] / 'upstream/armithlon/2.6/engine/full68k'
FIRST_ID = 16

g2.SCALAR['GLsync'] = (8, 'uint64_t')
g2.GUEST['GLsync'] = 'void *'
g2.SCALAR['GLDEBUGPROC'] = None  # not carried

# Hand-written on the host (qgl3h_<name> in qrx_gl3.c); pointers reach them as 64-bit guest addresses.
HOST_CUSTOM = {
    'glTexImage1D', 'glTexImage2D', 'glTexImage3D', 'glTexSubImage1D', 'glTexSubImage2D', 'glTexSubImage3D',
    'glCompressedTexImage1D', 'glCompressedTexImage2D', 'glCompressedTexImage3D',
    'glCompressedTexSubImage1D', 'glCompressedTexSubImage2D', 'glCompressedTexSubImage3D',
    'glReadPixels', 'glGetTexImage', 'glGetCompressedTexImage',
    'glBindFramebuffer', 'glDrawBuffer', 'glReadBuffer', 'glDrawBuffers',
    'glGetIntegerv', 'glGetInteger64v', 'glGetFloatv', 'glGetDoublev', 'glGetBooleanv', 'glGetIntegeri_v',
    'glGetInteger64i_v', 'glGetBooleani_v',
}
# Implemented by the guest library itself (qgl3.c).
GUEST_CUSTOM = {'glGetString', 'glGetStringi', 'glShaderSource', 'glGetError'}
# Buffer data: bytes, read or written by the host at the time of the call.
RAW = {'glBufferData': {'data': 'raw'}, 'glBufferSubData': {'data': 'raw'}, 'glGetBufferSubData': {'data': 'raw'}}
# Offsets into the bound buffers.
OFFSET = {
    'glVertexAttribPointer': 'pointer', 'glVertexAttribIPointer': 'pointer',
    'glDrawElements': 'indices', 'glDrawElementsInstanced': 'indices', 'glDrawRangeElements': 'indices',
    'glDrawElementsBaseVertex': 'indices', 'glDrawElementsInstancedBaseVertex': 'indices',
    'glDrawRangeElementsBaseVertex': 'indices',
}
SPEC = {
    'glClearBufferfv': {'value': 'in:(buffer == 0x1800 ? 4 : 1)'},
    'glClearBufferiv': {'value': 'in:(buffer == 0x1800 ? 4 : 1)'},
    'glClearBufferuiv': {'value': 'in:(buffer == 0x1800 ? 4 : 1)'},
    'glDeleteSync': {}, 'glGetSynciv': {'length': 'out:1', 'values': 'out:count'},
    'glGetAttachedShaders': {'count': 'out:1', 'shaders': 'out:maxCount'},
    'glGetUniformfv': {'params': 'out:16'}, 'glGetUniformiv': {'params': 'out:16'}, 'glGetUniformuiv': {'params': 'out:16'},
    'glGetQueryObjectiv': {'params': 'out:1'}, 'glGetQueryObjectuiv': {'params': 'out:1'},
    'glGetQueryObjecti64v': {'params': 'out:1'}, 'glGetQueryObjectui64v': {'params': 'out:1'},
    'glGetQueryiv': {'params': 'out:1'},
    'glSamplerParameterfv': {'param': 'in:PN(pname)'}, 'glSamplerParameteriv': {'param': 'in:PN(pname)'},
    'glSamplerParameterIiv': {'param': 'in:PN(pname)'}, 'glSamplerParameterIuiv': {'param': 'in:PN(pname)'},
    'glVertexAttribI1iv': {'v': 'in:1'}, 'glVertexAttribI2iv': {'v': 'in:2'}, 'glVertexAttribI3iv': {'v': 'in:3'},
    'glVertexAttribI4iv': {'v': 'in:4'}, 'glVertexAttribI1uiv': {'v': 'in:1'}, 'glVertexAttribI2uiv': {'v': 'in:2'},
    'glVertexAttribI3uiv': {'v': 'in:3'}, 'glVertexAttribI4uiv': {'v': 'in:4'},
    'glVertexAttribI4bv': {'v': 'in:4'}, 'glVertexAttribI4sv': {'v': 'in:4'}, 'glVertexAttribI4ubv': {'v': 'in:4'},
    'glVertexAttribI4usv': {'v': 'in:4'},
    'glMultiDrawArrays': {'first': 'in:drawcount', 'count': 'in:drawcount'},
}
# Left out: mapped buffers (the program's memory is not the host's), debug callbacks, client pointers.
EXCLUDE = re.compile(r'^gl(MapBuffer|MapBufferRange|UnmapBuffer|FlushMappedBufferRange|GetBufferPointerv'
                     r'|GetVertexAttribPointerv|GetPointerv|DebugMessageCallback|MultiDrawElements'
                     r'|GetUniformIndices|TransformFeedbackVaryings|GetSeparableFilter)')


def parse_corearb(path):
    """The functions of GL_VERSION_1_0 .. GL_VERSION_3_3 in glcorearb.h, as gen.py's json entries."""
    s = Path(path).read_text()
    out, cur = [], None
    for line in s.split('\n'):
        m = re.match(r'#ifndef (GL_VERSION_(\d)_(\d))$', line)
        if m:
            cur = (int(m.group(2)), int(m.group(3)))
            continue
        if line.startswith('#endif /* GL_VERSION'):
            cur = None
        m = re.match(r'GLAPI (.+?)\s*APIENTRY (gl\w+) \((.*)\);', line)
        if not m or not cur or cur > (3, 3):
            continue
        ret, name, args = m.group(1).strip(), m.group(2), m.group(3).strip()
        params = []
        if args and args != 'void':
            for a in args.split(','):
                a = a.strip()
                pm = re.match(r'(.*?)([A-Za-z_]\w*)$', a)
                t = re.sub(r'\s+', ' ', pm.group(1)).strip().replace(' *', '*').replace('*', ' *').replace('  ', ' ').strip()
                t = t.replace(' * ', ' *').replace('* ', '*')
                params.append([t, pm.group(2)])
        out.append({'name': name, 'ret': ret, 'params': params, 'gl11': cur <= (1, 1)})
    return out


def known(t):
    b = g2.base(t)
    return b in g2.SCALAR and g2.SCALAR[b] is not None


def rule(f):
    n, ps = f['name'], f['params']
    if EXCLUDE.search(n):
        return None
    if any(t.count('*') > 1 for t, _ in ps):
        return None if n not in GUEST_CUSTOM else {}
    if n in GUEST_CUSTOM:
        return {}
    ptrs = [(t, p) for t, p in ps if '*' in t]
    if n in HOST_CUSTOM:
        return {p: 'addr' for t, p in ptrs}
    if n in RAW:
        return dict(RAW[n])
    if n in OFFSET:
        return {OFFSET[n]: 'val'}
    if n in SPEC:
        spec = dict(SPEC[n])
        for t, p in ptrs:
            if p not in spec:
                return None
        return spec
    # the GL 2.1 door's rules for the rest (uniforms, vectors, glGen/glDelete, glGet with a pname, names)
    saved = (g2.HOST_CUSTOM, g2.EXCLUDE, g2.GUEST_CUSTOM)
    g2.HOST_CUSTOM, g2.EXCLUDE, g2.GUEST_CUSTOM = set(), re.compile(r'^$x'), set()
    try:
        spec = g2.rule(f)
    finally:
        g2.HOST_CUSTOM, g2.EXCLUDE, g2.GUEST_CUSTOM = saved
    if spec is not None and any(s == 'pix' for s in spec.values()):
        return None
    return spec


def main():
    funcs = parse_corearb(sys.argv[1] if len(sys.argv) > 1 else '/usr/include/GL/glcorearb.h')
    out, nid = [], FIRST_ID
    for f in funcs:
        if not all(known(t) for t, _ in f['params']) or not known(f['ret']):
            f['spec'] = None
        else:
            f['spec'] = rule(f)
        f['id'] = nid
        nid += 1
        out.append(f)
    funcs = out
    assert nid < 4096

    def ret_kind(f):
        r = f['ret'].replace(' ', '')
        if r == 'void':
            return None
        if '*' in r or g2.base(f['ret']) == 'GLsync':
            return 'ptr'
        return g2.base(f['ret'])

    def is_sync(f):
        if ret_kind(f) or f['name'] in HOST_CUSTOM:
            return True
        return any(not s.startswith(('in:', 'val')) for s in f['spec'].values())

    def gtype(t):
        return 'void *' if g2.base(t) == 'GLsync' and '*' not in t else g2.guest_type(t)

    # ---- guest
    gl = ['/* Generated by tools/qrxgl68k/gen3.py, do not edit. */', '#include "qgl3.h"', '']
    hdr = ['/* Generated by tools/qrxgl68k/gen3.py, do not edit. */', '#ifndef QGL3_IDS_H', '#define QGL3_IDS_H']
    for f in funcs:
        hdr.append(f'#define QGL3_ID_{f["name"]} {f["id"]}')
    hdr.append(f'#define QGL3_NFUNCS {nid}')
    hdr.append(g2.PN_C)
    hdr.append('#endif')
    for f in funcs:
        n, ps = f['name'], f['params']
        rt = gtype(f['ret']) if ret_kind(f) else 'void'
        args = ', '.join(f'{gtype(t)} {p}' for t, p in ps) or 'void'
        if f['spec'] is None:
            gl.append(f'static {rt} q3_{n}({args}) {{ qgl3_unsupported("{n}");' + (' return 0; }' if rt != 'void' else ' }'))
            continue
        if n in GUEST_CUSTOM:
            gl.append(f'{rt} q3_{n}({args});')
            continue
        spec = f['spec']
        fixed = sum(g2.words_of(t) if g2.base(t) != 'GLsync' else 2 for t, _ in ps)
        body = []
        inl = [(t, p, spec[p][3:]) for t, p in ps if spec.get(p, '').startswith('in:')]
        for i, (t, p, cnt) in enumerate(inl):
            body.append(f'unsigned _b{i} = {p} ? (unsigned)({cnt}) * {g2.esize(t)}u : 0xFFFFFFFFu;')
        extra = ' + '.join(f'1 + QGL3_W(_b{i})' for i in range(len(inl)))
        body.append(f'unsigned _nw = {fixed}' + (f' + {extra}' if extra else '') + ';')
        body.append('uint32_t *_q = qgl3_put(_nw + 1);')
        body.append(f'*_q++ = ((uint32_t){f["id"]} << 20) | _nw;')
        for t, p in ps:
            b = g2.base(t)
            if '*' in t or b == 'GLsync':
                body.append(f'_q = qgl3_64(_q, (unsigned long long)(unsigned long){p});')
            elif b in ('GLfloat', 'GLclampf'):
                body.append(f'*_q++ = qgl3_f2u({p});')
            elif b in ('GLdouble', 'GLclampd'):
                body.append(f'_q = qgl3_d(_q, {p});')
            elif abs(g2.SCALAR[b][0]) == 8:
                body.append(f'_q = qgl3_64(_q, (unsigned long long){p});')
            else:
                body.append(f'*_q++ = (uint32_t){p};')
        for i, (t, p, cnt) in enumerate(inl):
            body.append(f'_q = qgl3_inline(_q, {p}, _b{i});')
        rk = ret_kind(f)
        if rk == 'ptr':
            body.append(f'return ({rt})(unsigned long)qgl3_sync();')
        elif rk:
            body.append(f'return ({rt})qgl3_sync();')
        elif is_sync(f):
            body.append('qgl3_sync();')
        gl.append(f'static {rt} q3_{n}({args}) {{\n    ' + '\n    '.join(body) + '\n}')
    gl.append('const struct qgl3_name qgl3_names[] = {')
    for f in sorted(funcs, key=lambda f: f['name']):
        gl.append(f'    {{"{f["name"]}", (void *)q3_{f["name"]}}},')
    gl.append('};')
    gl.append(f'const unsigned qgl3_nnames = {len(funcs)};')
    (HERE / 'guest/qgl3_gen.c').write_text('\n'.join(gl) + '\n')
    (HERE / 'guest/qgl3_ids.h').write_text('\n'.join(hdr) + '\n')

    # ---- host
    supported = [f for f in funcs if f['spec'] is not None]
    h = ['/* Generated by tools/qrxgl68k/gen3.py, do not edit. Included by qrx_gl3.c. */', g2.PN_C]

    def htype(t):
        return 'uint64_t' if g2.base(t) == 'GLsync' and '*' not in t else g2.host_type(t)
    for f in supported:
        n, ps = f['name'], f['params']
        rt = htype(f['ret']) if ret_kind(f) and ret_kind(f) != 'ptr' else ('uint64_t' if ret_kind(f) == 'ptr' else 'void')
        if n in GUEST_CUSTOM:
            continue
        if n in HOST_CUSTOM:
            hargs = [('uint64_t ' + p) if '*' in t else f'{htype(t)} {p}' for t, p in ps]
            h.append(f'static {rt} qgl3h_{n}(QGL3 *g{"".join(", " + a for a in hargs)});')
        else:
            pt = ', '.join(('const void *' if f['spec'].get(p) == 'val' else htype(t)) for t, p in ps) or 'void'
            h.append(f'static {rt} (*p_{n})({pt});')
    h.append('static void qgl3_resolve(void) {')
    for f in supported:
        if f['name'] not in HOST_CUSTOM and f['name'] not in GUEST_CUSTOM:
            h.append(f'    p_{f["name"]} = QGL3_PROC("{f["name"]}");')
    h.append('}')
    h.append('static void qgl3_dispatch(QGL3 *g, unsigned id, const uint8_t *a, const uint8_t *end) {')
    h.append('    switch (id) {')
    for f in supported:
        n, ps, spec = f['name'], f['params'], f['spec']
        if n in GUEST_CUSTOM:
            continue
        lines, call, post, off = [], [], [], 0
        for t, p in ps:
            b = g2.base(t)
            if '*' in t or b == 'GLsync':
                lines.append(f'uint64_t ga_{p} = R64(a + {off});'); off += 8
            elif b in ('GLfloat', 'GLclampf'):
                lines.append(f'float v_{p} = RF(a + {off});'); off += 4
            elif b in ('GLdouble', 'GLclampd'):
                lines.append(f'double v_{p} = RD(a + {off});'); off += 8
            elif abs(g2.SCALAR[b][0]) == 8:
                lines.append(f'{g2.SCALAR[b][1]} v_{p} = ({g2.SCALAR[b][1]})R64(a + {off});'); off += 8
            else:
                lines.append(f'{g2.SCALAR[b][1]} v_{p} = ({g2.SCALAR[b][1]})R32(a + {off});'); off += 4
        lines.append(f'const uint8_t *in = a + {off};')
        for t, p in ps:
            b = g2.base(t)
            if b == 'GLsync' and '*' not in t:
                call.append(f'ga_{p}')
                continue
            if '*' not in t:
                call.append('v_' + p)
                continue
            s = spec.get(p, '')
            es = g2.esize(t)
            ht = g2.host_type(t).replace('const ', '')
            if n in HOST_CUSTOM or s == 'addr':
                call.append(f'ga_{p}')
            elif s.startswith('in:'):
                lines.append(f'{ht} v_{p} = ({ht})qgl3_take_in(g, &in, end, {es});')
                call.append('v_' + p)
            elif s.startswith('out:'):
                expr = re.sub(r'\b(' + '|'.join(q for _, q in ps) + r')\b', r'v_\1', s[4:])
                lines.append(f'size_t n_{p} = (size_t)({expr});')
                lines.append(f'{ht} v_{p} = ({ht})qgl3_out(g, ga_{p}, n_{p}, {es});')
                post.append(f'qgl3_out_done(g, ga_{p}, v_{p}, n_{p}, {es});')
                call.append('v_' + p)
            elif s == 'raw':
                lines.append(f'{ht} v_{p} = ({ht})qgl3_raw(g, ga_{p});')
                call.append('v_' + p)
            elif s == 'val':
                call.append(f'(const void *)(uintptr_t)ga_{p}')
            else:
                raise SystemExit(f'{n}: no spec for {p}')
        fn = f'qgl3h_{n}(g{"".join(", " + c for c in call)})' if n in HOST_CUSTOM else f'p_{n}({", ".join(call)})'
        rk = ret_kind(f)
        if rk == 'ptr':
            lines.append(f'g->ret = (uint64_t)(uintptr_t){fn};')
        elif rk == 'GLfloat':
            lines.append(f'{{ float r = {fn}; uint32_t u; memcpy(&u, &r, 4); g->ret = u; }}')
        elif rk:
            lines.append(f'g->ret = (uint64_t)(int64_t){fn};')
        else:
            lines.append(f'{fn};')
        lines += post
        lines.append('(void)in; break;')
        h.append(f'    case {f["id"]}: {{ /* {n} */ ' + ' '.join(lines) + ' }')
    h.append('    default: qgl3_bad_id(g, id); break;')
    h.append('    }')
    h.append('}')
    h.append('static const char *const qgl3_id_names[] = {')
    for f in funcs:
        h.append(f'    [{f["id"]}] = "{f["name"]}",')
    h.append('};')
    h.append(f'#define QGL3_NFUNCS {nid}')
    for n in sorted(GUEST_CUSTOM):
        f = next((x for x in funcs if x['name'] == n), None)
        if f:
            h.append(f'#define QGL3_ID_{n} {f["id"]}')
    (ENGINE / 'qrx_gl3_gen.inc').write_text('\n'.join(h) + '\n')
    unsup = [f['name'] for f in funcs if f['spec'] is None]
    print(f'{len(funcs)} functions, {len(supported)} carried, {len(unsup)} not: {" ".join(unsup)}', file=sys.stderr)


if __name__ == '__main__':
    main()
