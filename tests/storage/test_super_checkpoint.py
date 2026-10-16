#!/usr/bin/env python3
"""Unchanged superblocks avoid redundant durable writes without caching recovery state."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
base=(root/'tests/storage/bfs4_host.c').read_text().split('int main(')[0]
base=base.replace('"../../kernel/','"'+str(root/'kernel')+'/')
code=base+'\n#include "'+str(root/'kernel/drivers/storage/bfs4.c')+'"\n'+r'''
static unsigned writes;
static bool counted_write(block_device_t *d,uint32_t lba,uint8_t count,const void *data){writes++;return host_write(d,lba,count,data);}
int main(int argc,char **argv){
 if(argc!=2 || !(disk=fopen(argv[1],"r+b")))return 2;
 block_device_t dev={0};dev.sector_count=32*1024*1024/512;dev.sector_size=512;dev.read=host_read;dev.write=counted_write;
 fat_fs_t fs;require(bfs4_format(&dev,"CHECKPOINT") && bfs4_mount(&fs,&dev,0),"format/mount");
 bfs4_volume_t *v=bfs4_volume(&fs);require(bfs4_write_super(v),"initial checkpoint");
 writes=0;require(bfs4_write_super(v) && !writes,"identical superblock skips disk writes");
 v->super.generation++;require(bfs4_write_super(v) && writes,"changed checkpoint persists");
 writes=0;require(bfs4_write_super(v) && !writes,"new checkpoint cached");
 require(bfs4_journal_reload(v),"recovery reload");writes=0;
 require(bfs4_write_super(v) && writes,"recovery invalidates checkpoint cache");
 require(bfs4_unmount(&fs) && bfs4_mount(&fs,&dev,0),"remount");
 writes=0;require(bfs4_write_super(bfs4_volume(&fs)) && writes,"mount does not inherit stale checkpoint");
 fclose(disk);puts("PASS exact superblock checkpoint reuse, changed checkpoint, recovery invalidation and remount");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-super-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 with (p/'disk.img').open('wb') as f:f.truncate(32*1024*1024)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Wno-stringop-truncation',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'disk.img')],check=True)
