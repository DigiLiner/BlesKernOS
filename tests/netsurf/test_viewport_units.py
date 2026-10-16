#!/usr/bin/env python3
"""Exercise actual libcss conversions at nonsquare viewport dimensions."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
base=ROOT/'programs/netsurf/vendor'
source=r'''
#include <assert.h>
#include <stdlib.h>
#include <libcss/unit.h>
int main(void){
    css_unit_ctx c={0};
    const int sizes[][2]={{682,287},{562,327},{320,640},{1024,768}};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++){
        int w=sizes[i][0],h=sizes[i][1];
        c.viewport_width=INTTOFIX(w);c.viewport_height=INTTOFIX(h);
        const css_unit units[]={CSS_UNIT_VW,CSS_UNIT_VH,CSS_UNIT_VI,CSS_UNIT_VB,CSS_UNIT_VMIN,CSS_UNIT_VMAX};
        int dimension[]={w,h,w,h,w<h?w:h,w>h?w:h};
        for(unsigned j=0;j<6;j++)for(int n=1;n<=100;n++){
            int actual=FIXTOINT(css_unit_len2css_px(NULL,&c,INTTOFIX(n),units[j]));
            assert(abs(actual-dimension[j]*n/100)<=1);
        }
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'check.c').write_text(source)
    subprocess.run(['gcc','-fsanitize=address,undefined','-fno-pie','-no-pie',
        '-I'+str(base/'libcss/include'),'-I'+str(base/'libcss/src'),'-I'+str(base/'libwapcaplet/include'),
        str(p/'check.c'),str(base/'libcss/src/select/unit.c'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True)
print('PASS: actual libcss vh/vw/vi/vb/vmin/vmax axes and fractional viewport units')
