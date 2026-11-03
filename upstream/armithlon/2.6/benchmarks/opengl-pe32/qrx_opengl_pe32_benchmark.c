/* SPDX-License-Identifier: MIT
 * 32-bit Windows OpenGL 1.1 API smoke benchmark for the QRX86 PE32 interpreter.
 * Uses ONLY the tiny, documented ARMITHLON 0.9 OpenGL HLE subset.
 * A host output server listening on ARMITHLON_QRX86_SOCKET is required.
 * This is not a true raster/GPU timing benchmark: counts submitted frames. */
#include <windows.h>
#include <GL/gl.h>
#include <stdlib.h>

static void frame(int n) {
    int x, y;
    glClearColor(0.08f, 0.11f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glBegin(GL_TRIANGLES);
    for (y = 0; y < 12; ++y) for (x = 0; x < 20; ++x) {
        float xx = (float)(x * 39 + 4);
        float yy = (float)(y * 35 + 3);
        float shift = (float)(n % 12);
        float red = (float)(x + 2) / 23.0f;
        float green = (float)(y + 2) / 15.0f;
        glColor3f(red, green, 0.8f); glVertex3f(xx + shift, yy, 0.15f);
        glColor3f(red, 0.8f, green); glVertex3f(xx + 34 + shift, yy, 0.15f);
        glColor3f(0.9f, green, red); glVertex3f(xx + 17 + shift, yy + 29, 0.15f);
    }
    glEnd();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBegin(GL_TRIANGLES);
    for (x = 0; x < 16; ++x) {
        float xx = (float)(10 + x * 48);
        float yy = (float)(140 + (x % 4) * 45);
        glColor4f(1.0f, 0.8f, 0.2f, 0.35f); glVertex3f(xx, yy, 0.1f);
        glColor4f(0.2f, 0.8f, 1.0f, 0.35f); glVertex3f(xx + 37, yy, 0.1f);
        glColor4f(0.9f, 0.2f, 0.5f, 0.35f); glVertex3f(xx + 16, yy + 38, 0.1f);
    }
    glEnd();
    glDisable(GL_BLEND);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR args, int show) {
    int i, frames = 10;
    HGLRC rc;
    (void)instance; (void)previous; (void)show;
    if (args && *args) {
        frames = atoi(args);
        if (frames <= 0 || frames > 500) return 3;
    }
    /* HDC=1 is only accepted by the explicitly documented QRX86 WGL stub;
       DO NOT run this program outside ARMITHLON. It is not native GL/WGL. */
    rc = wglCreateContext((HDC)(UINT_PTR)1);
    if (!rc || !wglMakeCurrent((HDC)(UINT_PTR)1, rc)) return 4;
    glOrtho(0.0, 800.0, 450.0, 0.0, -1.0, 1.0);
    for (i = 0; i < frames; ++i) {
        frame(i);
        if (glGetError() != GL_NO_ERROR) return 6;
        if (!SwapBuffers((HDC)(UINT_PTR)1)) return 7;
    }
    wglMakeCurrent((HDC)0, (HGLRC)0);
    wglDeleteContext(rc);
    return 0;
}
