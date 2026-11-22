#include "common.h"
#include <GL/gl.h>
int main(void){
    HWND wnd=make_window(640,480);
    HDC dc=GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd;memset(&pfd,0,sizeof(pfd));
    pfd.nSize=sizeof(pfd);pfd.nVersion=1;pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;pfd.cColorBits=32;pfd.cDepthBits=24;
    SetPixelFormat(dc,ChoosePixelFormat(dc,&pfd),&pfd);
    HGLRC rc=wglCreateContext(dc);wglMakeCurrent(dc,rc);
    make_checker();
    GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,checker);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glViewport(0,0,640,480);glEnable(GL_DEPTH_TEST);glClearColor(0.1f,0.1f,0.3f,1);
    DWORD t0=timeGetTime();
    for(int f=0;f<TEST_FRAMES;f++){
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);glLoadIdentity();glFrustum(-1,1,-0.75,0.75,1,100);
        glMatrixMode(GL_MODELVIEW);glLoadIdentity();glTranslatef(0,0,-3);glRotatef(f*0.5f,0,1,0);glRotatef(25,1,0,0);
        glEnable(GL_TEXTURE_2D);
        glBegin(GL_QUADS);
        for(int y=0;y<GRID;y++)for(int x=0;x<GRID;x++){
            float x0=(x-GRID/2)*0.05f,y0=(y-GRID/2)*0.05f,x1=x0+0.05f,y1=y0+0.05f;
            glColor3f(1,1,1);
            glTexCoord2f(x*0.1f,y*0.1f);glVertex3f(x0,y0,0);
            glTexCoord2f((x+1)*0.1f,y*0.1f);glVertex3f(x1,y0,0);
            glTexCoord2f((x+1)*0.1f,(y+1)*0.1f);glVertex3f(x1,y1,0);
            glTexCoord2f(x*0.1f,(y+1)*0.1f);glVertex3f(x0,y1,0);
        }
        glEnd();
        glDisable(GL_TEXTURE_2D);
        /* a red triangle in front: the depth test and untextured path */
        glBegin(GL_TRIANGLES);glColor3f(1,0,0);glVertex3f(-0.4f,-0.4f,0.3f);glVertex3f(0.4f,-0.4f,0.3f);glVertex3f(0,0.4f,0.3f);glEnd();
        SwapBuffers(dc);
    }
    report("opengl",t0,timeGetTime());
    return 0;
}
