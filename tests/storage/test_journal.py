#!/usr/bin/env python3
"""Power-cut every production BFS4 write; remount and verify metadata/data."""
from pathlib import Path
import argparse, json, shutil, subprocess, tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sector-cuts',action='store_true',help='also tear multi-sector writes after each persisted sector')
args=p.parse_args()
root=Path(__file__).resolve().parents[2]
base=(root/'tests/storage/bfs4_host.c').read_text().split('int main(')[0]
base=base.replace('"../../kernel/','"'+str(root/'kernel')+'/')
base='#include <unistd.h>\n'+base
base=base.replace('static FILE *disk;', 'static FILE *disk; static int budget=-1, writes;')
base=base.replace('    (void)device;\n    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&\n           fwrite', '    (void)device;\n    if(budget==0)_exit(77);\n    if(budget>0)--budget;\n    ++writes;\n    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&\n           fwrite')
if args.sector_cuts:
 start=base.index('static bool host_write(');end=base.index('bool block_read(',start)
 base=base[:start]+r'''
static bool host_write(block_device_t *device,uint32_t lba,uint8_t count,const void *data) {
 (void)device;
 for(unsigned i=0;i<count;i++) {
  if(budget==0)_exit(77);
  if(budget>0)--budget;
  ++writes;
  if(fseek(disk,(long)(lba+i)*512L,SEEK_SET) ||
     fwrite((const char *)data+i*512,512,1,disk)!=1 || fflush(disk))return false;
 }
 return true;
}
'''+base[end:]
code=base+'\n#include "'+str(root/'kernel/drivers/storage/bfs4.c')+'"\n'+r'''
static block_device_t dev;static fat_fs_t fs;
static void check(void) {
 fat_check_report_t r;fat_dir_entry_t e;char b[16];uint32_t n;
 require(bfs4_check(&fs,&r) && !r.errors,"power-cut metadata integrity");
 require(bfs4_resolve(&fs,"/sentinel",&e) && bfs4_read(&fs,&e,0,b,16,&n) &&
  n==9 && !memcmp(b,"unchanged",9),"unrelated file byte integrity");
}
int main(int argc,char **argv) {
 if(argc<3 || !(disk=fopen(argv[1],"r+b")))return 2;
 dev.sector_count=32*1024*1024/512;dev.sector_size=512;dev.read=host_read;dev.write=host_write;
 if(!strcmp(argv[2],"seed")) {
  require(bfs4_format(&dev,"JOURNAL") && bfs4_mount(&fs,&dev,0),"seed mount");
  require(bfs4_write(&fs,"/sentinel","unchanged",9),"sentinel seed");
  require(bfs4_mkdir(&fs,"/d"),"directory seed");
  char a[8192];memset(a,'A',sizeof(a));
  require(bfs4_write(&fs,"/d/file",a,sizeof(a)),"file seed");
  require(bfs4_write(&fs,"/dest","old destination",15),"destination seed");
  require(bfs4_write(&fs,"/spacer",a,sizeof(a)),"defrag spacer");
  require(bfs4_resize(&fs,"/d/file",12288),"fragment seed");
  require(bfs4_sync(&fs),"seed checkpoint");
 } else {
  require(bfs4_mount(&fs,&dev,0),"recover after power cut");check();
  if(strcmp(argv[2],"check")) {
   budget=argc>3?atoi(argv[3]):-1;writes=0;bool ok=false;
   char a[20000];memset(a,'B',sizeof(a));
   if(!strcmp(argv[2],"create"))ok=bfs4_write(&fs,"/new",a,sizeof(a));
   if(!strcmp(argv[2],"extend"))ok=bfs4_write_at(&fs,"/d/file",11000,a,sizeof(a));
   if(!strcmp(argv[2],"shrink"))ok=bfs4_resize(&fs,"/d/file",17);
   if(!strcmp(argv[2],"delete"))ok=bfs4_remove(&fs,"/d/file");
   if(!strcmp(argv[2],"rename"))ok=bfs4_rename(&fs,"/d/file","/dest");
   if(!strcmp(argv[2],"mkdir"))ok=bfs4_mkdir(&fs,"/newdir");
   if(!strcmp(argv[2],"chmod"))ok=bfs4_set_mode(&fs,"/d/file",0600);
   if(!strcmp(argv[2],"times"))ok=bfs4_set_times(&fs,"/d/file",true,123,true,456);
   if(!strcmp(argv[2],"sync"))ok=bfs4_sync(&fs);
   if(!strcmp(argv[2],"defrag")) {
    ok=bfs4_defrag_begin(&fs,42,true);
    while(ok && g_defrag.info.state==BK_DEFRAG_RUNNING)ok=bfs4_defrag_step(&fs,42);
   }
   require(ok,"operation completed");check();printf("%d\n",writes);
  }
 }
 fclose(disk);return 0;
}
'''
results={}
with tempfile.TemporaryDirectory(prefix='bk-journal-',dir='/dev/shm') as directory:
 d=Path(directory);(d/'test.c').write_text(code)
 binary=d/'test';seed=d/'seed.img';image=d/'test.img'
 with seed.open('wb') as f:f.truncate(32*1024*1024)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Wno-stringop-truncation',str(d/'test.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(seed),'seed'],check=True)
 for op in ('create','extend','shrink','delete','rename','mkdir','chmod','times','sync','defrag'):
  shutil.copyfile(seed,image)
  count=int(subprocess.check_output([str(binary),str(image),op],text=True))
  for cut in range(count+1):
   shutil.copyfile(seed,image)
   result=subprocess.run([str(binary),str(image),op,str(cut)],capture_output=True,text=True)
   assert result.returncode in (0,77),(op,cut,result.stderr)
   subprocess.run([str(binary),str(image),'check'],check=True,capture_output=True)
  results[op]=count+1
  print(f'PASS {op}: {count+1} interruption points',flush=True)
 output=root/('tests/storage/evidence/journal-sector-cuts.json' if args.sector_cuts else 'tests/storage/evidence/journal.json');output.parent.mkdir(exist_ok=True)
 output.write_text(json.dumps({'result':'PASS','granularity':'sector' if args.sector_cuts else 'request','cuts':results,'total':sum(results.values())},indent=2)+'\n')
