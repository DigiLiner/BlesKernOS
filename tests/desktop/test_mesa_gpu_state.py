#!/usr/bin/env python3
"""Validate Mesa's actual vertex-color and lightmap GPU state translation."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
mesa=root/'libs/Mesa-3.5'
code=r'''
#include <assert.h>
#include <stdio.h>
#include "bkmesa_gpu.c"
static bk_u32 caps;
bk_u32 gfx3d_capabilities(void) {return caps;}
static void index_texel(const struct gl_texture_image *image, GLint x, GLint y, GLint z, GLvoid *out) {
    (void)image;(void)x;(void)y;(void)z; ((GLchan *)out)[0]=1;
}
int main(void) {
    GLcontext ctx={0}; bkmesa_gpu_context gpu={0}; SWvertex in={0}; bk_vertex out;
    gpu.height=32; ctx.DepthMaxF=65535.0f;
    in.color[0]=255; in.color[3]=255;
    in.win[1]=0.5f;
    convert_vertex(&gpu,&ctx,&out,&in,0,GL_FALSE);
    assert(out.y==31.5f); /* Pixel centers map h-y, without a row shift. */
    bk_vertex offset_vertices[3]={{0,0,.5f,1,0,0,0},{1,0,.5f,1,0,0,0},{0,1,.5f,1,0,0,0}};
    ctx.Polygon._OffsetAny=GL_TRUE;ctx.Polygon.OffsetMRD=-1;
    apply_polygon_offset(&ctx,offset_vertices);
    assert(fabsf(offset_vertices[0].z-(.5f-1.f/65535.f))<1.e-7f);
    ctx.Polygon._OffsetAny=GL_FALSE;ctx.Polygon.OffsetMRD=0;

    ctx.Texture._ReallyEnabled=TEXTURE0_2D;
    ctx.Texture.Unit[0].EnvMode=GL_REPLACE;
    convert_vertex(&gpu,&ctx,&out,&in,0xff00ff00U,GL_TRUE);
    assert(out.color==0xffffffffU);
    ctx.Texture.Unit[0].EnvMode=GL_MODULATE;
    convert_vertex(&gpu,&ctx,&out,&in,0,GL_FALSE);
    assert(out.color==0xffff0000U);
    convert_vertex(&gpu,&ctx,&out,&in,0xff00ff00U,GL_TRUE);
    assert(out.color==0xff00ff00U);
    ctx.Color.BlendEnabled=GL_TRUE; ctx.Color.BlendEquation=GL_FUNC_ADD;
    ctx.Color.BlendSrcRGB=ctx.Color.BlendSrcA=GL_ZERO;
    ctx.Color.BlendDstRGB=ctx.Color.BlendDstA=GL_SRC_COLOR;
    assert(!blend_supported(&ctx,caps));
    caps=BK_CAP_BLEND_MULTIPLY; assert(blend_supported(&ctx,caps));
    ctx.Color.BlendDstA=GL_ONE; assert(!blend_supported(&ctx,caps));
    ctx.Color.BlendSrcRGB=ctx.Color.BlendSrcA=GL_SRC_ALPHA;
    ctx.Color.BlendDstRGB=ctx.Color.BlendDstA=GL_ONE_MINUS_SRC_ALPHA;
    assert(blend_supported(&ctx,caps));
    ctx.Color.BlendSrcRGB=ctx.Color.BlendSrcA=GL_ONE;
    ctx.Color.BlendDstRGB=ctx.Color.BlendDstA=GL_ONE;
    assert(!blend_supported(&ctx,caps));
    caps|=BK_CAP_BLEND_ADDITIVE_ONE;
    assert(blend_supported(&ctx,caps));
    assert((draw_flags(&ctx)&(BK_DRAW_BLEND_ADDITIVE|BK_DRAW_PREMULTIPLIED))==(BK_DRAW_BLEND_ADDITIVE|BK_DRAW_PREMULTIPLIED));
    struct gl_texture_object obj={0}; struct gl_texture_image image={0};
    GLchan rgb[6]={255,0,0, 0,255,32}; GLchan rgba[8]={0,0,0,0, 32,64,255,128};
    image.Format=GL_COLOR_INDEX;image.FetchTexel=index_texel;
    obj.Palette.Format=GL_RGB;obj.Palette.Size=2;obj.Palette.Table=rgb;
    assert(texture_pixel(&ctx,&obj,&image,0,0)==0xff00ff20U);
    obj.Palette.Format=GL_RGBA;obj.Palette.Table=rgba;
    assert(texture_pixel(&ctx,&obj,&image,0,0)==0x802040ffU);
    ctx.Texture.SharedPalette=GL_TRUE;ctx.Texture.Palette.Format=GL_RGB;
    ctx.Texture.Palette.Size=2;ctx.Texture.Palette.Table=rgb;
    assert(texture_pixel(&ctx,&obj,&image,0,0)==0xff00ff20U);
    rgb[3]=200;rgb[4]=16;
    assert(texture_pixel(&ctx,&obj,&image,0,0)==0xffc81020U);
    gpu.ctx=&ctx; image.Data=rgb;obj.Image[0]=&image;
    gpu.textures[0].owner=&obj;gpu.textures[0].image=&image;
    gpu.textures[0].data=image.Data;gpu.textures[0].surface=1;gpu.textures[0].mip_levels=1;
    gpu.textures[0].palette=&ctx.Texture.Palette;
    assert(find_texture(&gpu,&obj,&image)==&gpu.textures[0]);
    ctx.Texture.SharedPalette=GL_FALSE;
    assert(!find_texture(&gpu,&obj,&image));
    gpu.textures[0].palette=&obj.Palette;
    assert(find_texture(&gpu,&obj,&image)==&gpu.textures[0]);
    /* Back-face colors must follow winding, preserve flat provoking vertex,
     * and leave shared SW vertices intact. */
    TNLcontext tnl={0};
    struct gl_client_array back={0};
    GLfloat back_colors[3][4]={{0,1,0,1},{0,0,1,1},{1,1,0,1}};
    SWvertex vertices[3]={0}; GLchan saved[3][4];
    ctx.swtnl_context=&tnl;tnl.vb.ColorPtr[1]=&back;back.Ptr=back_colors;
    vertices[0].win[0]=0;vertices[0].win[1]=0;
    vertices[1].win[0]=0;vertices[1].win[1]=1;
    vertices[2].win[0]=1;vertices[2].win[1]=0;
    vertices[0].color[0]=vertices[1].color[0]=vertices[2].color[0]=255;
    ctx._TriangleCaps=DD_TRI_LIGHT_TWOSIDE;ctx.Polygon._FrontBit=0;
    assert(triangle_colors(&ctx,vertices,0,1,2,saved));
    assert(vertices[0].color[1]==255 && vertices[0].color[0]==0);
    assert(vertices[1].color[2]==255 && vertices[2].color[0]==255 && vertices[2].color[1]==255);
    triangle_colors_restore(vertices,0,1,2,saved);
    assert(vertices[0].color[0]==255 && vertices[0].color[1]==0);
    assert(!triangle_colors(&ctx,vertices,0,2,1,saved));
    ctx.Polygon._FrontBit=1;
    assert(triangle_colors(&ctx,vertices,0,2,1,saved));
    assert(vertices[0].color[1]==255 && vertices[2].color[1]==255 && vertices[1].color[2]==255);
    triangle_colors_restore(vertices,0,2,1,saved);
    assert(triangle_colors(&ctx,vertices,0,0,1,saved));
    triangle_colors_restore(vertices,0,0,1,saved);
    assert(vertices[0].color[0]==255 && vertices[0].color[1]==0 && vertices[1].color[2]==0);
    puts("PASS two-sided lighting winding, provoking vertex and shared vertex preservation");
    puts("PASS indexed texture RGB/RGBA/shared palettes and updates");
    puts("PASS GL_REPLACE ignores primary color; MODULATE/flat colors preserved; lightmap blend gated by capability");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-gpu-state-') as directory:
    c=Path(directory)/'test.c'; c.write_text(code); binary=Path(directory)/'test'
    includes=['include','src','src/math','src/swrast','src/swrast_setup','src/tnl','src/array_cache','src/BlesKernOS']
    subprocess.run(['gcc','-m32','-O2','-DMESA_BLESKERNOS=1','-ffunction-sections','-fdata-sections',
                    *['-I'+str(mesa/p) for p in includes],str(c),'-Wl,--gc-sections','-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
