#!/usr/bin/env python3
"""Generates both halves of the GL door for 68k programs from gl_functions.json.

    gen.py            writes guest/qgl_gen.c, guest/qgl_ids.h and the engine's full68k/qrx_gl68k_gen.inc

The guest appends each call to a command stream in its own memory; the engine runs the stream on a
real OpenGL 2.1 context of the Mac (full68k/qrx_gl68k.c). Every word in the stream is big-endian.

    header  (id << 20) | words that follow
    scalar  one word; 64-bit values (double, GLint64, pointers, GLintptr) take two, high word first
    inline  one word with the byte count (0xFFFFFFFF for a null pointer), then the bytes as the guest
            has them, padded to a word. The engine swaps them by element size.

Guest addresses travel as 64-bit values, so a 64-bit guest needs no new protocol.

A call returns without waiting unless it returns a value or the engine has to read or write guest memory
at the time of the call. Pointer parameters follow RULES below or SPEC; a function the rules do not
cover is still exported, but only logs its name when called (core functions, so GLEW sees every
version up to 2.1) or is left out (extensions, so GLEW reports them missing)."""
import json, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENGINE = HERE.parents[1] / 'upstream/armithlon/2.6/engine/full68k'
FUNCS = json.loads((HERE / 'gl_functions.json').read_text())
FIRST_ID = 16  # 1..15 are the door's own requests (contexts, swap, strings)

# Size in bytes and host C type for each scalar type.
SCALAR = {
    'GLenum': (4, 'uint32_t'), 'GLuint': (4, 'uint32_t'), 'GLbitfield': (4, 'uint32_t'), 'GLint': (4, 'int32_t'),
    'GLsizei': (4, 'int32_t'), 'GLfloat': (4, 'float'), 'GLclampf': (4, 'float'), 'GLboolean': (1, 'uint8_t'),
    'GLubyte': (1, 'uint8_t'), 'GLbyte': (1, 'int8_t'), 'GLchar': (1, 'char'), 'GLcharARB': (1, 'char'),
    'GLshort': (2, 'int16_t'), 'GLushort': (2, 'uint16_t'), 'GLhalf': (2, 'uint16_t'),
    'GLdouble': (8, 'double'), 'GLclampd': (8, 'double'),
    'GLint64': (8, 'int64_t'), 'GLuint64': (8, 'uint64_t'), 'GLint64EXT': (8, 'int64_t'), 'GLuint64EXT': (8, 'uint64_t'),
    'GLintptr': (-8, 'intptr_t'), 'GLsizeiptr': (-8, 'intptr_t'), 'GLintptrARB': (-8, 'intptr_t'), 'GLsizeiptrARB': (-8, 'intptr_t'),
    'GLhandleARB': (-4, 'void *'),
    'void': (1, 'void'),
}
# Guest C types (68k, ILP32).
GUEST = {'GLenum': 'unsigned int', 'GLuint': 'unsigned int', 'GLbitfield': 'unsigned int', 'GLint': 'int', 'GLsizei': 'int',
         'GLfloat': 'float', 'GLclampf': 'float', 'GLboolean': 'unsigned char', 'GLubyte': 'unsigned char', 'GLbyte': 'signed char',
         'GLchar': 'char', 'GLcharARB': 'char', 'GLshort': 'short', 'GLushort': 'unsigned short', 'GLhalf': 'unsigned short',
         'GLdouble': 'double', 'GLclampd': 'double', 'GLint64': 'long long', 'GLuint64': 'unsigned long long',
         'GLint64EXT': 'long long', 'GLuint64EXT': 'unsigned long long', 'GLintptr': 'long', 'GLsizeiptr': 'long',
         'GLintptrARB': 'long', 'GLsizeiptrARB': 'long', 'GLhandleARB': 'unsigned int', 'void': 'void'}

# Functions the engine implements by hand in qrx_gl68k.c (qglh_<name>): buffer objects live on the host in
# the guest's byte order and vertex arrays are converted at draw time, the window's framebuffer is an FBO.
# Pointer parameters of these reach the handler as 64-bit guest addresses unless the entry says otherwise.
HOST_CUSTOM = {
    'glBindBuffer', 'glBindBufferARB', 'glGenBuffers', 'glGenBuffersARB', 'glDeleteBuffers', 'glDeleteBuffersARB',
    'glIsBuffer', 'glIsBufferARB', 'glBufferData', 'glBufferDataARB', 'glBufferSubData', 'glBufferSubDataARB',
    'glGetBufferSubData', 'glGetBufferSubDataARB', 'glGetBufferParameteriv', 'glGetBufferParameterivARB',
    'glVertexPointer', 'glNormalPointer', 'glColorPointer', 'glSecondaryColorPointer', 'glFogCoordPointer',
    'glTexCoordPointer', 'glIndexPointer', 'glEdgeFlagPointer', 'glVertexAttribPointer', 'glVertexAttribPointerARB',
    'glSecondaryColorPointerEXT', 'glFogCoordPointerEXT', 'glInterleavedArrays',
    'glEnableClientState', 'glDisableClientState', 'glClientActiveTexture', 'glClientActiveTextureARB',
    'glEnableVertexAttribArray', 'glDisableVertexAttribArray', 'glEnableVertexAttribArrayARB', 'glDisableVertexAttribArrayARB',
    'glPushClientAttrib', 'glPopClientAttrib',
    'glDrawArrays', 'glDrawElements', 'glDrawRangeElements', 'glDrawRangeElementsEXT', 'glArrayElement',
    'glMultiDrawArrays',
    'glBindFramebuffer', 'glBindFramebufferEXT', 'glDrawBuffer', 'glReadBuffer', 'glDrawBuffers', 'glDrawBuffersARB',
    'glGetIntegerv', 'glGetFloatv', 'glGetDoublev', 'glGetBooleanv', 'glPixelStorei', 'glPixelStoref',
    'glSelectBuffer', 'glFeedbackBuffer', 'glRenderMode', 'glCallLists', 'glGetVertexAttribiv', 'glGetVertexAttribivARB',
}
# Pointer setters only record the address; these HOST_CUSTOM functions get it without waiting.
POINTER_SETTERS = {'glVertexPointer', 'glNormalPointer', 'glColorPointer', 'glSecondaryColorPointer', 'glFogCoordPointer',
                   'glTexCoordPointer', 'glIndexPointer', 'glEdgeFlagPointer', 'glVertexAttribPointer', 'glVertexAttribPointerARB',
                   'glSecondaryColorPointerEXT', 'glFogCoordPointerEXT', 'glInterleavedArrays'}
# These read client arrays in guest memory, which the program may change as soon as the call returns.
SYNC = {'glDrawArrays', 'glArrayElement', 'glMultiDrawArrays'}
# Left out: vertex array objects and ranges, mapped ranges, instancing, uniform buffers. They would need more
# of the buffer emulation; GLEW reports their extensions missing and Blender 2.79 does without.
EXCLUDE = re.compile(r'VertexArray|ElementArray|BufferRange|BufferBase|Instanced|AttribDivisor|TextureRangeAPPLE'
                     r'|ObjectPurgeable|BaseVertex|BufferParameteriAPPLE|FlushMappedBufferRange|^glMultiDrawElements')
# Functions the guest library implements itself (qgl.c); they still get an id for its requests.
GUEST_CUSTOM = {'glGetString', 'glShaderSource', 'glShaderSourceARB', 'glMapBuffer', 'glMapBufferARB', 'glGetPointerv',
                'glGetBufferPointerv', 'glGetBufferPointervARB', 'glGetVertexAttribPointerv', 'glGetVertexAttribPointervARB',
                'glUnmapBuffer', 'glUnmapBufferARB'}
# Pointer parameters by hand: 'in:N' inline N elements, 'out:N' written back after the call, 'raw' the engine
# reads or writes guest memory directly (bytes, no swap), 'pix' pixel data (unpack/pack buffer aware), 'addr' a
# 64-bit guest address for a HOST_CUSTOM handler. N is a C expression over the parameters; PN(pname) is the
# number of values for a state name.
SPEC = {
    'glRectdv': {'v1': 'in:2', 'v2': 'in:2'}, 'glRectfv': {'v1': 'in:2', 'v2': 'in:2'},
    'glRectiv': {'v1': 'in:2', 'v2': 'in:2'}, 'glRectsv': {'v1': 'in:2', 'v2': 'in:2'},
    'glClipPlane': {'equation': 'in:4'}, 'glGetClipPlane': {'equation': 'out:4'},
    'glAreTexturesResident': {'textures': 'in:n', 'residences': 'out:n'},
    'glPrioritizeTextures': {'textures': 'in:n', 'priorities': 'in:n'},
    'glGetAttachedShaders': {'count': 'out:1', 'shaders': 'out:maxCount'},
    'glGetUniformfv': {'params': 'out:16'}, 'glGetUniformiv': {'params': 'out:16'},
    'glGetUniformfvARB': {'params': 'out:16'}, 'glGetUniformivARB': {'params': 'out:16'},
    'glGetQueryObjectiv': {'params': 'out:1'}, 'glGetQueryObjectuiv': {'params': 'out:1'},
    'glGetQueryObjecti64vEXT': {'params': 'out:1'}, 'glGetQueryObjectui64vEXT': {'params': 'out:1'},
    'glGetQueryiv': {'params': 'out:1'},
    'glDrawBuffers': {'bufs': 'in:n'}, 'glDrawBuffersARB': {'bufs': 'in:n'},
    'glMultiDrawArrays': {'first': 'in:drawcount', 'count': 'in:drawcount'},
    'glReadPixels': {'pixels': 'pix'},
    'glGetIntegerv': {'params': 'addr'}, 'glGetFloatv': {'params': 'addr'}, 'glGetDoublev': {'params': 'addr'},
    'glGetBooleanv': {'params': 'addr'},
    'glMultiDrawArraysEXT': {'first': 'in:primcount', 'count': 'in:primcount'},
    'glPolygonStipple': {'mask': 'pix'}, 'glGetPolygonStipple': {'mask': 'pix'}, 'glBitmap': {'bitmap': 'pix'},
    'glPixelMapfv': {'values': 'in:mapsize'}, 'glPixelMapuiv': {'values': 'in:mapsize'}, 'glPixelMapusv': {'values': 'in:mapsize'},
    'glGetPixelMapfv': {'values': 'out:PN(map)'}, 'glGetPixelMapuiv': {'values': 'out:PN(map)'}, 'glGetPixelMapusv': {'values': 'out:PN(map)'},
}
CORE_PIX = re.compile(r'^gl(Compressed)?Tex(Sub)?Image[123]D(ARB|EXT)?$|^glDrawPixels$|^glGetTexImage$|^glGetCompressedTexImage(ARB)?$'
                      r'|^gl(Get)?(ColorTable|ColorSubTable|ConvolutionFilter[12]D|SeparableFilter2D|Histogram|Minmax)(EXT)?$')
VEC = re.compile(r'^gl(Vertex|Color|SecondaryColor|Normal|TexCoord|MultiTexCoord|RasterPos|WindowPos|VertexAttrib|EvalCoord'
                 r'|Index|FogCoord|EdgeFlag)(\d)?N?(b|s|i|f|d|ub|us|ui)?v(ARB|EXT|NV|APPLE|MESA)?$')
PNAME_IN = re.compile(r'^gl(TexParameter|TexEnv|TexGen|Light|LightModel|Material|Fog|PointParameter|ColorTableParameter'
                      r'|ConvolutionParameter|TexParameterI)(f|i|d|I|Ii|Iui)?v(ARB|EXT|APPLE)?$')
MATRIX = re.compile(r'^gl(Load|Mult)(Transpose)?Matrix(f|d)(ARB)?$')
UNIFORM = re.compile(r'^glUniform(\d)(f|i|ui)v(ARB|EXT)?$')
UMATRIX = re.compile(r'^glUniformMatrix(\d)(x(\d))?fv(ARB)?$')
# Number of values for a state name in glGet*v and the pname setters; everything else has one.
PN = {4: [0x0C22, 0x0BA2, 0x0C10, 0x0C23, 0x0B00, 0x0B03, 0x0B07, 0x0B04, 0x0B06, 0x0B80, 0x8005, 0x0B66, 0x0B53, 0x1200,
          0x1201, 0x1202, 0x1203, 0x1600, 0x1602, 0x2201, 0x1004, 0x2501, 0x2502, 0x8459, 0x8626, 0x0DD3],
      3: [0x1603, 0x1204, 0x0B02, 0x8129],
      2: [0x0B70, 0x0B22, 0x0B12, 0x0D3A, 0x0B40, 0x846D, 0x846E, 0x0DD0, 0x0DD2],
      16: [0x0BA6, 0x0BA7, 0x0BA8, 0x80B1, 0x84E3, 0x84E4, 0x84E5, 0x84E6]}
PN_C = ('static inline unsigned qgl_pn(unsigned pname) {\n    switch (pname) {\n'
        + ''.join(f'    {" ".join(f"case 0x{v:04X}:" for v in vs)} return {k};\n' for k, vs in PN.items())
        + '    default: return 1;\n    }\n}\n#define PN(p) qgl_pn(p)')
SUFFIX = re.compile(r'(ARB|EXT|APPLE|NV|ATI|SGIS|SGIX|IBM|MESA|AMD|INTEL|KHR|OES|SUN|HP|3DFX)$')


def base(t):
    return t.replace('const', '').replace('*', '').strip()


def ptr_depth(t):
    return t.count('*')


def rule(f):
    """Spec for each pointer parameter, or None when the function is not supported."""
    n, ps = f['name'], f['params']
    ptrs = [(t, p) for t, p in ps if '*' in t]
    if EXCLUDE.search(n):
        return None
    if n in GUEST_CUSTOM and n not in HOST_CUSTOM:
        return {}
    spec = dict(SPEC.get(n, {}))
    if n in HOST_CUSTOM:
        for t, p in ptrs:
            spec.setdefault(p, 'val' if n in POINTER_SETTERS else 'addr')
        return spec
    if not ptrs:
        return spec
    m = VEC.match(n)
    if m and len(ptrs) == 1 and not spec:
        k = m.group(2) or '1'
        return {ptrs[0][1]: f'in:{k}'}
    m = UNIFORM.match(n)
    if m:
        return {ptrs[0][1]: f'in:count*{m.group(1)}'}
    m = UMATRIX.match(n)
    if m:
        return {ptrs[0][1]: f'in:count*{int(m.group(1)) * int(m.group(3) or m.group(1))}'}
    if MATRIX.match(n):
        return {ptrs[0][1]: 'in:16'}
    if PNAME_IN.match(n) and len(ptrs) == 1 and 'const' in ptrs[0][0]:
        return {ptrs[0][1]: 'in:PN(pname)'}
    if CORE_PIX.match(n):
        return {p: 'pix' for t, p in ptrs}
    if re.match(r'^glGen\w+$', n) and len(ptrs) == 1 and ps[0][0] == 'GLsizei':
        return {ptrs[0][1]: f'out:{ps[0][1]}'}
    if re.match(r'^glDelete\w+$', n) and len(ptrs) == 1 and ps[0][0] == 'GLsizei':
        return {ptrs[0][1]: f'in:{ps[0][1]}'}
    for t, p in ptrs:
        if p in spec:
            continue
        b = base(t)
        if ptr_depth(t) != 1:
            return None
        if b in ('GLchar', 'GLcharARB'):
            spec[p] = 'raw'
        elif n.startswith('glGet') and 'const' not in t and p in ('length', 'size', 'type', 'count'):
            spec[p] = 'out:1'
        elif n.startswith('glGet') and 'const' not in t and any(q == 'pname' for _, q in ps) and t == ptrs[-1][0] and p == ptrs[-1][1]:
            spec[p] = 'out:PN(pname)'
        else:
            return None
    return spec


def words_of(t):
    if '*' in t:
        return 2
    s = SCALAR[base(t)][0]
    return 2 if abs(s) == 8 else 1


def host_type(t):
    if '*' in t:
        return ('const ' if t.startswith('const') else '') + SCALAR[base(t)][1] + ' *' * ptr_depth(t)
    return SCALAR[base(t)][1]


def guest_type(t):
    if '*' in t:
        return ('const ' if t.startswith('const') else '') + GUEST[base(t)] + ' *' * ptr_depth(t)
    return GUEST[base(t)]


def esize(t):
    b = base(t)
    if b == 'void':
        return 1
    return abs(SCALAR[b][0])


def main():
    funcs, ids = [], {}
    nid = FIRST_ID
    for f in FUNCS:
        if any(base(t) not in SCALAR for t, _ in f['params']) or (base(f['ret']) not in SCALAR):
            continue
        if base(f['ret']) == 'GLsync' or any('GLsync' in t for t, _ in f['params']):
            continue
        spec = rule(f)
        f['spec'] = spec
        f['id'] = nid
        ids[f['name']] = nid
        nid += 1
        funcs.append(f)
    assert nid < 4096

    def ret_kind(f):
        r = f['ret'].replace(' ', '')
        if r == 'void':
            return None
        if '*' in r:
            return 'ptr'
        return base(f['ret'])

    def is_sync(f):
        if f['spec'] is None:
            return False
        if ret_kind(f) or f['name'] in SYNC:
            return True
        return any(not s.startswith(('in:', 'val')) for s in f['spec'].values())

    supported = [f for f in funcs if f['spec'] is not None]
    # ---- guest
    g = ['/* Generated by tools/qrxgl68k/gen.py, do not edit. */', '#include "qgl.h"', '']
    hdr = ['/* Generated by tools/qrxgl68k/gen.py, do not edit. */', '#ifndef QGL_IDS_H', '#define QGL_IDS_H']
    for f in funcs:
        hdr.append(f'#define QGL_ID_{f["name"]} {f["id"]}')
    hdr.append(f'#define QGL_NFUNCS {nid}')
    hdr.append(PN_C)
    hdr.append('#endif')
    names = []
    for f in funcs:
        if f['spec'] is not None or not SUFFIX.search(f['name']):
            args = ', '.join(f'{guest_type(t)} {p}' for t, p in f['params']) or 'void'
            g.append(f'{guest_type(f["ret"]) if ret_kind(f) else "void"} qgl_{f["name"]}({args});')
    for f in funcs:
        n, ps = f['name'], f['params']
        args = ', '.join(f'{guest_type(t)} {p}' for t, p in ps) or 'void'
        rt = guest_type(f['ret']) if ret_kind(f) else 'void'
        core = not SUFFIX.search(n)
        if f['spec'] is None:
            if not core:
                continue
            g.append(f'{rt} qgl_{n}({args}) {{ qgl_unsupported("{n}");' + (' return 0; }' if rt != 'void' else ' }'))
            names.append(n)
            continue
        names.append(n)
        if n in GUEST_CUSTOM:
            continue
        spec = f['spec']
        fixed = sum(words_of(t) for t, _ in ps)
        body = []
        inl = [(t, p, spec[p][3:]) for t, p in ps if spec.get(p, '').startswith('in:')]
        for i, (t, p, cnt) in enumerate(inl):
            body.append(f'unsigned _b{i} = {p} ? (unsigned)({cnt}) * {esize(t)}u : 0xFFFFFFFFu;')
        extra = ' + '.join(f'1 + QGL_W(_b{i})' for i in range(len(inl)))
        body.append(f'unsigned _nw = {fixed}' + (f' + {extra}' if extra else '') + ';')
        body.append('uint32_t *_q = qgl_put(_nw + 1);')
        body.append(f'*_q++ = ((uint32_t){f["id"]} << 20) | _nw;')
        for t, p in ps:
            if '*' in t:
                body.append(f'_q = qgl_ptr(_q, {p});')
            else:
                b = base(t)
                s = SCALAR[b][0]
                if b in ('GLfloat', 'GLclampf'):
                    body.append(f'*_q++ = qgl_f2u({p});')
                elif b in ('GLdouble', 'GLclampd'):
                    body.append(f'_q = qgl_d(_q, {p});')
                elif abs(s) == 8 and s > 0:
                    body.append(f'_q = qgl_64(_q, (unsigned long long){p});')
                elif s == -8:
                    body.append(f'_q = qgl_64(_q, (unsigned long long)(long long){p});')
                else:
                    body.append(f'*_q++ = (uint32_t){p};')
        for i, (t, p, cnt) in enumerate(inl):
            body.append(f'_q = qgl_inline(_q, {p}, _b{i});')
        rk = ret_kind(f)
        if rk:
            if rk == 'ptr':
                body.append(f'return ({rt})(unsigned long)qgl_sync();')
            else:
                body.append(f'return ({rt})qgl_sync();')
        elif is_sync(f):
            body.append('qgl_sync();')
        g.append(f'{rt} qgl_{n}({args}) {{\n    ' + '\n    '.join(body) + '\n}')
    # GL 1.1 functions are linked by name, everything else is found through qgl_GetProcAddress.
    gl11 = {f['name'] for f in FUNCS if f['gl11']}
    for n in names:
        if n in gl11 and n not in GUEST_CUSTOM:
            g.append(f'extern __typeof__(qgl_{n}) {n} __attribute__((alias("qgl_{n}")));')
    g.append('const struct qgl_name qgl_names[] = {')
    for n in sorted(names):
        g.append(f'    {{"{n}", (void *)qgl_{n}}},')
    g.append('};')
    g.append(f'const unsigned qgl_nnames = {len(names)};')
    (HERE / 'guest/qgl_gen.c').write_text('\n'.join(g) + '\n')
    (HERE / 'guest/qgl_ids.h').write_text('\n'.join(hdr) + '\n')

    # ---- host
    h = ['/* Generated by tools/qrxgl68k/gen.py, do not edit. Included by qrx_gl68k.c. */', PN_C]
    for f in supported:
        n, ps = f['name'], f['params']
        rt = host_type(f['ret']) if ret_kind(f) else 'void'
        if n in HOST_CUSTOM:
            hargs = []
            for t, p in ps:
                s = f['spec'].get(p)
                if s in ('addr', 'val'):
                    hargs.append(f'uint64_t {p}')
                elif s and s.startswith('in:'):
                    hargs.append(f'{host_type(t)} {p}')
                else:
                    hargs.append(f'{host_type(t)} {p}')
            h.append(f'static {rt} qglh_{n}(QGL *g{"".join(", " + a for a in hargs)});')
        else:
            h.append(f'static {rt} (*p_{n})({", ".join(host_type(t) for t, _ in ps) or "void"});')
    h.append('static void qgl_resolve(void) {')
    for f in supported:
        if f['name'] not in HOST_CUSTOM:
            h.append(f'    p_{f["name"]} = dlsym(RTLD_DEFAULT, "{f["name"]}");')
    h.append('}')
    h.append('static void qgl_dispatch(QGL *g, unsigned id, const uint8_t *a, const uint8_t *end) {')
    h.append('    switch (id) {')
    for f in supported:
        n, ps, spec = f['name'], f['params'], f['spec']
        if n in GUEST_CUSTOM and n not in HOST_CUSTOM:
            continue
        lines, call, post = [], [], []
        off = 0
        for t, p in ps:
            s = spec.get(p)
            b = base(t)
            if '*' in t:
                lines.append(f'uint64_t ga_{p} = R64(a + {off});')
                off += 8
            elif b in ('GLfloat', 'GLclampf'):
                lines.append(f'float v_{p} = RF(a + {off});'); off += 4
            elif b in ('GLdouble', 'GLclampd'):
                lines.append(f'double v_{p} = RD(a + {off});'); off += 8
            elif SCALAR[b][0] == -4:
                lines.append(f'void *v_{p} = (void *)(uintptr_t)R32(a + {off});'); off += 4
            elif abs(SCALAR[b][0]) == 8:
                lines.append(f'{SCALAR[b][1]} v_{p} = ({SCALAR[b][1]})R64(a + {off});'); off += 8
            else:
                lines.append(f'{SCALAR[b][1]} v_{p} = ({SCALAR[b][1]})R32(a + {off});'); off += 4
        cur = off
        lines.append(f'const uint8_t *in = a + {cur};')
        for t, p in ps:
            if '*' not in t:
                call.append('v_' + p)
                continue
            s = spec.get(p, '')
            es = esize(t)
            if s.startswith('in:'):
                lines.append(f'{host_type(t).replace("const ", "")} v_{p} = ({host_type(t).replace("const ", "")})qgl_take_in(g, &in, end, {es});')
                call.append('v_' + p)
            elif s.startswith('out:'):
                expr = re.sub(r'\b(' + '|'.join(q for _, q in ps) + r')\b', r'v_\1', s[4:])
                lines.append(f'size_t n_{p} = (size_t)({expr});')
                lines.append(f'{host_type(t)} v_{p} = ({host_type(t)})qgl_out(g, ga_{p}, n_{p}, {es});')
                post.append(f'qgl_out_done(g, ga_{p}, v_{p}, n_{p}, {es});')
                call.append('v_' + p)
            elif s == 'raw':
                lines.append(f'{host_type(t)} v_{p} = ({host_type(t)})qgl_raw(g, ga_{p});')
                call.append('v_' + p)
            elif s == 'pix':
                pack = n.startswith(('glGet', 'glReadPixels'))
                lines.append(f'{host_type(t)} v_{p} = ({host_type(t)})qgl_pix(g, ga_{p}, {1 if pack else 0});')
                call.append('v_' + p)
            elif s in ('addr', 'val'):
                call.append(f'ga_{p}')
            else:
                raise SystemExit(f'{n}: no spec for {p}')
        if n in HOST_CUSTOM:
            fn = f'qglh_{n}(g{"".join(", " + c for c in call)})'
        else:
            fn = f'p_{n}({", ".join(call)})'
        rk = ret_kind(f)
        if rk == 'ptr':
            lines.append(f'g->ret = (uint64_t)(uintptr_t){fn};')
        elif rk == 'GLhandleARB':
            lines.append(f'g->ret = (uint64_t)(uintptr_t){fn};')
        elif rk in ('GLfloat',):
            lines.append(f'{{ float r = {fn}; uint32_t u; memcpy(&u, &r, 4); g->ret = u; }}')
        elif rk:
            lines.append(f'g->ret = (uint64_t)(int64_t){fn};')
        else:
            lines.append(f'{fn};')
        lines += post
        lines.append('(void)in; break;')
        h.append(f'    case {f["id"]}: {{ /* {n} */ ' + ' '.join(lines) + ' }')
    h.append('    default: qgl_bad_id(g, id); break;')
    h.append('    }')
    h.append('}')
    h.append('static const char *const qgl_id_names[] = {')
    for f in funcs:
        h.append(f'    [{f["id"]}] = "{f["name"]}",')
    h.append('};')
    h.append(f'#define QGL_NFUNCS {nid}')
    (ENGINE / 'qrx_gl68k_gen.inc').write_text('\n'.join(h) + '\n')
    unsup = [f['name'] for f in funcs if f['spec'] is None]
    print(f'{len(funcs)} functions, {len(supported)} supported, {len(unsup)} not', file=sys.stderr)
    (HERE / 'unsupported.txt').write_text('\n'.join(unsup) + '\n')


if __name__ == '__main__':
    main()
