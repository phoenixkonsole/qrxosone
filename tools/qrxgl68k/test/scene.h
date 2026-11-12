#ifndef SCENE_H
#define SCENE_H
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/glew.h>
#endif
int scene_draw(int w, int h);
int scene_write_ppm(const char *path, const unsigned char *argb, int w, int h);
#endif
