#!/usr/bin/env python3
"""Crash/checkpoint regressions using the production BFS4 driver on temp disks."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
base=(root/'tests/storage/bfs4_host.c').read_text().split('int main(')[0]
base=base.replace('"../../kernel/','"'+str(root/'kernel')+'/')
base=base.replace('static FILE *disk;', 'static FILE *disk; static bool fail_write;')
base=base.replace('    (void)device;\n    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&\n           fwrite', '    (void)device;\n    if(fail_write) {fail_write=false;return false;}\n    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&\n           fwrite')
code=base+'\n#include "'+str(root/'kernel/drivers/storage/bfs4.c')+'"\n'+r'''
static fat_fs_t fs;
static block_device_t dev;
static void crash(void) {
 bfs4_volume_t *v=bfs4_volume(&fs);
 require(v!=NULL,"mounted volume");
 bfs4_defrag_forget(v);bfs4_release(v); /* discard RAM without sync */
}
static void intact(void) {
 fat_dir_entry_t entry;char text[16];uint32_t got;
 require(bfs4_resolve(&fs,"/saved.txt",&entry),"recover original name");
 require(bfs4_read(&fs,&entry,0,text,sizeof(text),&got) && got==5 && !memcmp(text,"hello",5),"recover original bytes");
 fat_check_report_t r;
 require(bfs4_check(&fs,&r) && r.completed && !r.errors,"consistent volume");
}
int main(int argc,char **argv) {
 if(argc!=2 || !(disk=fopen(argv[1],"r+b")))return 2;
 dev.sector_count=32*1024*1024/512;dev.sector_size=512;dev.read=host_read;dev.write=host_write;
 require(bfs4_format(&dev,"RECOVERY") && bfs4_mount(&fs,&dev,0),"format/mount temporary disk");
 require(bfs4_write(&fs,"/saved.txt","hello",5),"write original");
 bfs4_volume_t *v=bfs4_volume(&fs);
 uint32_t correct=v->super.free_blocks;
 v->super.free_blocks--;v->super.state=BFS4_DIRTY;
 require(bfs4_write_super(v),"inject interrupted counter checkpoint");crash();
 dev.read_only=true;require(bfs4_mount(&fs,&dev,0),"read-only counter recovery");
 intact();require(bfs4_volume(&fs)->super.free_blocks==correct,"counter reconstructed in RAM");crash();
 dev.read_only=false;require(bfs4_mount(&fs,&dev,0),"writable counter recovery");intact();
 require(bfs4_volume(&fs)->super.free_blocks==correct,"counter restored");
 /* A leaked allocation has no safe automatic owner: refuse to call clean. */
 v=bfs4_volume(&fs);uint32_t spare=v->super.total_blocks-1;
 require(!bfs4_bit(v->block_bitmap,spare),"spare fixture block");
 bfs4_set_bit(v->block_bitmap,spare,true);v->super.free_blocks--;
 require(bfs4_write_bitmap_byte(v,false,spare),"inject orphan allocation");
 require(!bfs4_sync(&fs) && v->super.state==BFS4_DIRTY,"inconsistent sync rejected");
 crash();require(!bfs4_mount(&fs,&dev,0),"orphan volume refused without deleting data");
 /* Isolated fixture only: start another filesystem for write-error testing. */
 require(bfs4_format(&dev,"IOFAULT") && bfs4_mount(&fs,&dev,0),"new temporary volume");
 require(bfs4_write(&fs,"/saved.txt","hello",5),"write second original");
 v=bfs4_volume(&fs);fail_write=true;
 require(!bfs4_write_inode(v,1),"inject real inode-sector write failure");
 require(v->write_failed && !bfs4_sync(&fs),"uncertain disk never marked clean");
 crash();require(bfs4_mount(&fs,&dev,0),"validate actual disk after failure");intact();
 require(bfs4_sync(&fs),"validated fresh mount checkpoints normally");
 /* Missing reserved bitmap bits must be errors, never free user space. */
 v=bfs4_volume(&fs);bfs4_set_bit(v->block_bitmap,0,false);
 fat_check_report_t r;require(bfs4_check(&fs,&r) && r.invalid_chains && r.errors,"missing reserved bit detected");
 bfs4_set_bit(v->block_bitmap,0,true);intact();
 require(bfs4_unmount(&fs),"clean final unmount");fclose(disk);
 puts("PASS dirty counter recovery, read-only recovery, orphan refusal, failed writes, remount, reserved bits");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-recovery-') as d:
    d=Path(d);c=d/'test.c';c.write_text(code);binary=d/'test';disk=d/'disk.img'
    with disk.open('wb') as f:f.truncate(32*1024*1024)
    subprocess.run(['gcc','-m32','-O2','-Wall','-Wno-stringop-truncation',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary),str(disk)],check=True)
