/* Win32 end-to-end graphics checks; an API existing is not a render pass. */
#define COBJMACROS
#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <d3d11.h>
#include <GL/gl.h>
#include <GL/wglext.h>
#include <GL/glext.h>
#include <stdio.h>
#include <string.h>
static LRESULT CALLBACK window_proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcA(w,m,a,b);}
static HWND window(void){
    WNDCLASSA c={0};c.lpfnWndProc=window_proc;c.hInstance=GetModuleHandleA(NULL);c.lpszClassName="BlesGraphicsTest";
    RegisterClassA(&c);
    return CreateWindowA(c.lpszClassName,"Wine graphics validation",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                          80,60,352,300,NULL,NULL,c.hInstance,NULL);
}

static int pbuffer(HDC dc,int format,HGLRC ctx){
    PFNWGLCREATEPBUFFERARBPROC create=(void *)wglGetProcAddress("wglCreatePbufferARB");
    PFNWGLGETPBUFFERDCARBPROC getdc=(void *)wglGetProcAddress("wglGetPbufferDCARB");
    PFNWGLRELEASEPBUFFERDCARBPROC releasedc=(void *)wglGetProcAddress("wglReleasePbufferDCARB");
    PFNWGLDESTROYPBUFFERARBPROC destroy=(void *)wglGetProcAddress("wglDestroyPbufferARB");
    PFNWGLMAKECONTEXTCURRENTARBPROC make=(void *)wglGetProcAddress("wglMakeContextCurrentARB");
    if(!create || !getdc || !releasedc || !destroy || !make){puts("[COMPAT:WGL] FAIL missing pbuffer/read-context API");return 1;}
    int attrs[]={0};HPBUFFERARB pa=create(dc,format,32,32,attrs),pb=create(dc,format,32,32,attrs);
    HDC da=pa?getdc(pa):NULL,db=pb?getdc(pb):NULL;int result=1;unsigned char rgb[4]={0};
    if(!da || !db || !wglMakeCurrent(da,ctx))goto done;
    glDrawBuffer(GL_FRONT);glReadBuffer(GL_FRONT);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    if(!wglMakeCurrent(db,ctx))goto done;
    glDrawBuffer(GL_FRONT);glReadBuffer(GL_FRONT);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
    if(!wglMakeCurrent(da,ctx))goto done;
    glReadBuffer(GL_FRONT);glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgb);
    int retained=rgb[0]==255 && !rgb[1] && !rgb[2];
    printf("[COMPAT:WGL] %s retained-pbuffer pixel=%u,%u,%u\n",retained?"PASS":"FAIL",rgb[0],rgb[1],rgb[2]);
    if(!make(da,db,ctx))goto done;
    glDrawBuffer(GL_FRONT);glReadBuffer(GL_FRONT);glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgb);
    int read=!rgb[0] && !rgb[1] && rgb[2]==255 && glGetError()==GL_NO_ERROR;
    printf("[COMPAT:WGL] %s independent-read-pbuffer pixel=%u,%u,%u\n",read?"PASS":"FAIL",rgb[0],rgb[1],rgb[2]);
    result=!(retained && read);
 done:
    wglMakeCurrent(dc,ctx);
    if(da)releasedc(pa,da);
    if(db)releasedc(pb,db);
    if(pa)destroy(pa);
    if(pb)destroy(pb);
    if(result)printf("[COMPAT:WGL] FAIL pbuffer error=%lu\n",GetLastError());
    return result;
}
/* A draw before a texture update must retain the old texels, even when the
 * host acknowledges uploads before GPU completion. No intervening glFinish. */
static int texture_order(void){
    GLuint texture;unsigned char red[]={255,0,0,255},green[]={0,255,0,255},pixels[8]={0};
    glDisable(GL_DEPTH_TEST);glColor3f(1,1,1);glEnable(GL_TEXTURE_2D);
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    for(int side=0;side<2;side++){
        if(!side)glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,red);
        else glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,green);
        glViewport(side,0,1,1);glBegin(GL_QUADS);
        glTexCoord2f(0,0);glVertex2f(-1,-1);glVertex2f(1,-1);glVertex2f(1,1);glVertex2f(-1,1);glEnd();
    }
    glReadPixels(0,0,2,1,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    int ok=pixels[0]==255 && !pixels[1] && !pixels[2] && !pixels[4] && pixels[5]==255 && !pixels[6] && glGetError()==GL_NO_ERROR;
    printf("[COMPAT:GL] %s upload-draw-update-order pixels=%u,%u,%u/%u,%u,%u\n",ok?"PASS":"FAIL",pixels[0],pixels[1],pixels[2],pixels[4],pixels[5],pixels[6]);
    glDeleteTextures(1,&texture);glDisable(GL_TEXTURE_2D);return !ok;
}
#define BK_GL_PROC(name) wglGetProcAddress(name)
#include "gl-buffer-reuse.h"
#undef BK_GL_PROC
static int opengl(void){
    HWND w=window();HDC dc=GetDC(w);int result=1;
    PIXELFORMATDESCRIPTOR p={sizeof(p),1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,
                            PFD_TYPE_RGBA,32,0,0,0,0,0,0,8,0,0,0,0,0,16,8,0,PFD_MAIN_PLANE,0,0,0,0,0};
    int format=ChoosePixelFormat(dc,&p);
    if(!format || !SetPixelFormat(dc,format,&p)){printf("[COMPAT:GL] FAIL pixel-format error=%lu\n",GetLastError());goto done;}
    HGLRC ctx=wglCreateContext(dc);
    if(!ctx || !wglMakeCurrent(dc,ctx)){printf("[COMPAT:GL] FAIL context error=%lu\n",GetLastError());
        if(ctx)wglDeleteContext(ctx);
        goto done;}
    result=0;
    GLint depth_bits=0,stencil_bits=0;glGetIntegerv(GL_DEPTH_BITS,&depth_bits);glGetIntegerv(GL_STENCIL_BITS,&stencil_bits);
    if(depth_bits<24 || stencil_bits<8)result=1;
    printf("[COMPAT:GL] depth=%d stencil=%d\n",depth_bits,stencil_bits);
    printf("[COMPAT:GL] version=%s renderer=%s\n",glGetString(GL_VERSION),glGetString(GL_RENDERER));
    const char *extensions=(const char *)glGetString(GL_EXTENSIONS);
    printf("[COMPAT:GL] extension_bytes=%u\n",extensions?(unsigned)strlen(extensions):0);
    glViewport(0,0,256,256);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);
    glBegin(GL_TRIANGLES);glColor3f(0,1,0);glVertex3f(-.8f,-.8f,0);glVertex3f(.8f,-.8f,0);glVertex3f(0,.8f,0);glEnd();
    /* This second triangle is behind the first and must not overwrite it. */
    glBegin(GL_TRIANGLES);glColor3f(1,0,0);glVertex3f(-.8f,-.8f,.5f);glVertex3f(.8f,-.8f,.5f);glVertex3f(0,.8f,.5f);glEnd();
    unsigned char rgb[4]={0};glFinish();glReadPixels(128,100,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgb);
    GLenum error=glGetError();
    result|=!(rgb[1]>200 && rgb[0]<20 && rgb[2]<20 && error==GL_NO_ERROR);
    printf("[COMPAT:GL] %s triangle-depth pixel=%u,%u,%u error=%x\n",result?"FAIL":"PASS",rgb[0],rgb[1],rgb[2],error);
    result|=texture_order();result|=buffer_reuse();SwapBuffers(dc);result|=pbuffer(dc,format,ctx);wglMakeCurrent(NULL,NULL);wglDeleteContext(ctx);
 done:ReleaseDC(w,dc);DestroyWindow(w);return result;
}
static int directdraw(void){
    IDirectDraw7 *dd=NULL;IDirectDrawSurface7 *src=NULL,*dst=NULL;HWND w=window();int result=1;
    HRESULT hr=DirectDrawCreateEx(NULL,(void **)&dd,&IID_IDirectDraw7,NULL);
    if(FAILED(hr)){printf("[COMPAT:DDRAW] FAIL create hr=%lx\n",hr);goto done;}
    hr=IDirectDraw7_SetCooperativeLevel(dd,w,DDSCL_NORMAL);
    if(FAILED(hr))goto error;
    DDSURFACEDESC2 d={0};d.dwSize=sizeof(d);d.dwFlags=DDSD_CAPS|DDSD_WIDTH|DDSD_HEIGHT|DDSD_PIXELFORMAT;
    d.dwWidth=d.dwHeight=256;d.ddsCaps.dwCaps=DDSCAPS_OFFSCREENPLAIN|DDSCAPS_SYSTEMMEMORY;
    d.ddpfPixelFormat.dwSize=sizeof(d.ddpfPixelFormat);d.ddpfPixelFormat.dwFlags=DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount=32;d.ddpfPixelFormat.dwRBitMask=0xff0000;
    d.ddpfPixelFormat.dwGBitMask=0xff00;d.ddpfPixelFormat.dwBBitMask=0xff;
    hr=IDirectDraw7_CreateSurface(dd,&d,&src,NULL);
    if(FAILED(hr))goto error;
    hr=IDirectDraw7_CreateSurface(dd,&d,&dst,NULL);
    if(FAILED(hr))goto error;
    DDSURFACEDESC2 lock={0};lock.dwSize=sizeof(lock);
    hr=IDirectDrawSurface7_Lock(src,NULL,&lock,DDLOCK_WAIT,NULL);
    if(FAILED(hr))goto error;
    for(unsigned y=0;y<256;y++)for(unsigned x=0;x<256;x++)
        ((DWORD *)((BYTE *)lock.lpSurface+y*lock.lPitch))[x]=0x0030b060;
    IDirectDrawSurface7_Unlock(src,NULL);
    hr=IDirectDrawSurface7_Blt(dst,NULL,src,NULL,DDBLT_WAIT,NULL);
    if(FAILED(hr))goto error;
    lock.dwSize=sizeof(lock);hr=IDirectDrawSurface7_Lock(dst,NULL,&lock,DDLOCK_WAIT|DDLOCK_READONLY,NULL);
    if(FAILED(hr))goto error;
    DWORD pixel=*(DWORD *)lock.lpSurface;IDirectDrawSurface7_Unlock(dst,NULL);
    result=pixel!=0x0030b060;
    printf("[COMPAT:DDRAW] %s lock-blit-read pixel=%08lx\n",result?"FAIL":"PASS",pixel);goto done;
 error:printf("[COMPAT:DDRAW] FAIL operation hr=%lx\n",hr);
 done:if(src)IDirectDrawSurface7_Release(src);
    if(dst)IDirectDrawSurface7_Release(dst);
    if(dd)IDirectDraw7_Release(dd);
    DestroyWindow(w);return result;
}

static int read_target(IDirect3DDevice9 *d,DWORD expected,const char *name){
    IDirect3DSurface9 *rt=NULL,*copy=NULL;D3DSURFACE_DESC desc;D3DLOCKED_RECT lock;
    HRESULT hr=IDirect3DDevice9_GetRenderTarget(d,0,&rt);DWORD pixel=0;int ok=0;
    if(SUCCEEDED(hr))hr=IDirect3DSurface9_GetDesc(rt,&desc);
    if(SUCCEEDED(hr))hr=IDirect3DDevice9_CreateOffscreenPlainSurface(d,desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&copy,NULL);
    if(SUCCEEDED(hr))hr=IDirect3DDevice9_GetRenderTargetData(d,rt,copy);
    if(SUCCEEDED(hr))hr=IDirect3DSurface9_LockRect(copy,&lock,NULL,D3DLOCK_READONLY);
    if(SUCCEEDED(hr)){
        pixel=*(DWORD *)((BYTE *)lock.pBits+(desc.Height/2)*lock.Pitch+(desc.Width/2)*4);
        ok=(pixel&0xffffff)==expected;IDirect3DSurface9_UnlockRect(copy);
    }
    printf("[COMPAT:D3D9] %s %s pixel=%08lx expected=%06lx hr=%08lx\n",ok?"PASS":"FAIL",name,pixel,expected,hr);
    if(copy)IDirect3DSurface9_Release(copy);
    if(rt)IDirect3DSurface9_Release(rt);
    return !ok;
}
struct vertex {float x,y,z,rhw;DWORD color;float u,v;};
#define FVF (D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1)
#define DX(call) do{hr=(call);if(FAILED(hr)){printf("[COMPAT:D3D9] FAIL line=%d hr=%08lx\n",__LINE__,hr);goto done;}}while(0)
static int direct3d(void){
    HWND w=window();IDirect3D9 *d3d=Direct3DCreate9(D3D_SDK_VERSION);int result=1;
    if(!d3d){puts("[COMPAT:D3D9] FAIL no-adapter");DestroyWindow(w);return 1;}
    IDirect3DDevice9 *d=NULL;IDirect3DTexture9 *texture=NULL,*render=NULL;
    IDirect3DSurface9 *back=NULL,*rt=NULL;IDirect3DVertexBuffer9 *vb=NULL;IDirect3DIndexBuffer9 *ib=NULL;
    IDirect3DQuery9 *query=NULL;IDirect3DPixelShader9 *ps=NULL;ID3DBlob *code=NULL,*errors=NULL;IDirect3DStateBlock9 *state=NULL;
    HRESULT hr;D3DLOCKED_RECT lock;void *mapped=NULL;
    D3DPRESENT_PARAMETERS p={0};p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;
    p.BackBufferWidth=p.BackBufferHeight=64;p.BackBufferFormat=D3DFMT_X8R8G8B8;p.hDeviceWindow=w;
    p.EnableAutoDepthStencil=TRUE;p.AutoDepthStencilFormat=D3DFMT_D24S8;
    DX(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,w,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&p,&d));
    D3DCAPS9 caps;DX(IDirect3DDevice9_GetDeviceCaps(d,&caps));
    printf("[COMPAT:D3D9] caps vs=%08lx ps=%08lx\n",caps.VertexShaderVersion,caps.PixelShaderVersion);
    DX(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,0xff30b060,1,0));
    if(read_target(d,0x30b060,"clear-readback"))goto done;
    DX(IDirect3DDevice9_CreateTexture(d,2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,NULL));
    DX(IDirect3DTexture9_LockRect(texture,0,&lock,NULL,0));
    for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++)((DWORD *)((BYTE *)lock.pBits+y*lock.Pitch))[x]=0xff00ff00;
    DX(IDirect3DTexture9_UnlockRect(texture,0));
    DX(IDirect3DDevice9_SetRenderState(d,D3DRS_LIGHTING,FALSE));
    DX(IDirect3DDevice9_SetRenderState(d,D3DRS_CULLMODE,D3DCULL_NONE));
    DX(IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,TRUE));
    DX(IDirect3DDevice9_SetRenderState(d,D3DRS_ZFUNC,D3DCMP_LESS));
    DX(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
    DX(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
    DX(IDirect3DDevice9_SetTexture(d,0,(IDirect3DBaseTexture9 *)texture));
    DX(IDirect3DDevice9_SetFVF(d,FVF));
    struct vertex v[4]={{-.5f,-.5f,.25f,1,0xffffffff,0,0},{63.5f,-.5f,.25f,1,0xffffffff,1,0},{-.5f,63.5f,.25f,1,0xffffffff,0,1},{63.5f,63.5f,.25f,1,0xffffffff,1,1}};
    WORD indices[]={0,1,2,2,1,3};
    DX(IDirect3DDevice9_CreateVertexBuffer(d,sizeof(v),D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,FVF,D3DPOOL_DEFAULT,&vb,NULL));
    DX(IDirect3DVertexBuffer9_Lock(vb,0,sizeof(v),&mapped,D3DLOCK_DISCARD));memcpy(mapped,v,sizeof(v));DX(IDirect3DVertexBuffer9_Unlock(vb));
    DX(IDirect3DDevice9_CreateIndexBuffer(d,sizeof(indices),D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&ib,NULL));
    DX(IDirect3DIndexBuffer9_Lock(ib,0,sizeof(indices),&mapped,0));memcpy(mapped,indices,sizeof(indices));DX(IDirect3DIndexBuffer9_Unlock(ib));
    DX(IDirect3DDevice9_SetStreamSource(d,0,vb,0,sizeof(v[0])));DX(IDirect3DDevice9_SetIndices(d,ib));
    DX(IDirect3DDevice9_BeginScene(d));DX(IDirect3DDevice9_DrawIndexedPrimitive(d,D3DPT_TRIANGLELIST,0,0,4,0,2));DX(IDirect3DDevice9_EndScene(d));
    if(read_target(d,0x00ff00,"indexed-texture-depth"))goto done;
    /* A red triangle behind the green quad must fail the depth test. */
    DX(IDirect3DDevice9_SetTexture(d,0,NULL));DX(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLORARG1,D3DTA_DIFFUSE));
    for(unsigned i=0;i<4;i++){v[i].z=.75f;v[i].color=0xffff0000;}
    DX(IDirect3DDevice9_BeginScene(d));DX(IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,v,sizeof(v[0])));DX(IDirect3DDevice9_EndScene(d));
    if(read_target(d,0x00ff00,"depth-reject-behind"))goto done;
    DX(IDirect3DDevice9_CreateStateBlock(d,D3DSBT_ALL,&state));
    DX(IDirect3DDevice9_GetRenderTarget(d,0,&back));
    DX(IDirect3DDevice9_CreateTexture(d,64,64,1,D3DUSAGE_RENDERTARGET,D3DFMT_X8R8G8B8,D3DPOOL_DEFAULT,&render,NULL));
    DX(IDirect3DTexture9_GetSurfaceLevel(render,0,&rt));DX(IDirect3DDevice9_SetRenderTarget(d,0,rt));
    DX(IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,FALSE));
    const char *shader="float4 main() : COLOR { return float4(0,0,1,1); }";
    DX(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"main","ps_2_0",0,0,&code,&errors));
    DX(IDirect3DDevice9_CreatePixelShader(d,(const DWORD *)ID3D10Blob_GetBufferPointer(code),&ps));
    DX(IDirect3DDevice9_SetPixelShader(d,ps));DX(IDirect3DDevice9_BeginScene(d));
    DX(IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,v,sizeof(v[0])));DX(IDirect3DDevice9_EndScene(d));
    if(read_target(d,0x0000ff,"shader-model-2-render-texture"))goto done;
    DX(IDirect3DDevice9_SetRenderTarget(d,0,back));DX(IDirect3DStateBlock9_Apply(state));
    DX(IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,FALSE));
    DX(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
    DX(IDirect3DDevice9_SetTexture(d,0,(IDirect3DBaseTexture9 *)render));
    DX(IDirect3DDevice9_BeginScene(d));DX(IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,v,sizeof(v[0])));DX(IDirect3DDevice9_EndScene(d));
    if(read_target(d,0x0000ff,"sample-render-texture-state-restore"))goto done;
    DX(IDirect3DDevice9_Present(d,NULL,NULL,NULL,NULL));
    /* Reset requires releasing all default-pool resources and state-block references. */
    IDirect3DStateBlock9_Release(state);state=NULL;
    DX(IDirect3DDevice9_SetTexture(d,0,NULL));DX(IDirect3DDevice9_SetStreamSource(d,0,NULL,0,0));DX(IDirect3DDevice9_SetIndices(d,NULL));
    IDirect3DSurface9_Release(back);back=NULL;IDirect3DSurface9_Release(rt);rt=NULL;
    IDirect3DTexture9_Release(render);render=NULL;IDirect3DVertexBuffer9_Release(vb);vb=NULL;IDirect3DIndexBuffer9_Release(ib);ib=NULL;
    p.BackBufferWidth=80;p.BackBufferHeight=48;DX(IDirect3DDevice9_Reset(d,&p));
    DX(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,0xffffff00,1,0));
    if(read_target(d,0xffff00,"reset-resize"))goto done;
    DX(IDirect3DDevice9_Present(d,NULL,NULL,NULL,NULL));
    DX(IDirect3DDevice9_SetRenderState(d,D3DRS_LIGHTING,FALSE));
    DX(IDirect3DDevice9_SetRenderState(d,D3DRS_CULLMODE,D3DCULL_NONE));
    DX(IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,FALSE));
    DX(IDirect3DDevice9_SetFVF(d,FVF));
    DX(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
    DX(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
    DX(IDirect3DDevice9_SetTexture(d,0,(IDirect3DBaseTexture9 *)texture));
    /* The managed texture and the shader object must survive Reset. */
    for(unsigned frame=0;frame<100;frame++){
        DX(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,0xffff0000,1,0));
        DX(IDirect3DDevice9_SetPixelShader(d,(frame&1)?ps:NULL));
        DX(IDirect3DDevice9_BeginScene(d));
        DX(IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,v,sizeof(v[0])));
        DX(IDirect3DDevice9_EndScene(d));
        if(frame<2 || frame>=98)
            if(read_target(d,(frame&1)?0x0000ff:0x00ff00,(frame&1)?"shader-survives-reset":"managed-texture-survives-reset"))goto done;
        DX(IDirect3DDevice9_Present(d,NULL,NULL,NULL,NULL));
    }
    DX(IDirect3DDevice9_CreateQuery(d,D3DQUERYTYPE_EVENT,&query));
    DX(IDirect3DQuery9_Issue(query,D3DISSUE_END));
    DWORD started=GetTickCount();BOOL completed=FALSE;
    do{hr=IDirect3DQuery9_GetData(query,&completed,sizeof(completed),D3DGETDATA_FLUSH);
       if(hr==S_FALSE)Sleep(1);
    }while(hr==S_FALSE && GetTickCount()-started<5000);
    if(hr!=S_OK || !completed){printf("[COMPAT:D3D9] FAIL event-query hr=%08lx\n",hr);goto done;}
    puts("[COMPAT:D3D9] PASS 100 presents and GPU event-query");result=0;
 done:
    if(query)IDirect3DQuery9_Release(query);
    if(state)IDirect3DStateBlock9_Release(state);
    if(errors)ID3D10Blob_Release(errors);
    if(code)ID3D10Blob_Release(code);
    if(ps)IDirect3DPixelShader9_Release(ps);
    if(back)IDirect3DSurface9_Release(back);
    if(rt)IDirect3DSurface9_Release(rt);
    if(render)IDirect3DTexture9_Release(render);
    if(texture)IDirect3DTexture9_Release(texture);
    if(vb)IDirect3DVertexBuffer9_Release(vb);
    if(ib)IDirect3DIndexBuffer9_Release(ib);
    if(d)IDirect3DDevice9_Release(d);
    IDirect3D9_Release(d3d);DestroyWindow(w);return result;
}
#undef DX

static int direct3d11(void){
    ID3D11Device *d=NULL;ID3D11DeviceContext *c=NULL;ID3D11Texture2D *target=NULL,*copy=NULL;
    ID3D11RenderTargetView *rt=NULL;ID3D11VertexShader *vs=NULL;ID3D11PixelShader *ps=NULL;
    ID3D11RasterizerState *raster=NULL;ID3DBlob *vcode=NULL,*pcode=NULL,*errors=NULL;
    D3D_FEATURE_LEVEL requested[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0},level=0;
    HRESULT hr;int result=1;
#define DX11(call) do{hr=(call);if(FAILED(hr)){printf("[COMPAT:D3D11] FAIL line=%d hr=%08lx\n",__LINE__,hr);goto done;}}while(0)
    DX11(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,requested,3,D3D11_SDK_VERSION,&d,&level,&c));
    printf("[COMPAT:D3D11] feature-level=%04x\n",level);
    D3D11_TEXTURE2D_DESC desc={0};desc.Width=desc.Height=32;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    DX11(ID3D11Device_CreateTexture2D(d,&desc,NULL,&target));
    DX11(ID3D11Device_CreateRenderTargetView(d,(ID3D11Resource *)target,NULL,&rt));
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    DX11(ID3D11Device_CreateTexture2D(d,&desc,NULL,&copy));
    const char *vsource="float4 main(uint id : SV_VertexID) : SV_Position { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }";
    const char *psource="float4 main() : SV_Target { return float4(0.25,0.5,0.75,1); }";
    DX11(D3DCompile(vsource,strlen(vsource),NULL,NULL,NULL,"main","vs_4_0",0,0,&vcode,&errors));
    if(errors){ID3D10Blob_Release(errors);errors=NULL;}
    DX11(D3DCompile(psource,strlen(psource),NULL,NULL,NULL,"main","ps_4_0",0,0,&pcode,&errors));
    DX11(ID3D11Device_CreateVertexShader(d,ID3D10Blob_GetBufferPointer(vcode),ID3D10Blob_GetBufferSize(vcode),NULL,&vs));
    DX11(ID3D11Device_CreatePixelShader(d,ID3D10Blob_GetBufferPointer(pcode),ID3D10Blob_GetBufferSize(pcode),NULL,&ps));
    D3D11_RASTERIZER_DESC rd={0};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    DX11(ID3D11Device_CreateRasterizerState(d,&rd,&raster));
    D3D11_VIEWPORT viewport={0,0,32,32,0,1};float red[]={1,0,0,1};
    ID3D11DeviceContext_OMSetRenderTargets(c,1,&rt,NULL);
    ID3D11DeviceContext_ClearRenderTargetView(c,rt,red);
    ID3D11DeviceContext_RSSetState(c,raster);ID3D11DeviceContext_RSSetViewports(c,1,&viewport);
    ID3D11DeviceContext_IASetPrimitiveTopology(c,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c,vs,NULL,0);ID3D11DeviceContext_PSSetShader(c,ps,NULL,0);
    ID3D11DeviceContext_Draw(c,3,0);
    ID3D11DeviceContext_CopyResource(c,(ID3D11Resource *)copy,(ID3D11Resource *)target);
    D3D11_MAPPED_SUBRESOURCE map;DX11(ID3D11DeviceContext_Map(c,(ID3D11Resource *)copy,0,D3D11_MAP_READ,0,&map));
    BYTE *pixel=(BYTE *)map.pData+16*map.RowPitch+16*4;
    result=!(abs(pixel[0]-64)<=1 && abs(pixel[1]-128)<=1 && abs(pixel[2]-191)<=1 && pixel[3]==255);
    printf("[COMPAT:D3D11] %s shader-model-4-triangle pixel=%u,%u,%u,%u\n",result?"FAIL":"PASS",pixel[0],pixel[1],pixel[2],pixel[3]);
    ID3D11DeviceContext_Unmap(c,(ID3D11Resource *)copy,0);
 done:
    if(c){ID3D11DeviceContext_ClearState(c);ID3D11DeviceContext_Flush(c);}
    if(errors)ID3D10Blob_Release(errors);
    if(vcode)ID3D10Blob_Release(vcode);
    if(pcode)ID3D10Blob_Release(pcode);
    if(raster)ID3D11RasterizerState_Release(raster);
    if(vs)ID3D11VertexShader_Release(vs);
    if(ps)ID3D11PixelShader_Release(ps);
    if(rt)ID3D11RenderTargetView_Release(rt);
    if(target)ID3D11Texture2D_Release(target);
    if(copy)ID3D11Texture2D_Release(copy);
    if(c)ID3D11DeviceContext_Release(c);
    if(d)ID3D11Device_Release(d);
    return result;
#undef DX11
}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("[GAMES32] pointer-bits=%u\n",(unsigned)(sizeof(void *)*8));
    int result=direct3d();
    result|=direct3d11();
    result|=opengl();
    result|=directdraw();
    puts(result?"[GAMES32] FAIL":"[GAMES32] ALL PASS");
    return result;
}
