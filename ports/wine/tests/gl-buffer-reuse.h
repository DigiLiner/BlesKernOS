/* Shared native GLX / Wine WGL GPU ordering regression. Define BK_GL_PROC. */
/* Exercise reusing host vertex storage while earlier draws are in flight.
 * The final readback must not let later vertex colours replace earlier ones. */
#include <string.h>
static int buffer_reuse(void){
    __typeof__(&glDisable) glDisable=(void *)BK_GL_PROC("glDisable");
    __typeof__(&glVertexPointer) glVertexPointer=(void *)BK_GL_PROC("glVertexPointer");
    __typeof__(&glColorPointer) glColorPointer=(void *)BK_GL_PROC("glColorPointer");
    __typeof__(&glEnableClientState) glEnableClientState=(void *)BK_GL_PROC("glEnableClientState");
    __typeof__(&glViewport) glViewport=(void *)BK_GL_PROC("glViewport");
    __typeof__(&glDrawArrays) glDrawArrays=(void *)BK_GL_PROC("glDrawArrays");
    __typeof__(&glFlush) glFlush=(void *)BK_GL_PROC("glFlush");
    __typeof__(&glReadPixels) glReadPixels=(void *)BK_GL_PROC("glReadPixels");
    __typeof__(&glGetError) glGetError=(void *)BK_GL_PROC("glGetError");
    __typeof__(&glDisableClientState) glDisableClientState=(void *)BK_GL_PROC("glDisableClientState");
    if(!glDisable || !glVertexPointer || !glColorPointer || !glEnableClientState || !glViewport || !glDrawArrays || !glFlush || !glReadPixels || !glGetError || !glDisableClientState){puts("[COMPAT:GL] FAIL buffer-reuse core API");return 1;}
    PFNGLGENBUFFERSPROC gen=(void *)BK_GL_PROC("glGenBuffers");
    PFNGLBINDBUFFERPROC bind=(void *)BK_GL_PROC("glBindBuffer");
    PFNGLBUFFERDATAPROC data=(void *)BK_GL_PROC("glBufferData");
    PFNGLBUFFERSUBDATAPROC sub=(void *)BK_GL_PROC("glBufferSubData");
    PFNGLDELETEBUFFERSPROC del=(void *)BK_GL_PROC("glDeleteBuffers");
    if(!gen || !bind || !data || !sub || !del){puts("[COMPAT:GL] FAIL buffer-reuse API");return 1;}
    PFNGLMAPBUFFERRANGEPROC map=(void *)BK_GL_PROC("glMapBufferRange");
    PFNGLUNMAPBUFFERPROC unmap=(void *)BK_GL_PROC("glUnmapBuffer");
    if(!map || !unmap){puts("[COMPAT:GL] FAIL discard-map API");return 1;}
    enum {COLS=32,ROWS=32};
    GLuint buffers[257];unsigned nbuf=1,total_bad=0;unsigned char pixels[COLS*ROWS*4];
    GLfloat vertices[4][5]={{-1,-1,0,0,0},{1,-1,0,0,0},{1,1,0,0,0},{-1,1,0,0,0}};
    glDisable(GL_TEXTURE_2D);glDisable(GL_DEPTH_TEST);glDisable(GL_DITHER);
    gen(1,buffers);bind(GL_ARRAY_BUFFER,buffers[0]);
    data(GL_ARRAY_BUFFER,65536,NULL,GL_DYNAMIC_DRAW);
    glEnableClientState(GL_VERTEX_ARRAY);glEnableClientState(GL_COLOR_ARRAY);
    for(unsigned mode=0;mode<3;mode++){
        unsigned rows=mode?8:ROWS,bad=0,failed=0;
        bind(GL_ARRAY_BUFFER,buffers[0]);
        glVertexPointer(2,GL_FLOAT,sizeof(vertices[0]),(void *)0);
        glColorPointer(3,GL_FLOAT,sizeof(vertices[0]),(void *)(2*sizeof(GLfloat)));
        for(unsigned i=0;i<COLS*rows;i++){
            for(unsigned v=0;v<4;v++){
                vertices[v][2]=(i&1)?0:1;vertices[v][3]=(i&1)?1:0;
            }
            if(mode==1){
                /* Keep every resource alive until readback: destruction must
                 * not accidentally fence these first uploads. */
                gen(1,&buffers[nbuf]);bind(GL_ARRAY_BUFFER,buffers[nbuf++]);
                data(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_DYNAMIC_DRAW);
                glVertexPointer(2,GL_FLOAT,sizeof(vertices[0]),(void *)0);
                glColorPointer(3,GL_FLOAT,sizeof(vertices[0]),(void *)(2*sizeof(GLfloat)));
            }else if(mode==2){
                void *ptr=map(GL_ARRAY_BUFFER,0,sizeof(vertices),GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_BUFFER_BIT);
                if(!ptr){puts("[COMPAT:GL] FAIL discard-map null");failed=1;break;}
                memcpy(ptr,vertices,sizeof(vertices));
                if(!unmap(GL_ARRAY_BUFFER)){failed=1;break;}
            }else sub(GL_ARRAY_BUFFER,0,sizeof(vertices),vertices);
            glViewport(i%COLS,i/COLS,1,1);glDrawArrays(GL_QUADS,0,4);
            /* The discard case deliberately leaves draws batched. */
            if(mode!=2)glFlush();
        }
        glReadPixels(0,0,COLS,rows,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        for(unsigned i=0;i<COLS*rows;i++)
            if(pixels[i*4]!=((i&1)?0:255) || pixels[i*4+1]!=((i&1)?255:0) || pixels[i*4+2])bad++;
        GLenum error=glGetError();total_bad+=bad+(error!=GL_NO_ERROR)+failed;
        const char *name=mode==0?"buffer-reuse-order":mode==1?"fresh-buffer-order":"discard-buffer-order";
        printf("[COMPAT:GL] %s %s bad_pixels=%u/%u error=%x\n",!bad && !error && !failed?"PASS":"FAIL",name,bad,COLS*rows,error);
    }
    glDisableClientState(GL_VERTEX_ARRAY);glDisableClientState(GL_COLOR_ARRAY);
    bind(GL_ARRAY_BUFFER,0);del(nbuf,buffers);
    return total_bad!=0;
}
