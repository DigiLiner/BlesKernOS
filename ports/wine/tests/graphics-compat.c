/* Win32 end-to-end graphics checks; an API existing is not a render pass. */
#define COBJMACROS
#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
static LRESULT CALLBACK window_proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcA(w,m,a,b);}
static HWND window(void){
    WNDCLASSA c={0};c.lpfnWndProc=window_proc;c.hInstance=GetModuleHandleA(NULL);c.lpszClassName="BlesGraphicsTest";
    RegisterClassA(&c);
    return CreateWindowA(c.lpszClassName,"Wine graphics validation",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                          80,60,352,300,NULL,NULL,c.hInstance,NULL);
}
static int opengl(void){
    HWND w=window();HDC dc=GetDC(w);int result=1;
    PIXELFORMATDESCRIPTOR p={sizeof(p),1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,
                            PFD_TYPE_RGBA,32,0,0,0,0,0,0,8,0,0,0,0,0,16,8,0,PFD_MAIN_PLANE,0,0,0,0};
    int format=ChoosePixelFormat(dc,&p);
    if(!format || !SetPixelFormat(dc,format,&p)){printf("[COMPAT:GL] FAIL pixel-format error=%lu\n",GetLastError());goto done;}
    HGLRC ctx=wglCreateContext(dc);
    if(!ctx || !wglMakeCurrent(dc,ctx)){printf("[COMPAT:GL] FAIL context error=%lu\n",GetLastError());if(ctx)wglDeleteContext(ctx);goto done;}
    printf("[COMPAT:GL] version=%s renderer=%s\n",glGetString(GL_VERSION),glGetString(GL_RENDERER));
    glViewport(0,0,256,256);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);
    glBegin(GL_TRIANGLES);glColor3f(0,1,0);glVertex3f(-.8f,-.8f,0);glVertex3f(.8f,-.8f,0);glVertex3f(0,.8f,0);glEnd();
    /* This second triangle is behind the first and must not overwrite it. */
    glBegin(GL_TRIANGLES);glColor3f(1,0,0);glVertex3f(-.8f,-.8f,.5f);glVertex3f(.8f,-.8f,.5f);glVertex3f(0,.8f,.5f);glEnd();
    unsigned char rgb[4]={0};glFinish();glReadPixels(128,100,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgb);
    GLenum error=glGetError();
    result=!(rgb[1]>200 && rgb[0]<20 && rgb[2]<20 && error==GL_NO_ERROR);
    printf("[COMPAT:GL] %s triangle-depth pixel=%u,%u,%u error=%x\n",result?"FAIL":"PASS",rgb[0],rgb[1],rgb[2],error);
    SwapBuffers(dc);Sleep(1500);wglMakeCurrent(NULL,NULL);wglDeleteContext(ctx);
 done:ReleaseDC(w,dc);DestroyWindow(w);return result;
}
static int directdraw(void){
    IDirectDraw7 *dd=NULL;IDirectDrawSurface7 *src=NULL,*dst=NULL;HWND w=window();int result=1;
    HRESULT hr=DirectDrawCreateEx(NULL,(void **)&dd,&IID_IDirectDraw7,NULL);
    if(FAILED(hr)){printf("[COMPAT:DDRAW] FAIL create hr=%lx\n",hr);goto done;}
    hr=IDirectDraw7_SetCooperativeLevel(dd,w,DDSCL_NORMAL);if(FAILED(hr))goto error;
    DDSURFACEDESC2 d={0};d.dwSize=sizeof(d);d.dwFlags=DDSD_CAPS|DDSD_WIDTH|DDSD_HEIGHT|DDSD_PIXELFORMAT;
    d.dwWidth=d.dwHeight=256;d.ddsCaps.dwCaps=DDSCAPS_OFFSCREENPLAIN|DDSCAPS_SYSTEMMEMORY;
    d.ddpfPixelFormat.dwSize=sizeof(d.ddpfPixelFormat);d.ddpfPixelFormat.dwFlags=DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount=32;d.ddpfPixelFormat.dwRBitMask=0xff0000;
    d.ddpfPixelFormat.dwGBitMask=0xff00;d.ddpfPixelFormat.dwBBitMask=0xff;
    hr=IDirectDraw7_CreateSurface(dd,&d,&src,NULL);if(FAILED(hr))goto error;
    hr=IDirectDraw7_CreateSurface(dd,&d,&dst,NULL);if(FAILED(hr))goto error;
    DDSURFACEDESC2 lock={0};lock.dwSize=sizeof(lock);
    hr=IDirectDrawSurface7_Lock(src,NULL,&lock,DDLOCK_WAIT,NULL);if(FAILED(hr))goto error;
    for(unsigned y=0;y<256;y++)for(unsigned x=0;x<256;x++)
        ((DWORD *)((BYTE *)lock.lpSurface+y*lock.lPitch))[x]=0x0030b060;
    IDirectDrawSurface7_Unlock(src,NULL);
    hr=IDirectDrawSurface7_Blt(dst,NULL,src,NULL,DDBLT_WAIT,NULL);if(FAILED(hr))goto error;
    lock.dwSize=sizeof(lock);hr=IDirectDrawSurface7_Lock(dst,NULL,&lock,DDLOCK_WAIT|DDLOCK_READONLY,NULL);if(FAILED(hr))goto error;
    DWORD pixel=*(DWORD *)lock.lpSurface;IDirectDrawSurface7_Unlock(dst,NULL);
    result=pixel!=0x0030b060;
    printf("[COMPAT:DDRAW] %s lock-blit-read pixel=%08lx\n",result?"FAIL":"PASS",pixel);goto done;
 error:printf("[COMPAT:DDRAW] FAIL operation hr=%lx\n",hr);
 done:if(src)IDirectDrawSurface7_Release(src);if(dst)IDirectDrawSurface7_Release(dst);if(dd)IDirectDraw7_Release(dd);DestroyWindow(w);return result;
}
static int direct3d(void){
    HWND w=window();IDirect3D9 *d3d=Direct3DCreate9(D3D_SDK_VERSION);int result=1;
    if(!d3d){printf("[COMPAT:D3D9] FAIL no-adapter\n");DestroyWindow(w);return 1;}
    IDirect3DDevice9 *device=NULL;D3DPRESENT_PARAMETERS p={0};p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;
    p.BackBufferWidth=p.BackBufferHeight=256;p.BackBufferFormat=D3DFMT_X8R8G8B8;p.hDeviceWindow=w;
    HRESULT hr=IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,w,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&p,&device);
    if(FAILED(hr)){printf("[COMPAT:D3D9] FAIL create-device hr=%lx\n",hr);goto done;}
    hr=IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0xff30b060,1,0);if(FAILED(hr))goto done;
    IDirect3DSurface9 *target=NULL,*copy=NULL;
    hr=IDirect3DDevice9_GetRenderTarget(device,0,&target);if(FAILED(hr))goto done;
    hr=IDirect3DDevice9_CreateOffscreenPlainSurface(device,256,256,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&copy,NULL);
    if(SUCCEEDED(hr))hr=IDirect3DDevice9_GetRenderTargetData(device,target,copy);
    if(SUCCEEDED(hr)){
        D3DLOCKED_RECT lock;hr=IDirect3DSurface9_LockRect(copy,&lock,NULL,D3DLOCK_READONLY);
        if(SUCCEEDED(hr)){DWORD pixel=*(DWORD *)lock.pBits;result=(pixel&0xffffff)!=0x30b060;
            printf("[COMPAT:D3D9] %s clear-readback pixel=%08lx\n",result?"FAIL":"PASS",pixel);
            IDirect3DSurface9_UnlockRect(copy);}
    }
    if(FAILED(hr))printf("[COMPAT:D3D9] FAIL readback hr=%lx\n",hr);
    if(copy)IDirect3DSurface9_Release(copy);IDirect3DSurface9_Release(target);
    IDirect3DDevice9_Present(device,NULL,NULL,NULL,NULL);Sleep(1500);
 done:if(device)IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(w);return result;
}
int main(int argc,char **argv){
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc<2)return 2;
    if(!strcmp(argv[1],"gl"))return opengl();
    if(!strcmp(argv[1],"ddraw"))return directdraw();
    if(!strcmp(argv[1],"d3d9"))return direct3d();
    return 2;
}
