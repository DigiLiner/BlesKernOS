/* Shared native GLX / Wine WGL GPU ordering regression. Define BK_GL_PROC. */
/* Exercise reusing host vertex storage while earlier draws are in flight.
 * The final readback must not let later vertex colours replace earlier ones. */
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
    enum {COLS=32,ROWS=32};
    GLuint buffer;unsigned char pixels[COLS*ROWS*4];unsigned bad=0;
    GLfloat vertices[4][5]={{-1,-1,0,0,0},{1,-1,0,0,0},{1,1,0,0,0},{-1,1,0,0,0}};
    glDisable(GL_TEXTURE_2D);glDisable(GL_DEPTH_TEST);glDisable(GL_DITHER);
    gen(1,&buffer);bind(GL_ARRAY_BUFFER,buffer);
    data(GL_ARRAY_BUFFER,65536,NULL,GL_DYNAMIC_DRAW);
    glVertexPointer(2,GL_FLOAT,sizeof(vertices[0]),(void *)0);
    glColorPointer(3,GL_FLOAT,sizeof(vertices[0]),(void *)(2*sizeof(GLfloat)));
    glEnableClientState(GL_VERTEX_ARRAY);glEnableClientState(GL_COLOR_ARRAY);
    for(unsigned i=0;i<COLS*ROWS;i++){
        for(unsigned v=0;v<4;v++){
            vertices[v][2]=(i&1)?0:1;vertices[v][3]=(i&1)?1:0;
        }
        sub(GL_ARRAY_BUFFER,0,sizeof(vertices),vertices);
        glViewport(i%COLS,i/COLS,1,1);glDrawArrays(GL_QUADS,0,4);
        /* Submit the draw, without waiting for it to finish. */
        glFlush();
    }
    glReadPixels(0,0,COLS,ROWS,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    for(unsigned i=0;i<COLS*ROWS;i++)
        if(pixels[i*4]!=((i&1)?0:255) || pixels[i*4+1]!=((i&1)?255:0) || pixels[i*4+2])bad++;
    GLenum error=glGetError();
    glDisableClientState(GL_VERTEX_ARRAY);glDisableClientState(GL_COLOR_ARRAY);
    bind(GL_ARRAY_BUFFER,0);del(1,&buffer);
    printf("[COMPAT:GL] %s buffer-reuse-order bad_pixels=%u/%u error=%x\n",!bad && !error?"PASS":"FAIL",bad,COLS*ROWS,error);
    return bad || error;
}
