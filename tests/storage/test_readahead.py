#!/usr/bin/env python3
"""Exercise production BFS4 read-ahead, bounds, coherence and failure fallback."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
base=(root/'tests/storage/bfs4_host.c').read_text().split('int main(')[0]
base=base.replace('"../../kernel/','"'+str(root/'kernel')+'/')
base=base.replace('void *kmalloc(size_t n) { return malloc(n); }','static bool fail_alloc;\nvoid *kmalloc(size_t n) { return fail_alloc?NULL:malloc(n); }')
base=base.replace('uint32_t mm_total_bytes(void) { return 512U * 1024U * 1024U; }','static uint32_t ram=512U*1024U*1024U;\nuint32_t mm_total_bytes(void) { return ram; }')
code=base+'\n#include "'+str(root/'kernel/drivers/storage/bfs4.c')+'"\n'+r'''
static unsigned reads,max_count;
static bool fail_long,fail_all;
static bool counted_read(block_device_t *d,uint32_t lba,uint8_t count,void *data){
 reads++;if(count>max_count)max_count=count;
 if(fail_all || (fail_long && count>8))return false;
 return host_read(d,lba,count,data);
}
static void cold(void){memset(g_cache,0,g_cache_slots*sizeof(*g_cache));reads=max_count=0;}
int main(int argc,char **argv){
 if(argc!=2 || !(disk=fopen(argv[1],"r+b")))return 2;
 block_device_t dev={0};dev.sector_count=32*1024*1024/512;dev.sector_size=512;
 dev.read=counted_read;dev.write=host_write;
 fat_fs_t fs;fat_dir_entry_t e;uint32_t got;
 require(bfs4_format(&dev,"READAHEAD") && bfs4_mount(&fs,&dev,0),"format/mount");
 unsigned char *data=malloc(64*4096+17),page[4096+32];
 for(unsigned i=0;i<64*4096+17;i++)data[i]=(i/4096*37+i%251)&255;
 require(bfs4_write(&fs,"/pages",data,64*4096+17) && bfs4_resolve(&fs,"/pages",&e),"write fixture");
 cold();
 for(unsigned i=0;i<64;i++){
  memset(page,0xa5,sizeof page);
  require(bfs4_read(&fs,&e,i*4096,page+16,4096,&got) && got==4096 && !memcmp(page+16,data+i*4096,4096),"page bytes");
  for(unsigned j=0;j<16;j++)require(page[j]==0xa5 && page[4112+j]==0xa5,"caller bounds");
 }
 require(reads==4 && max_count==128,"64 demand pages need four disk commands");
 memset(page,0xa5,sizeof page);
 require(bfs4_read(&fs,&e,64*4096,page+16,4096,&got) && got==17 && !memcmp(page+16,data+64*4096,17),"partial EOF");
 require(reads==5 && page[33]==0xa5,"no speculative partial final page exposed");
 cold();fail_long=true;
 require(bfs4_read(&fs,&e,0,page,4096,&got) && got==4096 && !memcmp(page,data,4096) && reads==2,"failed speculative read falls back");
 fail_long=false;cold();fail_all=true;
 require(!bfs4_read(&fs,&e,0,page,4096,&got),"requested read failure is reported");
 fail_all=false;reads=0;
 require(bfs4_read(&fs,&e,0,page,4096,&got) && reads==1,"failed data never cached");
 free(g_read_ahead);g_read_ahead=NULL;cold();fail_alloc=true;
 require(bfs4_read(&fs,&e,0,page,4096,&got) && got==4096 && max_count==8,"allocation failure fallback");
 fail_alloc=false;cold();ram=64U*1024U*1024U;
 require(bfs4_read(&fs,&e,0,page,4096,&got) && max_count==8,"low memory fallback");
 ram=512U*1024U*1024U;
 memset(data,0x63,64*4096+17);
 require(bfs4_write(&fs,"/pages",data,64*4096+17),"overwrite prefetched data");
 require(bfs4_read(&fs,&e,7*4096,page,4096,&got) && !memcmp(page,data+7*4096,4096),"overwrite invalidates prefetched blocks");
 require(bfs4_remove(&fs,"/pages") && bfs4_write(&fs,"/pages",data,4096) && bfs4_resolve(&fs,"/pages",&e),"unlink/recreate");
 cold();require(bfs4_read(&fs,&e,0,page,4096,&got) && got==4096 && max_count==8,"single block EOF");
 /* Interleave allocations so the read-ahead cannot cross into another file. */
 for(unsigned i=1;i<8;i++){
  char other[32];sprintf(other,"/other%u",i);
  require(bfs4_write(&fs,other,data,4096),"fragment separator");
  require(bfs4_append(&fs,"/pages",data,i*4096,(i+1)*4096),"fragmented append");
 }
 cold();
 for(unsigned i=0;i<8;i++)require(bfs4_read(&fs,&e,i*4096,page,4096,&got) && got==4096 && !memcmp(page,data,4096),"fragmented bytes");
 require(max_count==8,"no prefetch across discontiguous extents");
 fat_check_report_t report;require(bfs4_check(&fs,&report) && !report.errors,"filesystem integrity");
 require(bfs4_unmount(&fs) && bfs4_mount(&fs,&dev,0),"remount");
 free(data);free(g_read_ahead);fclose(disk);
 puts("PASS BFS4 read-ahead: 64 pages/4 commands, bounds/EOF, fragmented extents, overwrite/unlink, failed I/O/OOM/low RAM fallback and remount");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-bfs4-readahead-') as folder:
 d=Path(folder);(d/'test.c').write_text(code)
 with (d/'disk.img').open('wb') as f:f.truncate(32*1024*1024)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Wextra','-Wno-stringop-truncation',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test'),str(d/'disk.img')],check=True)
