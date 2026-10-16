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
