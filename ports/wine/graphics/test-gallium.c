/* SPDX-License-Identifier: MIT
 * Real guest test: GLSL 1.20 -> VirGL GPU -> FBO texture -> CPU readback.
 * Link to the experimental GPU-only Mesa backend. No X server required. */
#define GL_GLEXT_PROTOTYPES
#include "blesmesa.h"
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef void *OSMesaContext;
extern OSMesaContext OSMesaBleskCreateContextAttribs(const int *,OSMesaContext);
extern int OSMesaMakeCurrent(OSMesaContext,void *,unsigned,int,int);
extern int OSMesaBleskSwapBuffers(OSMesaContext);
extern void OSMesaDestroyContext(OSMesaContext);
static GLuint shader(GLenum type,const char *source){
    GLuint s=glCreateShader(type);glShaderSource(s,1,&source,NULL);glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof(log),NULL,log);fprintf(stderr,"shader: %s\n",log);exit(2);}return s;
}
int main(void){
    setbuf(stdout,NULL);setbuf(stderr,NULL);
    puts("[GPU-PROBE] creating GPU context");
    int attrs[]={0x22,GL_RGBA,0x30,16,0x33,0x35,0x36,2,0x37,1,OSMESA_BLES_CONTEXT_FLAGS,OSMESA_BLES_CONTEXT_DEBUG,0};
    OSMesaContext c=OSMesaBleskCreateContextAttribs(attrs,NULL);
    unsigned char screen[16*16*4]={0};
    if(!c || !OSMesaMakeCurrent(c,screen,GL_UNSIGNED_BYTE,16,16)){puts("[GPU-PROBE] context FAILED");return 1;}
    const char *renderer=(const char *)glGetString(GL_RENDERER);
    printf("[GPU-PROBE] renderer=%s version=%s\n",renderer,glGetString(GL_VERSION));
    if(!renderer || !strstr(renderer,"virgl")){puts("[GPU-PROBE] wrong renderer");return 1;}
    GLint flags=0;glGetIntegerv(GL_CONTEXT_FLAGS,&flags);
    if(!(flags&GL_CONTEXT_FLAG_DEBUG_BIT)){puts("[GPU-PROBE] debug flag FAILED");return 1;}
    GLuint tex,fb;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){puts("[GPU-PROBE] FBO FAILED");return 2;}
    GLuint vs=shader(GL_VERTEX_SHADER,"#version 120\nvoid main(){gl_Position=gl_Vertex;}\n");
    GLuint fs=shader(GL_FRAGMENT_SHADER,"#version 120\nvoid main(){gl_FragColor=vec4(0.25,0.5,0.75,1.0);}\n");
    GLuint p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);glLinkProgram(p);
    GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok)return 2;
    glUseProgram(p);glViewport(0,0,16,16);
    glBegin(GL_TRIANGLES);glVertex2f(-1,-1);glVertex2f(3,-1);glVertex2f(-1,3);glEnd();
    unsigned char pixel[4]={0};glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);glFinish();
    GLenum error=glGetError();printf("[GPU-PROBE] pixel=%u,%u,%u,%u error=%x\n",pixel[0],pixel[1],pixel[2],pixel[3],error);
    ok=!error && abs(pixel[0]-64)<=1 && abs(pixel[1]-128)<=1 && abs(pixel[2]-191)<=1 && pixel[3]==255;
    OSMesaContext shared=OSMesaBleskCreateContextAttribs(attrs,c);
    unsigned char second[sizeof(screen)]={0};
    if(!shared || !OSMesaMakeCurrent(shared,second,GL_UNSIGNED_BYTE,16,16) || !glIsTexture(tex)){
        puts("[GPU-PROBE] shared context FAILED");return 4;
    }
    unsigned char image[sizeof(screen)]={0};glBindTexture(GL_TEXTURE_2D,tex);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,image);
    if(abs(image[0]-64)>1 || abs(image[1]-128)>1 || abs(image[2]-191)>1){puts("[GPU-PROBE] shared texture FAILED");return 4;}
    OSMesaMakeCurrent(NULL,NULL,GL_UNSIGNED_BYTE,0,0);OSMesaDestroyContext(shared);
    if(!OSMesaMakeCurrent(c,screen,GL_UNSIGNED_BYTE,16,16))return 4;
    puts("[GPU-PROBE] PASS debug flag and shared GPU texture");
    glDeleteProgram(p);glDeleteShader(vs);glDeleteShader(fs);glDeleteFramebuffers(1,&fb);glDeleteTextures(1,&tex);
    OSMesaMakeCurrent(NULL,NULL,GL_UNSIGNED_BYTE,0,0);OSMesaDestroyContext(c);
    int double_attrs[]={0x22,GL_RGBA,0x30,24,0x31,8,OSMESA_BLES_DOUBLE_BUFFER,1,0};
    c=OSMesaBleskCreateContextAttribs(double_attrs,NULL);
    memset(screen,0,sizeof(screen));
    if(!c || !OSMesaMakeCurrent(c,screen,GL_UNSIGNED_BYTE,16,16))return 5;
    GLboolean double_buffer=0;glGetBooleanv(GL_DOUBLEBUFFER,&double_buffer);
    if(!double_buffer){puts("[GPU-PROBE] double buffer visual FAILED");return 5;}
    GLint depth=0,stencil=0;glGetIntegerv(GL_DEPTH_BITS,&depth);glGetIntegerv(GL_STENCIL_BITS,&stencil);
    glClearDepth(0.25);glClearStencil(5);glClear(GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    float z=0;unsigned char st=0;
    glReadPixels(0,0,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&z);
    glReadPixels(0,0,1,1,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,&st);
    printf("[GPU-PROBE] depth=%d stencil=%d z=%f s=%u error=%x\n",depth,stencil,z,st,glGetError());
    if(depth<24 || stencil<8 || z<0.249 || z>0.251 || st!=5)return 6;
    glDrawBuffer(GL_BACK);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
    glReadBuffer(GL_BACK);glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    printf("[GPU-PROBE] back=%u,%u,%u error=%x\n",pixel[0],pixel[1],pixel[2],glGetError());
    int swapped=OSMesaBleskSwapBuffers(c);
    printf("[GPU-PROBE] first swap=%d pixel=%u,%u,%u error=%x\n",swapped,screen[0],screen[1],screen[2],glGetError());
    if(!swapped || screen[0]!=255 || screen[1]!=0)return 5;
    glReadBuffer(GL_FRONT);glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    printf("[GPU-PROBE] read front=%u,%u,%u error=%x\n",pixel[0],pixel[1],pixel[2],glGetError());
    if(pixel[0]!=255 || pixel[1]!=0)return 5;
    glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    if(pixel[0]!=255 || pixel[1]!=0){puts("[GPU-PROBE] front/back isolation FAILED");return 5;}
    swapped=OSMesaBleskSwapBuffers(c);
    printf("[GPU-PROBE] second swap=%d pixel=%u,%u,%u error=%x\n",swapped,screen[0],screen[1],screen[2],glGetError());
    if(!swapped || screen[0]!=0 || screen[1]!=255)return 5;
    glDrawBuffer(GL_FRONT);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
    error=glGetError();printf("[GPU-PROBE] flush front=%u,%u,%u error=%x\n",screen[0],screen[1],screen[2],error);
    if(screen[0]!=0 || screen[1]!=0 || screen[2]!=255 || error)return 5;
    /* Private CPU staging may be modified while an earlier GPU draw runs.
     * Ordered explicit transfers must preserve that draw and other ranges. */
    const float full[]={-1,-1,3,-1,-1,3}, corner[]={-1,-1,-0.75f,-1,-1,-0.75f};
    const float guard[]={0.125f,0.25f,0.375f,0.5f,0.625f,0.75f};
    GLuint vbo;glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,4096,NULL,GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER,1024,sizeof(guard),guard);
    glEnableClientState(GL_VERTEX_ARRAY);glVertexPointer(2,GL_FLOAT,0,(void *)128);
    glDrawBuffer(GL_BACK);glReadBuffer(GL_BACK);
    for(unsigned i=0;i<32;i++){
        glBufferSubData(GL_ARRAY_BUFFER,128,sizeof(full),full);
        glColor3f(1,0,0);glDrawArrays(GL_TRIANGLES,0,3);glFlush();
        glBufferSubData(GL_ARRAY_BUFFER,128,sizeof(corner),corner);
        glColor3f(0,1,0);glDrawArrays(GL_TRIANGLES,0,3);
    }
    glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    if(pixel[0]!=255 || pixel[1]!=0)return 7;
    const unsigned char *mapped=glMapBuffer(GL_ARRAY_BUFFER,GL_READ_ONLY);
    if(!mapped || memcmp(mapped+128,corner,sizeof(corner)) ||
       memcmp(mapped+1024,guard,sizeof(guard)))return 7;
    if(!glUnmapBuffer(GL_ARRAY_BUFFER) || glGetError())return 7;
    glDisableClientState(GL_VERTEX_ARRAY);glBindBuffer(GL_ARRAY_BUFFER,0);glDeleteBuffers(1,&vbo);
    puts("[GPU-PROBE] PASS 32 asynchronous VBO updates, ordered draws, partial readback and neighbour preservation");
    OSMesaMakeCurrent(NULL,NULL,GL_UNSIGNED_BYTE,0,0);OSMesaDestroyContext(c);
    puts("[GPU-PROBE] PASS distinct front/back, GPU copy-swap and front flush");
    puts(ok?"[GPU-PROBE] PASS real shader and FBO readback":"[GPU-PROBE] FAILED pixels");return ok?0:3;
}
