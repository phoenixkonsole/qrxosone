/* The reference: scene.c on the Mac's own OpenGL 2.1 in a real window, read back as the door does.
 *     clang -fobjc-arc host_main.m scene.c -framework Cocoa -framework OpenGL -o scene_host && ./scene_host out.ppm */
#define GL_SILENCE_DEPRECATION
#import <Cocoa/Cocoa.h>
#include <OpenGL/gl.h>
#include "scene.h"
#include <stdlib.h>
#ifdef REF_FBO
GLuint ref_fbo;
#endif

int main(int argc, char **argv)
{
    @autoreleasepool {
        const int w = 640, h = 400;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h) styleMask:NSWindowStyleMaskBorderless
                                                      backing:NSBackingStoreBuffered defer:NO];
        NSOpenGLPixelFormatAttribute at[] = {NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersionLegacy, NSOpenGLPFADoubleBuffer,
                                             NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8, NSOpenGLPFADepthSize, 24,
                                             NSOpenGLPFAStencilSize, 8, NSOpenGLPFAAccelerated, 0};
        NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:at];
        NSOpenGLView *view = [[NSOpenGLView alloc] initWithFrame:NSMakeRect(0, 0, w, h) pixelFormat:pf];
        [view setWantsBestResolutionOpenGLSurface:NO];
        [win setContentView:view];
        [win orderFront:nil];
        [[view openGLContext] setView:view];
        [[view openGLContext] makeCurrentContext];
#ifdef REF_FBO
        GLuint rb[2];
        glGenFramebuffersEXT(1, &ref_fbo);
        glGenRenderbuffersEXT(2, rb);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[0]);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[1]);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, ref_fbo);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb[0]);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb[1]);
        glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
        glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
#endif
        glViewport(0, 0, w, h);
        int fails = scene_draw(w, h);
#ifdef REF_FBO
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, ref_fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
#else
        glReadBuffer(GL_BACK);
#endif
        unsigned char *px = malloc((size_t)w * h * 4), *flip = malloc((size_t)w * h * 4);
        glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, px);
        for (int y = 0; y < h; y++)
            memcpy(flip + (size_t)y * w * 4, px + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
        scene_write_ppm(argc > 1 ? argv[1] : "scene_host.ppm", flip, w, h);
        return fails != 0;
    }
}
