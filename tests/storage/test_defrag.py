#!/usr/bin/env python3
"""Exercise the actual BFS4 defragmenter, including failed block transfers."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
base=(root/'tests/storage/bfs4_host.c').read_text().split('int main(')[0]
base=base.replace('"../../kernel/','"'+str(root/'kernel')+'/')
base=base.replace('static FILE *disk;', '''static FILE *disk;
static uint32_t fail_read_lba=0xffffffffU,fail_write_lba=0xffffffffU,corrupt_read_lba=0xffffffffU;
static uint32_t data_reads,data_writes;''')
base=base.replace('    (void)device;\n    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&\n           fread(data, 512, count, disk) == count;', '''    (void)device;data_reads++;
    if(lba==fail_read_lba) {fail_read_lba=0xffffffffU;return false;}
    bool ok=fseek(disk,(long)lba*512L,SEEK_SET)==0 && fread(data,512,count,disk)==count;
    if(ok && lba==corrupt_read_lba) {((uint8_t*)data)[0]^=1;corrupt_read_lba=0xffffffffU;}
    return ok;''')
base=base.replace('    (void)device;\n    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&\n           fwrite', '''    (void)device;data_writes++;
    if(lba==fail_write_lba) {fail_write_lba=0xffffffffU;return false;}
    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&
           fwrite''')
source=base+'\n#include "'+str(root/'kernel/drivers/storage/bfs4.c')+'"\n'+r'''
static fat_fs_t fs;
static block_device_t device;
static uint8_t expected[67*4096],received[67*4096];
static void consistent(void) {
 fat_check_report_t report;
 require(bfs4_check(&fs,&report) && !report.errors,"consistent bitmap/inodes");
}
static void content(void) {
 fat_dir_entry_t entry;uint32_t got;
 require(bfs4_resolve(&fs,"/a.bin",&entry),"resolve original");
 require(bfs4_read(&fs,&entry,0,received,sizeof(received),&got) && got==sizeof(received),"read original");
 require(!memcmp(received,expected,sizeof(expected)),"original bytes preserved");
}
static void run(void) {
 for(uint32_t n=0;n<10000 && g_defrag.info.state==BK_DEFRAG_RUNNING;n++)
  require(bfs4_defrag_step(&fs,42),"step completes");
 require(g_defrag.info.state==BK_DEFRAG_COMPLETE,"bounded job completes");
}
static void start_fragmented(void) {
 require(bfs4_defrag_begin(&fs,42,true),"begin optimize");
 require(!bfs4_defrag_begin(&fs,99,true),"exclusive job owner");
 require(!bfs4_defrag_step(&fs,99) && !bfs4_defrag_cancel(&fs,99),"other process cannot drive/cancel");
 while(!g_defrag.destination && g_defrag.info.state==BK_DEFRAG_RUNNING)
  require(bfs4_defrag_step(&fs,42),"find fragmented inode");
 require(g_defrag.destination && g_defrag.position==32,"step bounded to 32 copied blocks");
}
static void fault(unsigned type) {
 start_fragmented();
 uint32_t old=bfs4_map(g_defrag.volume,&g_defrag.original,g_defrag.position);
 uint32_t target=g_defrag.destination+g_defrag.position;
 if(type==0) fail_read_lba=g_defrag.volume->start_lba+old*8;
 if(type==1) fail_write_lba=g_defrag.volume->start_lba+target*8;
 if(type==2) corrupt_read_lba=g_defrag.volume->start_lba+target*8;
 require(!bfs4_defrag_step(&fs,42),"failed transfer stops relocation");
 require(g_defrag.info.state==BK_DEFRAG_ERROR && g_defrag.info.error==BK_DEFRAG_IO_ERROR,"actual I/O error reported");
 unsigned red=0;for(unsigned i=0;i<BK_DEFRAG_CELLS;i++)red+=g_defrag.info.cells[i]==BK_DEFRAG_FAILED;
 require(red==1,"failed physical block appears red");
 content();consistent();
 if(type==1) {
  require(bfs4_sync(&fs),"validated journal rollback permits clean checkpoint");
  bfs4_volume_t *v=bfs4_volume(&fs);
  bfs4_defrag_forget(v);bfs4_release(v);
  require(bfs4_mount(&fs,&device,0),"fresh mount validates rollback on disk");
  content();consistent();
 }
}
int main(int argc,char **argv) {
 if(argc!=2 || !(disk=fopen(argv[1],"r+b")))return 2;
 device.sector_count=32*1024*1024/512;device.sector_size=512;device.read=host_read;device.write=host_write;
 require(bfs4_format(&device,"DEFRAGTEST") && bfs4_mount(&fs,&device,0),"format/mount real BFS4");
 for(uint32_t i=0;i<sizeof(expected);i++)expected[i]=(uint8_t)(i*37U+19);
 require(bfs4_write(&fs,"/a.bin",expected,2*4096),"first extent");
 require(bfs4_write(&fs,"/spacer.bin",expected,2*4096),"spacer");
 require(bfs4_append(&fs,"/a.bin",expected,2*4096,sizeof(expected)),"fragmented append");
 content();consistent();
 device.read_only=true;
 require(!bfs4_defrag_begin(&fs,42,true),"read-only volume refuses relocation");
 require(bfs4_defrag_begin(&fs,42,false),"read-only analysis allowed");
 unsigned writes=data_writes;run();require(data_writes==writes,"analysis never writes disk");
 require(g_defrag.info.fragmented_before==1 && g_defrag.info.fragmented_after==1,"analysis preserves fragmentation");
 device.read_only=false;
 uint64_t total,free_before,free_after;
 require(bfs4_space(&fs,&total,&free_before),"space before cancel");
 start_fragmented();content();
 require(bfs4_sync(&fs) && g_defrag.volume->super.state==BFS4_DIRTY,"pending relocation never checkpoints clean");
 require(bfs4_defrag_cancel(&fs,42),"cancel pending copy");
 require(bfs4_space(&fs,&total,&free_after) && free_before==free_after,"cancel releases only destination");
 content();consistent();
 start_fragmented();expected[7]^=0x55;
 require(bfs4_write_at(&fs,"/a.bin",7,expected+7,1),"writer supersedes pending copy");
 require(!g_defrag.destination && !g_defrag.inode && g_defrag.info.skipped_files==1,"changed file skipped");
 run();content();consistent();
 start_fragmented();require(bfs4_set_mode(&fs,"/a.bin",0600),"chmod cancels stale metadata copy");
 require(!g_defrag.destination,"chmod reservation released");run();consistent();
 start_fragmented();require(bfs4_set_times(&fs,"/a.bin",true,100,true,200),"timestamp change cancels copy");
 run();consistent();
 start_fragmented();require(bfs4_rename(&fs,"/a.bin","/renamed.bin"),"rename cancels copy");
 run();require(bfs4_rename(&fs,"/renamed.bin","/a.bin"),"restore fixture path");content();consistent();
 fault(0);fault(1);fault(2);
 uint64_t inode;uint16_t mode;
 require(bfs4_inode(&fs,"/a.bin",&inode) && bfs4_get_mode(&fs,"/a.bin",&mode),"capture metadata");
 require(bfs4_defrag_begin(&fs,42,true),"successful optimize begin");run();
 require(g_defrag.info.moved_files==1 && g_defrag.info.fragmented_after==0,"real fragmentation reduction");
 content();consistent();
 uint64_t after;uint16_t after_mode;uint32_t atime,mtime;
 require(bfs4_inode(&fs,"/a.bin",&after) && after==inode,"stable inode");
 require(bfs4_get_mode(&fs,"/a.bin",&after_mode) && mode==after_mode,"preserve permissions");
 require(bfs4_get_times(&fs,"/a.bin",&atime,&mtime) && atime==100 && mtime==200,"preserve timestamps");
 require(bfs4_unmount(&fs) && bfs4_mount(&fs,&device,0),"remount optimized layout");
 content();consistent();
 /* Indirect extent tables must be released after consolidation too. */
 require(bfs4_write(&fs,"/indirect.bin",expected,4096),"indirect first extent");
 for(unsigned i=0;i<10;i++) {
  char path[32];sprintf(path,"/gap%u.bin",i);
  require(bfs4_write(&fs,path,expected,4096),"indirect spacer");
  require(bfs4_append(&fs,"/indirect.bin",expected,(i+1)*4096,(i+2)*4096),"indirect extent append");
 }
 require(bfs4_defrag_begin(&fs,42,true),"begin indirect relocation");run();consistent();
 uint64_t indirect;require(bfs4_inode(&fs,"/indirect.bin",&indirect),"indirect inode");
 require(g_defrag.volume->inodes[indirect].extent_count==1 && !g_defrag.volume->inodes[indirect].indirect,"indirect table consolidated and released");
 fat_dir_entry_t indirect_entry;uint32_t got;
 require(bfs4_resolve(&fs,"/indirect.bin",&indirect_entry) && bfs4_read(&fs,&indirect_entry,0,received,11*4096,&got) && got==11*4096 && !memcmp(received,expected,got),"indirect contents unchanged");
 /* Failed inode publication is ambiguous: retain both copies, stay dirty,
    reject another maintenance job, and leave original data accessible. */
 require(bfs4_remove(&fs,"/a.bin"),"replace fixture");
 require(bfs4_write(&fs,"/a.bin",expected,2*4096),"first new extent");
 require(bfs4_write(&fs,"/spacer2.bin",expected,3*4096),"new spacer");
 require(bfs4_append(&fs,"/a.bin",expected,2*4096,sizeof(expected)),"new fragmented file");
 start_fragmented();
 uint32_t sector=g_defrag.volume->start_lba+g_defrag.volume->super.inode_table_start*8+
     (g_defrag.inode*BFS4_INODE_SIZE)/512;
 fail_write_lba=sector;
 while(g_defrag.info.state==BK_DEFRAG_RUNNING) bfs4_defrag_step(&fs,42);
 require(g_defrag.info.state==BK_DEFRAG_ERROR && !g_defrag.uncertain,"journal undoes failed inode publication");
 content();consistent();require(bfs4_sync(&fs),"validated failed-publication rollback checkpoints safely");
 require(bfs4_defrag_begin(&fs,42,true),"recovered publication permits another maintenance job");
 require(bfs4_defrag_cancel(&fs,42),"cancel recovered maintenance job");
 fclose(disk);
 puts("PASS real BFS4 defrag, bounded copies, read-only analysis, owner isolation, cancellation, concurrent writes/metadata, I/O and verification failures, remount and uncertain publication");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-defrag-') as folder:
 p=Path(folder);(p/'test.c').write_text(source);image=p/'disk.img'
 with image.open('wb') as stream:stream.truncate(32*1024*1024)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Wextra',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(image)],check=True)
