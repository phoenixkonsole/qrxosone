/* scene.c as a 68k program through the GL door, without a window: the frame goes into a file.
 * usage: scene_guest out.ppm */
#include "scene.h"
#include "../guest/qgl_api.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const int w = 640, h = 400;
    if (!qgl_available()) {
        printf("no GL door in this engine\n");
        return 20;
    }
    unsigned ctx = qgl_create_context(0);
    if (!ctx || !qgl_make_current(ctx, w, h)) {
        printf("no context\n");
        return 20;
    }
    GLenum e = glewInit();
    printf("glewInit %u, GL 2.1 %d, FBO %d\n", e, GLEW_VERSION_2_1, GLEW_EXT_framebuffer_object);
    int fails = scene_draw(w, h);
    unsigned char *px = malloc((size_t)w * h * 4);
    qgl_read_frame(px, w * 4, w, h);
    scene_write_ppm(argc > 1 ? argv[1] : "scene_guest.ppm", px, w, h);
    qgl_destroy_context(ctx);
    return fails ? 10 : 0;
}
