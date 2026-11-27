/* EC64GL3Test: the GL 3.3 door from a program of QRX OS One 64 (HOST_CALL service 13,
 * tools/qrxgl68k/guest/qgl3.c). A GLSL 330 program, a vertex array object with per-instance data,
 * a texture from big-endian texels, an FBO blitted to the window, a fence; checks the pixels it reads
 * back and prints PASS or FAIL.
 *   EC64GL3Test [FILE.ppm]   with a file, the frame goes there too
 * Built with the AROS 64 SDK, see build-ec64.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../guest/qgl3.h"

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef long GLsizeiptr;

#define F(ret, name, args) static ret (*name) args;
#define FUNCS \
    F(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) F(void, glClear, (GLbitfield)) \
    F(GLuint, glCreateShader, (GLenum)) F(void, glShaderSource, (GLuint, GLsizei, const char *const *, const GLint *)) \
    F(void, glCompileShader, (GLuint)) F(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    F(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *)) F(GLuint, glCreateProgram, (void)) \
    F(void, glAttachShader, (GLuint, GLuint)) F(void, glLinkProgram, (GLuint)) F(void, glUseProgram, (GLuint)) \
    F(void, glGetProgramiv, (GLuint, GLenum, GLint *)) F(GLint, glGetUniformLocation, (GLuint, const char *)) \
    F(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) F(void, glUniform1i, (GLint, GLint)) \
    F(void, glGenVertexArrays, (GLsizei, GLuint *)) F(void, glBindVertexArray, (GLuint)) \
    F(void, glGenBuffers, (GLsizei, GLuint *)) F(void, glBindBuffer, (GLenum, GLuint)) \
    F(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    F(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    F(void, glEnableVertexAttribArray, (GLuint)) F(void, glVertexAttribDivisor, (GLuint, GLuint)) \
    F(void, glGenTextures, (GLsizei, GLuint *)) F(void, glBindTexture, (GLenum, GLuint)) \
    F(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    F(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    F(void, glDrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei)) \
    F(void, glGenFramebuffers, (GLsizei, GLuint *)) F(void, glBindFramebuffer, (GLenum, GLuint)) \
    F(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    F(GLenum, glCheckFramebufferStatus, (GLenum)) F(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    F(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    F(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    F(void *, glFenceSync, (GLenum, GLbitfield)) F(GLenum, glClientWaitSync, (void *, GLbitfield, unsigned long long)) \
    F(void, glDeleteSync, (void *)) F(GLenum, glGetError, (void)) F(const unsigned char *, glGetString, (GLenum)) \
    F(const unsigned char *, glGetStringi, (GLenum, GLuint)) F(void, glGetIntegerv, (GLenum, GLint *))
FUNCS
#undef F

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static const char *VS =
    "#version 330 core\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in vec2 offs;\n"
    "out vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos * 0.5 + offs, 0.0, 1.0); }\n";
static const char *FS =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "uniform vec4 tint;\n"
    "uniform sampler2D tex;\n"
    "out vec4 o;\n"
    "void main() { o = texture(tex, uv) * tint; }\n";

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, 0x8B81, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        printf("FAIL: shader: %s\n", log);
        exit(20);
    }
    return s;
}

/* little endian, as the host reads buffer data */
static void le_floats(void *dst, const float *f, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &f[i], 4);
        u = __builtin_bswap32(u);
        memcpy((char *)dst + i * 4, &u, 4);
    }
}

#define W 64
#define H 64
int main(int argc, char **argv)
{
    if (!qgl3_available()) {
        printf("no GL 3.3 door on this host\n");
        return 10;
    }
    qgl3_context *ctx = qgl3_create_context(NULL);
    if (!ctx || !qgl3_make_current(ctx) || !qgl3_resize(W, H)) {
        printf("FAIL: no context\n");
        return 20;
    }
#define F(ret, name, args) name = (ret (*) args)qgl3_GetProcAddress(#name); if (!name) { printf("FAIL: no %s\n", #name); return 20; }
    FUNCS
#undef F
    printf("GL_VERSION %s\nGL_RENDERER %s\nGLSL %s\n", glGetString(0x1F02), glGetString(0x1F01), glGetString(0x8B8C));
    GLint next = 0;
    glGetIntegerv(0x821D /* NUM_EXTENSIONS */, &next);
    printf("%d extensions, the first %s\n", next, next > 0 ? (const char *)glGetStringi(0x1F03, 0) : "-");

    static uint32_t frame[W * H];
    glClearColor(0, 0, 1, 1);
    glClear(0x4000);
    qgl3_read_frame(frame, W * 4, W, H);
    CHECK(frame[10 * W + 10] == 0xFF0000FFu, "clear: %08lx", (unsigned long)frame[10 * W + 10]);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, shader(0x8B31, VS));
    glAttachShader(prog, shader(0x8B30, FS));
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, 0x8B82, &ok);
    CHECK(ok == 1, "link");
    glUseProgram(prog);
    static const float tint[4] = {1, 1, 0, 1};
    glUniform4fv(glGetUniformLocation(prog, "tint"), 1, tint);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);

    GLuint vao, vb[2], tex, fbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, vb);
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1}, offs[] = {-0.5f, -0.5f, 0.5f, 0.5f};
    float le[8];
    le_floats(le, quad, 8);
    glBindBuffer(0x8892, vb[0]);
    glBufferData(0x8892, sizeof le, le, 0x88E4);
    glVertexAttribPointer(0, 2, 0x1406, 0, 8, (void *)0);
    glEnableVertexAttribArray(0);
    le_floats(le, offs, 4);
    glBindBuffer(0x8892, vb[1]);
    glBufferData(0x8892, 16, le, 0x88E4);
    glVertexAttribPointer(1, 2, 0x1406, 0, 8, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 1);

    static const uint32_t texels[4] = {0x80FF40FFu, 0x80FF40FFu, 0x80FF40FFu, 0x80FF40FFu}; /* R G B A */
    glGenTextures(1, &tex);
    glBindTexture(0x0DE1, tex);
    glTexParameteri(0x0DE1, 0x2801, 0x2600);
    glTexParameteri(0x0DE1, 0x2800, 0x2600);
    glTexImage2D(0x0DE1, 0, 0x8058, 2, 2, 0, 0x1908, 0x8035, texels);

    glClear(0x4000);
    glDrawArraysInstanced(5, 0, 4, 2);
    qgl3_read_frame(frame, W * 4, W, H);
    CHECK(frame[16 * W + 48] == 0xFF80FF00u, "instance top right: %08lx", (unsigned long)frame[16 * W + 48]);
    CHECK(frame[48 * W + 16] == 0xFF80FF00u, "instance bottom left: %08lx", (unsigned long)frame[48 * W + 16]);
    CHECK(frame[16 * W + 16] == 0xFF0000FFu, "background: %08lx", (unsigned long)frame[16 * W + 16]);
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (int i = 0; i < W * H; i++) {
                unsigned char rgb[3] = {(unsigned char)(frame[i] >> 16), (unsigned char)(frame[i] >> 8), (unsigned char)frame[i]};
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(0x8D40, fbo);
    glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
    CHECK(glCheckFramebufferStatus(0x8D40) == 0x8CD5, "FBO incomplete");
    glViewport(0, 0, 2, 2);
    glClearColor(1, 0, 0, 1);
    glClear(0x4000);
    glBindFramebuffer(0x8CA9, 0);
    glBlitFramebuffer(0, 0, 2, 2, 0, 0, W, H, 0x4000, 0x2600);
    glBindFramebuffer(0x8D40, 0);
    uint32_t px = 0;
    glReadPixels(5, 5, 1, 1, 0x1908, 0x8035, &px);
    CHECK(px == 0xFF0000FFu, "blit: %08lx", (unsigned long)px);

    void *sync = glFenceSync(0x9117, 0);
    GLenum w = glClientWaitSync(sync, 1, 1000000000ull);
    CHECK(w == 0x911A || w == 0x911C, "fence: %x", w);
    glDeleteSync(sync);
    GLenum e = glGetError();
    CHECK(e == 0, "GL error %x", e);

    qgl3_destroy_context(ctx);
    printf(fails ? "EC64GL3Test: FAIL\n" : "EC64GL3Test: PASS\n");
    return fails ? 20 : 0;
}
