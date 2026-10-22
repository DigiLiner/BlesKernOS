#!/usr/bin/env python3
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2];out=ROOT/'build/tests/lbs';out.mkdir(parents=True,exist_ok=True)
def obj(name,source):
 p=out/(name+'.c');p.write_text(source)
 subprocess.run(['gcc','-m32','-ffreestanding','-fPIC','-fno-stack-protector','-c',str(p),'-o',str(out/(name+'.o'))],check=True)
def lib(name,*inputs):
 subprocess.run(['ld','-m','elf_i386','-shared','--hash-style=sysv','-soname',name+'.LBS','-o',str(out/(name+'.LBS'))]+[str(out/x) for x in inputs],check=True)
obj('fixture','static int state;static int *counter;__attribute__((constructor)) static void init(void){state=7;} __attribute__((destructor)) static void fini(void){if(counter)(*counter)++;} int lbs_fixture_get(void){return state;} void lbs_fixture_set(int n){state=n;} void lbs_fixture_counter(int *p){counter=p;}')
lib('FIXDEP','fixture.o')
obj('root','extern int lbs_fixture_get(void);int lbs_fixture_answer(void){return lbs_fixture_get()+35;}');lib('FIXROOT','root.o','FIXDEP.LBS')
obj('a','int fixture_a(void){return 1;}');obj('b','int fixture_b(void){return 2;}')
lib('CYCLEA','a.o');lib('CYCLEB','b.o','CYCLEA.LBS');lib('CYCLEA','a.o','CYCLEB.LBS')
# Missing transitive dependency, preserving the root library's DT_NEEDED.
obj('missing','int lbs_missing_value(void){return 0;}');lib('MISSINGDEP','missing.o')
obj('need','extern int lbs_missing_value(void);int lbs_need(void){return lbs_missing_value();}');lib('MISSINGROOT','need.o','MISSINGDEP.LBS');(out/'MISSINGDEP.LBS').unlink()
print('Built loader lifecycle/negative fixtures')

from PIL import Image
Image.new('RGB',(8,8),(220,30,70)).save(out/'FRAME.GIF',format='GIF')
(out/'CORRUPT.BAD').write_bytes(b'not an ELF library')

obj('share','__attribute__((aligned(4096))) const unsigned char share_constant[4096]={42}; int share_state=17; int share_get(void){return share_state;}')
lib('FIXSHARE','share.o')
