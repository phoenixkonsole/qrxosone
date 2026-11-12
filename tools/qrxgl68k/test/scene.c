/* One frame that touches the door's parts: immediate mode, textures in three pixel types, buffer
 * objects with interleaved vertices and indices, client arrays, a GLSL 1.20 program with uniforms, a
 * display list, an FBO and back to the window, glReadPixels, state queries. The same source runs
 * natively on the Mac (host_main.c) and as a 68k program through the door (guest_main.c); the two
 * frames must be the same. */
#include "scene.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *vs =
    "#version 120\n"
    "uniform mat4 m;\n"
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = m * vec4(pos, 0.0, 1.0); }\n";
static const char *fs =
    "#version 120\n"
    "uniform vec4 tint;\n"
    "uniform sampler2D tex;\n"
    "varying vec2 uv;\n"
    "void main() { gl_FragColor = texture2D(tex, uv) * tint; }\n";

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    const char *parts[2] = {src, ""};
    GLint lens[2] = {-1, 0};
    glShaderSource(s, 2, parts, lens);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof log, &n, log);
        printf("shader log (%d): %s\n", (int)n, log);
    }
    CHECK(ok, "shader compile");
    return s;
}

int scene_draw(int w, int h)
{
    fails = 0;
    printf("GL_VERSION %s\n", (const char *)glGetString(GL_VERSION));
    printf("GL_SHADING_LANGUAGE_VERSION %s\n", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    CHECK(ext && strstr(ext, "GL_EXT_framebuffer_object"), "extensions");
    GLint vp[4] = {0};
    glGetIntegerv(GL_VIEWPORT, vp);
    CHECK(vp[2] == w && vp[3] == h, "viewport %d %d %d %d", vp[0], vp[1], vp[2], vp[3]);
    GLint fb = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fb);
    CHECK(fb == 0, "framebuffer binding %d", fb);

    glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, 0, h, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Immediate mode, doubles and a vector form. */
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex2f(10, 10);
    glColor3ub(0, 255, 0); glVertex2d(110, 10);
    static const GLfloat blue[3] = {0, 0, 1};
    glColor3fv(blue); glVertex2i(60, 90);
    glEnd();

    /* Textures: RGBA bytes, 5_6_5 shorts and floats, each a 4x4 checker. */
    GLuint tex[3];
    glGenTextures(3, tex);
    GLubyte rgba[4 * 4 * 4];
    GLushort rgb565[16];
    GLfloat lum[16];
    for (int i = 0; i < 16; i++) {
        int on = ((i & 3) + (i >> 2)) & 1;
        rgba[i * 4 + 0] = on ? 255 : 30; rgba[i * 4 + 1] = on ? 200 : 30; rgba[i * 4 + 2] = 40; rgba[i * 4 + 3] = 255;
        rgb565[i] = on ? 0xF800 : 0x07E0;
        lum[i] = on ? 1.0f : 0.25f;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565);
    glBindTexture(GL_TEXTURE_2D, tex[2]);
    GLint filt[1] = {GL_NEAREST};
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 4, 4, 0, GL_LUMINANCE, GL_FLOAT, lum);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    for (int t = 0; t < 3; t++) {
        glBindTexture(GL_TEXTURE_2D, tex[t]);
        float x = 130 + t * 60;
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x, 10);
        glTexCoord2f(1, 0); glVertex2f(x + 50, 10);
        glTexCoord2f(1, 1); glVertex2f(x + 50, 60);
        glTexCoord2f(0, 1); glVertex2f(x, 60);
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);

    /* A buffer object with interleaved float positions and byte colors, indices in another one. */
    struct V { GLfloat x, y; GLubyte r, g, b, a; } verts[4] = {
        {10, 110, 255, 255, 0, 255}, {90, 110, 0, 255, 255, 255}, {90, 190, 255, 0, 255, 255}, {10, 190, 255, 255, 255, 255}};
    GLushort idx[6] = {0, 1, 2, 0, 2, 3};
    GLuint bo[2];
    glGenBuffers(2, bo);
    glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, NULL, GL_STATIC_DRAW);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof idx, idx);
    GLint bsize = 0;
    glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &bsize);
    CHECK(bsize == (GLint)sizeof idx, "buffer size %d", bsize);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, sizeof(struct V), (const void *)0);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct V), (const void *)8);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);
    /* Mapped: move the quad right and draw again. */
    struct V *m = glMapBuffer(GL_ARRAY_BUFFER, GL_READ_WRITE);
    CHECK(m != NULL, "map");
    if (m) {
        CHECK(m[1].x == 90.0f && m[2].r == 255, "mapped contents %f %d", (double)m[1].x, m[2].r);
        for (int i = 0; i < 4; i++) m[i].x += 100;
        CHECK(glUnmapBuffer(GL_ARRAY_BUFFER), "unmap");
    }
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    GLint ab = -1;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    CHECK(ab == 0, "array buffer binding %d", ab);

    /* Client arrays in program memory, shorts, glDrawArrays. */
    GLshort sv[8] = {210, 110, 290, 110, 290, 190, 210, 190};
    GLfloat sc[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0};
    glVertexPointer(2, GL_SHORT, 0, sv);
    glColorPointer(3, GL_FLOAT, 0, sc);
    glDrawArrays(GL_QUADS, 0, 4);
    GLvoid *vp_back = NULL;
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, &vp_back);
    CHECK(vp_back == sv, "vertex array pointer");
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    /* GLSL program with a matrix and a vector uniform, vertices through a generic attribute. */
    GLuint prog = glCreateProgram();
    glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    CHECK(linked, "link");
    glUseProgram(prog);
    GLint um = glGetUniformLocation(prog, "m"), ut = glGetUniformLocation(prog, "tint"), us = glGetUniformLocation(prog, "tex");
    CHECK(um >= 0 && ut >= 0 && us >= 0, "uniform locations %d %d %d", um, ut, us);
    /* Maps -1..1 to the square 310..390 x 110..190 in window coordinates. */
    GLfloat mat[16] = {80.0f / w, 0, 0, 0, 0, 80.0f / h, 0, 0, 0, 0, 1, 0, 2 * 350.0f / w - 1, 2 * 150.0f / h - 1, 0, 1};
    glUniformMatrix4fv(um, 1, GL_FALSE, mat);
    GLfloat tint[4] = {0.5f, 1.0f, 1.0f, 1.0f};
    glUniform4fv(ut, 1, tint);
    glUniform1i(us, 0);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    GLfloat quad[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(0);
    glUseProgram(0);

    /* A display list drawn twice. */
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0.5f, 0); glVertex2f(0, 0); glVertex2f(30, 0); glVertex2f(15, 30);
    glEnd();
    glEndList();
    glPushMatrix(); glTranslatef(410, 110, 0); glCallList(list); glPopMatrix();
    glPushMatrix(); glTranslatef(450, 150, 0); glCallList(list); glPopMatrix();

    /* Render into a texture through an FBO, then back to the window and draw it. */
    GLuint ftex, fbo;
    glGenTextures(1, &ftex);
    glBindTexture(GL_TEXTURE_2D, ftex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, ftex, 0);
    CHECK(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT, "fbo");
    glViewport(0, 0, 32, 32);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glDrawBuffer(GL_BACK);
    glViewport(0, 0, w, h);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(500, 10);
    glTexCoord2f(1, 0); glVertex2f(560, 10);
    glTexCoord2f(1, 1); glVertex2f(560, 70);
    glTexCoord2f(0, 1); glVertex2f(500, 70);
    glEnd();
    glDisable(GL_TEXTURE_2D);

    /* Blender 2.79's mesh path: positions and GL_SHORT normals in buffers filled through glMapBuffer, GL_UNSIGNED_INT
     * indices in an element buffer, fixed-function lighting, depth test, a perspective projection. */
    {
        static const GLfloat cube[8][3] = {{1, 1, -1}, {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, 1}, {1, -1, 1}, {-1, -1, 1}, {-1, 1, 1}};
        static const GLuint quads[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {4, 0, 3, 7}};
        static const GLshort nrm[6][3] = {{0, 0, -32767}, {0, 0, 32767}, {32767, 0, 0}, {0, -32767, 0}, {-32767, 0, 0}, {0, 32767, 0}};
        GLuint b[3];
        glGenBuffers(3, b);
        glBindBuffer(GL_ARRAY_BUFFER, b[0]);
        glBufferData(GL_ARRAY_BUFFER, 24 * 12, NULL, GL_STATIC_DRAW);
        GLfloat *pv = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
        glBindBuffer(GL_ARRAY_BUFFER, b[1]);
        glBufferData(GL_ARRAY_BUFFER, 24 * 8, NULL, GL_STATIC_DRAW);
        GLshort *pn = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
        for (int f = 0; f < 6; f++)
            for (int k = 0; k < 4; k++) {
                memcpy(pv + (f * 4 + k) * 3, cube[quads[f][k]], 12);
                memcpy(pn + (f * 4 + k) * 4, nrm[f], 6);
                pn[(f * 4 + k) * 4 + 3] = 0;
            }
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glBindBuffer(GL_ARRAY_BUFFER, b[0]);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        GLuint tri[36];
        for (int f = 0; f < 6; f++) {
            GLuint q = (GLuint)f * 4;
            GLuint t[6] = {q, q + 1, q + 2, q, q + 2, q + 3};
            memcpy(tri + f * 6, t, sizeof t);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b[2]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
        glViewport(400, 200, 200, 180);
        glScissor(400, 200, 200, 180);
        glEnable(GL_SCISSOR_TEST);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glFrustum(-0.1, 0.1, -0.09, 0.09, 0.1, 100);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glTranslatef(0, 0, -6);
        glRotatef(30, 1, 0, 0);
        glRotatef(40, 0, 1, 0);
        GLfloat lpos[4] = {1, 1, 1, 0}, diff[4] = {0.8f, 0.8f, 0.8f, 1};
        glLightfv(GL_LIGHT0, GL_POSITION, lpos);
        glEnable(GL_LIGHT0);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, diff);
        glEnable(GL_LIGHTING);
        glEnable(GL_NORMALIZE);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glBindBuffer(GL_ARRAY_BUFFER, b[0]);
        glVertexPointer(3, GL_FLOAT, 0, (const void *)0);
        glBindBuffer(GL_ARRAY_BUFFER, b[1]);
        glNormalPointer(GL_SHORT, 8, (const void *)0);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, (const void *)0);
        glDisable(GL_LIGHTING);
        glColor3f(1, 0.6f, 0);
        glDepthMask(GL_FALSE);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDrawElements(GL_LINES, 24, GL_UNSIGNED_INT, (const void *)0);
        glDepthMask(GL_TRUE);
        glDisableClientState(GL_VERTEX_ARRAY);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glViewport(0, 0, w, h);
    }

    /* Reading back: one pixel as bytes, one as floats, and the matrix stack. */
    glFinish();
    GLubyte px[4] = {0};
    glReadPixels(30, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px[0] > 100 && px[2] < 150, "red corner %d %d %d", px[0], px[1], px[2]);
    GLfloat pf[4] = {0};
    glReadPixels(530, 40, 1, 1, GL_RGBA, GL_FLOAT, pf);
    CHECK(pf[0] > 0.99f && pf[1] < 0.01f && pf[2] > 0.99f, "fbo texture %f %f %f", (double)pf[0], (double)pf[1], (double)pf[2]);
    GLfloat mv[16];
    glMatrixMode(GL_PROJECTION);
    glGetFloatv(GL_PROJECTION_MATRIX, mv);
    CHECK(mv[0] > 0.0f && mv[12] == -1.0f, "projection %f %f", (double)mv[0], (double)mv[12]);
    glMatrixMode(GL_MODELVIEW);
    GLenum err = glGetError();
    CHECK(err == GL_NO_ERROR, "gl error 0x%x", err);
    printf("%s, %d failed\n", fails ? "FAILED" : "PASSED", fails);
    return fails;
}

int scene_write_ppm(const char *path, const unsigned char *argb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
        fwrite(argb + i * 4 + 1, 1, 3, f);
    fclose(f);
    return 0;
}
