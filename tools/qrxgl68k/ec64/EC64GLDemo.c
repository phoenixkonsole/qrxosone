/* EC64GLDemo: OpenGL on QRX OS One 64. A lit, spinning cube drawn by the Mac through the board's GL
 * service (HOST_CALL service 8, tools/qrxgl68k/guest), shown in an Intuition window.
 *   EC64GLDemo              a window until it is closed
 *   EC64GLDemo FILE.ppm     one frame into a file, no window (for tests)
 * Built with the AROS 64 SDK, see build-ec64.sh. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include "../guest/qgl_api.h"

#define W 400
#define H 300

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;

static void cube(void)
{
    static const float n[6][3] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    static const float c[6][3] = {{1,.3f,.3f},{.3f,1,.3f},{.3f,.3f,1},{1,1,.3f},{1,.3f,1},{.3f,1,1}};
    static const float v[6][4][3] = {
        {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}},     {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}},
        {{1,-1,1},{1,-1,-1},{1,1,-1},{1,1,1}},     {{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1}},
        {{-1,1,1},{1,1,1},{1,1,-1},{-1,1,-1}},     {{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1}}};
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        glColor3fv(c[f]);
        glNormal3fv(n[f]);
        for (int k = 0; k < 4; k++) glVertex3fv(v[f][k]);
    }
    glEnd();
}
static void draw(int frame)
{
    static const float light[4] = { 2, 3, 4, 0 };
    glViewport(0, 0, W, H);
    glClearColor(0.08f, 0.10f, 0.16f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-0.8, 0.8, -0.6, 0.6, 1.5, 20);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, light);
    glTranslatef(0, 0, -6);
    glRotatef(frame * 2.0f, 1, 0, 0);
    glRotatef(frame * 3.0f, 0, 1, 0);
    cube();
}
static int to_file(const char *path, const unsigned char *argb)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) fwrite(argb + i * 4 + 1, 1, 3, f);
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char *px = malloc(W * H * 4);
    if (!px || !qgl_available()) { printf("EC64GLDemo: no GL service on this machine\n"); return 20; }
    unsigned ctx = qgl_create_context(0);
    if (!ctx || !qgl_make_current(ctx, W, H)) { printf("EC64GLDemo: no GL context\n"); return 20; }
    printf("GL: %s, %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    if (argc > 1) {
        draw(20);
        qgl_read_frame(px, W * 4, W, H);
        int ok = to_file(argv[1], px);
        printf("EC64GLDemo: %s %s\n", ok ? "wrote" : "cannot write", argv[1]);
        qgl_destroy_context(ctx);
        return ok ? 0 : 10;
    }
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    CyberGfxBase = OpenLibrary("cybergraphics.library", 40);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase) { printf("EC64GLDemo: needs intuition, graphics, cybergraphics\n"); return 20; }
    struct Window *win = OpenWindowTags(NULL, WA_Title, (IPTR)"OpenGL on QRX OS One 64", WA_InnerWidth, W,
                                        WA_InnerHeight, H, WA_DragBar, TRUE, WA_DepthGadget, TRUE,
                                        WA_CloseGadget, TRUE, WA_Activate, TRUE, WA_GimmeZeroZero, TRUE,
                                        WA_IDCMP, IDCMP_CLOSEWINDOW, TAG_DONE);
    if (!win) { printf("EC64GLDemo: no window\n"); return 20; }
    int frame = 0, done = 0;
    ULONG t0 = 0;
    while (!done) {
        struct IntuiMessage *m;
        draw(frame++);
        qgl_read_frame(px, W * 4, W, H);
        WritePixelArray(px, 0, 0, W * 4, win->RPort, 0, 0, W, H, RECTFMT_ARGB);
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            if (m->Class == IDCMP_CLOSEWINDOW) done = 1;
            ReplyMsg((struct Message *)m);
        }
        (void)t0;
        Delay(1);
    }
    CloseWindow(win);
    qgl_destroy_context(ctx);
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
