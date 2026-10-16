#define COBJMACROS
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
static LRESULT CALLBACK window_proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcA(w,m,a,b);}
static HWND window(void){
    WNDCLASSA c={0};c.lpfnWndProc=window_proc;c.hInstance=GetModuleHandleA(NULL);c.lpszClassName="BlesGraphicsTest";
    RegisterClassA(&c);
    return CreateWindowA(c.lpszClassName,"Wine graphics validation",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                          80,60,352,300,NULL,NULL,c.hInstance,NULL);
}

/* FIFA 2000 uses the legacy Direct3D path. Keep hundreds of textured
 * client-memory draws batched, then validate every target pixel. */
int test_direct3d7(void){
    IDirectDraw7 *dd=NULL;IDirect3D7 *d3d=NULL;IDirect3DDevice7 *device=NULL;
    IDirectDrawSurface7 *target=NULL,*textures[2]={NULL,NULL};
    HWND w=window();int result=1;HRESULT hr;
#define DX7(call) do{hr=(call);if(FAILED(hr)){printf("[COMPAT:D3D7] FAIL line=%u hr=%08lx\n",__LINE__,hr);goto done;}}while(0)
    DX7(DirectDrawCreateEx(NULL,(void **)&dd,&IID_IDirectDraw7,NULL));
    DX7(IDirectDraw7_SetCooperativeLevel(dd,w,DDSCL_NORMAL));
    DX7(IDirectDraw7_QueryInterface(dd,&IID_IDirect3D7,(void **)&d3d));
    DDSURFACEDESC2 desc={0};desc.dwSize=sizeof(desc);
    desc.dwFlags=DDSD_CAPS|DDSD_WIDTH|DDSD_HEIGHT|DDSD_PIXELFORMAT;
    desc.dwWidth=desc.dwHeight=32;desc.ddsCaps.dwCaps=DDSCAPS_OFFSCREENPLAIN|DDSCAPS_3DDEVICE;
    desc.ddpfPixelFormat.dwSize=sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags=DDPF_RGB;desc.ddpfPixelFormat.dwRGBBitCount=32;
    desc.ddpfPixelFormat.dwRBitMask=0xff0000;desc.ddpfPixelFormat.dwGBitMask=0xff00;desc.ddpfPixelFormat.dwBBitMask=0xff;
    DX7(IDirectDraw7_CreateSurface(dd,&desc,&target,NULL));
    DX7(IDirect3D7_CreateDevice(d3d,&IID_IDirect3DHALDevice,target,&device));
    desc.dwWidth=desc.dwHeight=16;desc.ddsCaps.dwCaps=DDSCAPS_TEXTURE;
    for(unsigned t=0;t<2;t++){
        DX7(IDirectDraw7_CreateSurface(dd,&desc,&textures[t],NULL));
        DDSURFACEDESC2 lock={0};lock.dwSize=sizeof(lock);
        DX7(IDirectDrawSurface7_Lock(textures[t],NULL,&lock,DDLOCK_WAIT,NULL));
        for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++)
            ((DWORD *)((BYTE *)lock.lpSurface+y*lock.lPitch))[x]=t?0x0000ff00:0x00ff0000;
        DX7(IDirectDrawSurface7_Unlock(textures[t],NULL));
    }
    D3DVIEWPORT7 viewport={0,0,32,32,0,1};
    DX7(IDirect3DDevice7_SetViewport(device,&viewport));
    DX7(IDirect3DDevice7_SetRenderState(device,D3DRENDERSTATE_ZENABLE,FALSE));
    DX7(IDirect3DDevice7_SetRenderState(device,D3DRENDERSTATE_LIGHTING,FALSE));
    DX7(IDirect3DDevice7_SetRenderState(device,D3DRENDERSTATE_CULLMODE,D3DCULL_NONE));
    DX7(IDirect3DDevice7_SetTextureStageState(device,0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
    DX7(IDirect3DDevice7_SetTextureStageState(device,0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
    DX7(IDirect3DDevice7_SetTextureStageState(device,0,D3DTSS_ALPHAOP,D3DTOP_DISABLE));
    for(unsigned frame=0;frame<4;frame++){
        DX7(IDirect3DDevice7_BeginScene(device));
        for(unsigned i=0;i<1024;i++){
            float x=(i%32)-.5f,y=(i/32)-.5f;
            struct {float x,y,z,rhw;DWORD colour;float u,v;} vertices[4]={
                {x,y,.5f,1,0xffffffff,0,0},{x+1,y,.5f,1,0xffffffff,1,0},
                {x,y+1,.5f,1,0xffffffff,0,1},{x+1,y+1,.5f,1,0xffffffff,1,1}};
            DX7(IDirect3DDevice7_SetTexture(device,0,textures[(i^frame)&1]));
            DX7(IDirect3DDevice7_DrawPrimitive(device,D3DPT_TRIANGLESTRIP,
                D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1,vertices,4,0));
        }
        DX7(IDirect3DDevice7_EndScene(device));
    }
    DDSURFACEDESC2 lock={0};lock.dwSize=sizeof(lock);
    DX7(IDirectDrawSurface7_Lock(target,NULL,&lock,DDLOCK_WAIT|DDLOCK_READONLY,NULL));
    unsigned bad=0;
    for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++){
        DWORD pixel=((DWORD *)((BYTE *)lock.lpSurface+y*lock.lPitch))[x]&0xffffff;
        if(pixel!=(((x^3)&1)?0x0000ff00:0x00ff0000))bad++;
    }
    DX7(IDirectDrawSurface7_Unlock(target,NULL));result=bad!=0;
    printf("[COMPAT:D3D7] %s batched-textured-quads draws=4096 bad_pixels=%u/1024\n",result?"FAIL":"PASS",bad);
 done:
    if(device)IDirect3DDevice7_Release(device);
    for(unsigned t=0;t<2;t++)if(textures[t])IDirectDrawSurface7_Release(textures[t]);
    if(target)IDirectDrawSurface7_Release(target);
    if(d3d)IDirect3D7_Release(d3d);
    if(dd)IDirectDraw7_Release(dd);
    DestroyWindow(w);return result;
#undef DX7
}
