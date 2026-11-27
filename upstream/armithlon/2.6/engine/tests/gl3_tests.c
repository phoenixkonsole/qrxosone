/* The GL 3.3 door (HOST_CALL service 13, full68k/qrx_gl3.c) from the host side: streams as the guest
 * library writes them (big-endian words) in a block of fake guest RAM. Draws with a GLSL 330 program,
 * a vertex array object, instancing, a texture from big-endian texels and an FBO, then reads the
 * window back. Skips (exit 77) when the host has no OpenGL 3.3 core context. */
#include "../full68k/qrx_gl3.h"
#include "../../../../../tools/qrxgl68k/guest/qgl3_ids.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_CREATE = 1, OP_DESTROY, OP_RESIZE, OP_READFRAME, OP_GETSTRING, OP_SHADERSOURCE, OP_GETERROR };
#define RAM_SIZE (8u << 20)
static uint8_t *ram;
static uint32_t ctx;
/* guest memory: the block at 0x1000, the stream at 0x10000, data from 0x100000 */
#define BLOCK 0x1000u
#define STREAM 0x10000u
static uint32_t sw;  /* stream words */
static uint64_t data_top = 0x100000;

static void put(uint32_t v) { uint32_t b = __builtin_bswap32(v); memcpy(ram + STREAM + sw++ * 4, &b, 4); }
static void put64(uint64_t v) { put((uint32_t)(v >> 32)); put((uint32_t)v); }
static void putf(float f) { uint32_t u; memcpy(&u, &f, 4); put(u); }
static void call(unsigned id, unsigned nw) { put(id << 20 | nw); }
static uint64_t gdata(const void *p, size_t n)
{
    uint64_t a = data_top;
    memcpy(ram + a, p, n);
    data_top = (data_top + n + 15) & ~15ull;
    return a;
}
static uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, ram + a, 4); return __builtin_bswap32(v); }
static void wr32(uint64_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(ram + a, &v, 4); }
static uint64_t run(void)
{
    uint8_t *q = ram + BLOCK;
    memset(q, 0, 64);
    wr32(BLOCK + 0, 13); wr32(BLOCK + 12, 1); wr32(BLOCK + 16, 0); wr32(BLOCK + 20, STREAM);
    wr32(BLOCK + 24, sw * 4); wr32(BLOCK + 28, ctx);
    qrx_gl3_service(ram, RAM_SIZE, q);
    sw = 0;
    return (uint64_t)rd32(BLOCK + 32) << 32 | rd32(BLOCK + 36);
}
static uint32_t err(void) { call(OP_GETERROR, 0); return (uint32_t)run(); }

static void source(uint32_t sh, const char *s)
{
    uint32_t n = (uint32_t)strlen(s), w = (n + 3) / 4;
    call(OP_SHADERSOURCE, 2 + w);
    put(sh); put(n);
    memcpy(ram + STREAM + sw * 4, s, n);
    memset(ram + STREAM + sw * 4 + n, 0, w * 4 - n);
    sw += w;
}
static uint32_t shader(uint32_t type, const char *s)
{
    call(QGL3_ID_glCreateShader, 1); put(type);
    uint32_t sh = (uint32_t)run();
    source(sh, s);
    call(QGL3_ID_glCompileShader, 1); put(sh);
    uint64_t st = data_top; data_top += 16;
    call(QGL3_ID_glGetShaderiv, 4); put(sh); put(0x8B81 /* COMPILE_STATUS */); put64(st);
    run();
    if (rd32(st) != 1) {
        uint64_t log = data_top; data_top += 4096;
        call(QGL3_ID_glGetShaderInfoLog, 6); put(sh); put(4096); put64(0); put64(log);
        run();
        fprintf(stderr, "shader: %s\n", (char *)ram + log);
        exit(1);
    }
    return sh;
}

static const char *VS =
    "#version 330 core\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in vec2 offs;\n"   /* per instance */
    "out vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos * 0.5 + offs, 0.0, 1.0); }\n";
static const char *FS =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "uniform vec4 tint;\n"
    "uniform sampler2D tex;\n"
    "out vec4 o;\n"
    "void main() { o = texture(tex, uv) * tint; }\n";

static uint32_t pixel(uint64_t frame, int w, int x, int y) { return rd32(frame + ((uint64_t)y * w + x) * 4); }

int main(void)
{
    if (!qrx_gl3_available()) {
        printf("no OpenGL 3.3 core context on this host: skipped\n");
        return 77;
    }
    ram = calloc(1, RAM_SIZE);
    call(OP_CREATE, 1); put(0);
    ctx = (uint32_t)run();
    assert(ctx);
    call(OP_RESIZE, 2); put(64); put(64);
    assert(run() == 1);
    uint64_t sbuf = data_top; data_top += 1024;
    call(OP_GETSTRING, 5); put(0x1F02 /* VERSION */); put(0xFFFFFFFFu); put64(sbuf); put(1024);
    run();
    printf("GL_VERSION %s\n", (char *)ram + sbuf);
    call(OP_GETSTRING, 5); put(0x1F01 /* RENDERER */); put(0xFFFFFFFFu); put64(sbuf); put(1024);
    run();
    printf("GL_RENDERER %s\n", (char *)ram + sbuf);

    /* the extension list holds only what the door carries */
    uint64_t ne = data_top; data_top += 16;
    call(QGL3_ID_glGetIntegerv, 3); put(0x821D /* NUM_EXTENSIONS */); put64(ne);
    run();
    printf("%u extensions carried\n", rd32(ne));
    for (uint32_t i = 0; i < rd32(ne); i++) {
        call(OP_GETSTRING, 5); put(0x1F03); put(i); put64(sbuf); put(1024);
        assert(run() != 0xFFFFFFFFu);
        assert(strcmp((char *)ram + sbuf, "GL_ARB_texture_storage") != 0);
    }
    call(OP_GETSTRING, 5); put(0x1F03); put(rd32(ne)); put64(sbuf); put(1024);
    assert(run() == 0xFFFFFFFFu);

    /* a clear and the window's frame */
    uint64_t frame = data_top; data_top += 64 * 64 * 4;
    call(QGL3_ID_glClearColor, 4); putf(0.0f); putf(0.0f); putf(1.0f); putf(1.0f);
    call(QGL3_ID_glClear, 1); put(0x4000);
    call(OP_READFRAME, 5); put64(frame); put(64 * 4); put(64); put(64);
    run();
    printf("clear: %08x\n", pixel(frame, 64, 10, 10));
    assert(pixel(frame, 64, 10, 10) == 0xFF0000FFu);
    assert(err() == 0);

    /* framebuffer 0 as the guest sees it */
    uint64_t iv = data_top; data_top += 64;
    call(QGL3_ID_glGetIntegerv, 3); put(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */); put64(iv);
    run();
    assert(rd32(iv) == 0);
    call(QGL3_ID_glGetIntegerv, 3); put(0x0BA2 /* VIEWPORT */); put64(iv);
    run();
    assert(rd32(iv + 8) == 64 && rd32(iv + 12) == 64);

    /* a program */
    uint32_t vs = shader(0x8B31, VS), fs = shader(0x8B30, FS);
    call(QGL3_ID_glCreateProgram, 0);
    uint32_t prog = (uint32_t)run();
    call(QGL3_ID_glAttachShader, 2); put(prog); put(vs);
    call(QGL3_ID_glAttachShader, 2); put(prog); put(fs);
    call(QGL3_ID_glLinkProgram, 1); put(prog);
    call(QGL3_ID_glGetProgramiv, 4); put(prog); put(0x8B82 /* LINK_STATUS */); put64(iv);
    run();
    assert(rd32(iv) == 1);
    call(QGL3_ID_glUseProgram, 1); put(prog);
    uint64_t name = gdata("tint", 5);
    call(QGL3_ID_glGetUniformLocation, 3); put(prog); put64(name);
    int32_t tint = (int32_t)run();
    assert(tint >= 0);
    /* glUniform4fv: an inline array of big-endian floats */
    call(QGL3_ID_glUniform4fv, 2 + 2 + 1 + 4); put((uint32_t)tint); put(1); put64(name); put(16);
    putf(1.0f); putf(1.0f); putf(0.0f); putf(1.0f);

    /* a vertex array: positions and per-instance offsets, little endian as the host reads them */
    call(QGL3_ID_glGenVertexArrays, 3); put(1); put64(iv);
    run();
    uint32_t vao = rd32(iv);
    call(QGL3_ID_glBindVertexArray, 1); put(vao);
    call(QGL3_ID_glGenBuffers, 3); put(2); put64(iv);
    run();
    uint32_t vb = rd32(iv), ib = rd32(iv + 4);
    float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    float offs[] = {-0.5f, -0.5f, 0.5f, 0.5f};
    call(QGL3_ID_glBindBuffer, 2); put(0x8892); put(vb);
    call(QGL3_ID_glBufferData, 6); put(0x8892); put64(sizeof quad); put64(gdata(quad, sizeof quad)); put(0x88E4);
    call(QGL3_ID_glVertexAttribPointer, 7); put(0); put(2); put(0x1406); put(0); put(8); put64(0);
    call(QGL3_ID_glEnableVertexAttribArray, 1); put(0);
    call(QGL3_ID_glBindBuffer, 2); put(0x8892); put(ib);
    call(QGL3_ID_glBufferData, 6); put(0x8892); put64(sizeof offs); put64(gdata(offs, sizeof offs)); put(0x88E4);
    call(QGL3_ID_glVertexAttribPointer, 7); put(1); put(2); put(0x1406); put(0); put(8); put64(0);
    call(QGL3_ID_glEnableVertexAttribArray, 1); put(1);
    call(QGL3_ID_glVertexAttribDivisor, 2); put(1); put(1);

    /* a 2x2 texture: big-endian 32-bit texels R G B A with UNSIGNED_INT_8_8_8_8 */
    uint32_t tex_be[4];
    for (int i = 0; i < 4; i++) tex_be[i] = __builtin_bswap32(0x80FF40FFu);  /* in guest memory */
    call(QGL3_ID_glGenTextures, 3); put(1); put64(iv);
    run();
    uint32_t tex = rd32(iv);
    call(QGL3_ID_glBindTexture, 2); put(0x0DE1); put(tex);
    call(QGL3_ID_glTexParameteri, 3); put(0x0DE1); put(0x2801); put(0x2600);
    call(QGL3_ID_glTexParameteri, 3); put(0x0DE1); put(0x2800); put(0x2600);
    call(QGL3_ID_glTexImage2D, 10); put(0x0DE1); put(0); put(0x8058); put(2); put(2); put(0); put(0x1908); put(0x8035);
    put64(gdata(tex_be, sizeof tex_be));

    call(QGL3_ID_glClear, 1); put(0x4000);
    call(QGL3_ID_glDrawArraysInstanced, 4); put(5 /* TRIANGLE_STRIP */); put(0); put(4); put(2);
    call(OP_READFRAME, 5); put64(frame); put(64 * 4); put(64); put(64);
    run();
    /* top row first: the instance at (+0.5, +0.5) is top right */
    uint32_t tr = pixel(frame, 64, 48, 16), bl = pixel(frame, 64, 16, 48), tl = pixel(frame, 64, 16, 16);
    printf("draw: top right %08x, bottom left %08x, top left %08x\n", tr, bl, tl);
    call(OP_READFRAME, 6); put64(frame); put(64 * 4); put(64); put(64); put(1);  /* B G R A bytes */
    run();
    assert(pixel(frame, 64, 48, 16) == 0x00FF80FFu);
    assert(tl == 0xFF0000FFu);
    assert(tr == 0xFF80FF00u && bl == 0xFF80FF00u);  /* texel (0x80, 0xFF, 0x40) times tint (1, 1, 0) */
    assert(err() == 0);

    /* an FBO with a texture, cleared, blitted to the window */
    call(QGL3_ID_glGenFramebuffers, 3); put(1); put64(iv);
    run();
    uint32_t fbo = rd32(iv);
    call(QGL3_ID_glBindFramebuffer, 2); put(0x8D40); put(fbo);
    call(QGL3_ID_glFramebufferTexture2D, 5); put(0x8D40); put(0x8CE0); put(0x0DE1); put(tex); put(0);
    call(QGL3_ID_glCheckFramebufferStatus, 1); put(0x8D40);
    assert(run() == 0x8CD5);
    call(QGL3_ID_glViewport, 4); put(0); put(0); put(2); put(2);
    call(QGL3_ID_glClearColor, 4); putf(1.0f); putf(0.0f); putf(0.0f); putf(1.0f);
    call(QGL3_ID_glClear, 1); put(0x4000);
    call(QGL3_ID_glBindFramebuffer, 2); put(0x8CA9 /* DRAW */); put(0);
    call(QGL3_ID_glBlitFramebuffer, 10); put(0); put(0); put(2); put(2); put(0); put(0); put(64); put(64); put(0x4000); put(0x2600);
    /* glReadPixels from the window into guest memory: big endian with the type's element */
    call(QGL3_ID_glBindFramebuffer, 2); put(0x8D40); put(0);
    uint64_t px = data_top; data_top += 16;
    call(QGL3_ID_glReadPixels, 8); put(5); put(5); put(1); put(1); put(0x1908); put(0x8035); put64(px);
    run();
    printf("blit, read back: %08x\n", rd32(px));
    assert(rd32(px) == 0xFF0000FFu);
    assert(err() == 0);

    /* a fence */
    call(QGL3_ID_glFenceSync, 2); put(0x9117); put(0);
    uint64_t sync = run();
    assert(sync);
    call(QGL3_ID_glClientWaitSync, 4); put64(sync); put(1); put64(1000000000ull);
    uint32_t w = (uint32_t)run();
    assert(w == 0x911A || w == 0x911C);
    call(QGL3_ID_glDeleteSync, 2); put64(sync);
    assert(err() == 0);

    call(OP_DESTROY, 1); put(ctx);
    run();
    qrx_gl3_report();
    printf("gl3 door: ok\n");
    return 0;
}
