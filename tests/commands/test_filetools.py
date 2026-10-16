#!/usr/bin/env python3
"""Run real native file tools through POSIX file adapters, with sanitizers."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
harness=(ROOT/'tests/commands/host_adapter.h').read_text()+r'''
int main(void){
 store("source","alpha beta\nGamma\nz",18);
 RUN(0,"touch","source");contents("source","alpha beta\nGamma\nz");
 RUN(0,"touch","-c","missing");assert(access("missing",F_OK));
 RUN(0,"touch","created","created2");assert(!access("created2",F_OK));
 RUN(0,"cp","source","dest");contents("dest","alpha beta\nGamma\nz");
 RUN(2,"cp","-n","created","dest");contents("dest","alpha beta\nGamma\nz");
 RUN(2,"cp","source","./source");contents("source","alpha beta\nGamma\nz");
 store("dest","KEEP",4);fail_after=3;RUN(2,"cp","source","dest");fail_after=-1;contents("dest","KEEP");
 assert(!mkdir("folder",0755));RUN(0,"cp","source","folder");contents("folder/source","alpha beta\nGamma\nz");
 RUN(0,"move","folder/source","moved");assert(access("folder/source",F_OK));
 RUN(0,"wc","source");assert(strstr(output,"2 4 18 source"));
 RUN(0,"head","-n","1","source");assert(!strcmp(output,"alpha beta\n"));
 RUN(0,"head","-n","0","source");assert(!used);
 RUN(0,"tail","-n","1","source");assert(!strcmp(output,"z"));
 RUN(0,"tail","-n","2","source");assert(!strcmp(output,"Gamma\nz"));
 RUN(0,"grep","-in","GAMMA","source");assert(!strcmp(output,"2:Gamma\n"));
 RUN(1,"grep","absent","source");RUN(2,"grep","alpha","missing","source");
 RUN(0,"diff","source","moved");RUN(1,"diff","source","dest");assert(strstr(output,"@@ linea 1 @@"));
 RUN(1,"cmp","source","dest");RUN(0,"cmp","source","moved");
 RUN(2,"rmdir","source");contents("source","alpha beta\nGamma\nz");RUN(0,"rmdir","folder");
 RUN(0,"chmod","640","source");uint16_t mode;assert(bk_file_get_mode("source",&mode)&&mode==0640);
 RUN(2,"chmod","999","source");RUN(0,"nice","5","low");RUN(2,"nice","99","high");
 RUN(0,"stat","source");assert(strstr(output,"0640"));RUN(0,"find","Gamma");
 char *longline=malloc(20000);memset(longline,'x',20000);store("long",longline,20000);free(longline);RUN(0,"cat","long");assert(used==20000);
 char pages[400];unsigned n=0;for(int i=0;i<40;i++){pages[n++]='x';pages[n++]='\n';}store("pages",pages,n);RUN(0,"more","pages");assert(strstr(output,"-- Mas")&&used<200);
 bool ok;assert(command_number("4294967295",&ok)==UINT32_MAX&&ok);command_number("4294967296",&ok);assert(!ok);command_number("0x100000000",&ok);assert(!ok);
 puts("PASS: real streaming tools, preserved touch, staged copies/short writes/read failures, line selection, grep, diff/cmp, paging, permissions, safe rmdir, numeric bounds");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-commands-') as d:
 p=Path(d);(p/'test.c').write_text(harness)
 flags=['gcc','-std=c11','-O1','-g','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-pie','-no-pie','-DTYPES_H','-include','stdint.h','-include','stdbool.h','-include','stddef.h','-I'+str(ROOT/'system/commands'),str(p/'test.c'),str(ROOT/'system/commands/common.c'),'-Wl,--gc-sections','-o',str(p/'test')]
 subprocess.run(flags,check=True);subprocess.run([str(p/'test')],cwd=p,check=True)
