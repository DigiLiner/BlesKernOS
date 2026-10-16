#!/usr/bin/env python3
"""Production BFS4 + swap: pinning, direct slots, rejected headers and guards."""
from pathlib import Path
import subprocess
import sys
import hashlib
import tempfile
root=Path(__file__).resolve().parents[2]
base=(root/'tests/storage/bfs4_host.c').read_text().split('int main(')[0]
base=base.replace('"../../kernel/','"'+str(root/'kernel')+'/')
code=base+r'''
/* A regular host file has no volatile controller cache; both APIs complete
 * through the same writer while exercising the real swap slot bounds. */
bool block_write_volatile(block_device_t *d,uint32_t l,uint8_t n,const void *p){
 return block_write(d,l,n,p);
}
static block_device_t test_device;
static fat_fs_t test_fs;
uint32_t block_count(void){return 1;}
block_device_t *block_at(uint32_t i){return i?NULL:&test_device;}
bool fat_get_current(fat_fs_t *out){*out=test_fs;return true;}
bool mm_vm_frame_alloc(uint32_t count,uint32_t *out){
 void *p=calloc(count,4096);*out=(uint32_t)(uintptr_t)p;return p!=NULL;
}
'''+'\n#include "'+str(root/'kernel/drivers/storage/bfs4.c')+'"\n'+\
'\n#include "'+str(root/'kernel/mm/swap.c')+'"\n'+r'''
static void header(uint8_t *data,uint32_t size){
 memset(data,0,512);memcpy(data,"BKSWAP01",8);
 uint32_t *f=(uint32_t *)(data+8);
 f[0]=1;f[1]=4096;f[2]=(size-4096)/4096;f[3]=8;f[4]=size/512;
 *(uint32_t *)(data+508)=crc32(data,508);
}
int main(int argc,char **argv){
 if(argc!=2 || !(disk=fopen(argv[1],"r+b")))return 2;
 test_device.type=BLOCK_DEVICE_ATA;test_device.sector_count=64*1024*1024/512;
 test_device.sector_size=512;test_device.read=host_read;test_device.write=host_write;
 require(bfs4_format(&test_device,"SWAPFILE")&&bfs4_mount(&test_fs,&test_device,0),"format/mount");
 require(bfs4_mkdir(&test_fs,"/SYSTEM"),"system directory");
 require(bfs4_write(&test_fs,"/saved.txt","preserved",9),"unrelated file");
 uint32_t size=8*1024*1024;uint8_t *data=calloc(1,size);
 header(data,size);data[508]^=1;
 require(bfs4_write(&test_fs,"/SYSTEM/SWAP.BKS",data,size),"reserve file");
 swap_init();require(!device,"filesystem disk never activated as whole swap");
 swap_init_file();require(!device && !bfs4_volume(&test_fs)->swap_inode,"bad CRC releases pin");
 data[508]^=1;require(bfs4_write_at(&test_fs,"/SYSTEM/SWAP.BKS",0,data,512),"correct header");
 bfs4_volume_t *v=bfs4_volume(&test_fs);uint32_t inode;
 require(bfs4_lookup(v,"/SYSTEM/SWAP.BKS",&inode),"swap inode");
 bfs4_inode_t original=v->inodes[inode];
 v->inodes[inode].extent_count=2;v->inodes[inode].extents[0].count--;
 v->inodes[inode].extents[1]=(bfs4_extent_t){original.extents[0].start+original.extents[0].count-1,1};
 swap_init_file();require(!device&&!v->swap_inode,"fragmented file refused");
 v->inodes[inode]=original;
 test_device.read_only=true;swap_init_file();require(!device,"readonly refused");test_device.read_only=false;
 swap_init_file();require(device==&test_device&&stats.enabled&&stats.slots==2047,"activate file");
 uint32_t saved_start=v->inodes[inode].extents[0].start;
 require(storage_lba==v->start_lba+saved_start*8,"physical extent offset");
 require(!bfs4_write(&test_fs,"/SYSTEM/SWAP.BKS","x",1),"overwrite blocked");
 require(!bfs4_write_at(&test_fs,"/SYSTEM/SWAP.BKS",4096,"x",1),"range write blocked");
 require(!bfs4_resize(&test_fs,"/SYSTEM/SWAP.BKS",0),"truncate blocked");
 require(!bfs4_remove(&test_fs,"/SYSTEM/SWAP.BKS"),"remove blocked");
 require(!bfs4_rename(&test_fs,"/SYSTEM/SWAP.BKS","/other"),"rename blocked");
 require(!bfs4_rename(&test_fs,"/SYSTEM","/moved"),"ancestor rename blocked");
 require(!bfs4_set_mode(&test_fs,"/SYSTEM/SWAP.BKS",0600),"mode blocked");
 require(!bfs4_unmount(&test_fs)&&!bfs4_format(&test_device,"BAD"),"unmount/format blocked");
 fat_dir_entry_t entry;uint32_t got;
 require(bfs4_resolve(&test_fs,"/SYSTEM/SWAP.BKS",&entry),"list reserved inode");
 require(!bfs4_read(&test_fs,&entry,4096,data,4096,&got),"VFS access blocked during raw I/O");
 uint32_t slot=swap_slot_alloc();require(slot==1,"first slot");
 memset(data,0x5a,4096);require(swap_page_write(slot,data),"raw page out");
 memset(data,0,4096);require(swap_page_read(slot,data),"raw page in");
 for(uint32_t i=0;i<4096;i++)require(data[i]==0x5a,"page data preserved");
 require(!swap_enable(false),"busy disable refused");swap_slot_free(slot);
 require(swap_enable(false)&&!bfs4_resize(&test_fs,"/SYSTEM/SWAP.BKS",0),"disabled backend keeps boot pin");
 require(swap_enable(true),"safe reenable");
 require(bfs4_defrag_begin(&test_fs,7,true),"defrag with reserved swap");
 bk_defrag_info_t progress;
 for(uint32_t step=0;step<2000;step++){
  require(bfs4_defrag_status(&test_fs,&progress),"defrag status");
  if(progress.state!=BK_DEFRAG_RUNNING)break;
  require(bfs4_defrag_step(&test_fs,7),"defrag step");
 }
 require(progress.state==BK_DEFRAG_COMPLETE&&progress.skipped_files,"swap skipped by defrag");
 require(v->inodes[inode].extents[0].start==saved_start,"extent remains pinned");
 require(bfs4_resolve(&test_fs,"/saved.txt",&entry)&&bfs4_read(&test_fs,&entry,0,data,9,&got)&&
  got==9&&!memcmp(data,"preserved",9),"unrelated file untouched");
 fat_check_report_t report;require(bfs4_check(&test_fs,&report)&&!report.errors&&bfs4_sync(&test_fs),"metadata consistent");
 free(data);fclose(disk);
 puts("PASS actual BFS4 file swap: signatures, contiguous allocation, raw page I/O, pin guards, swapoff/enable, defrag exclusion and filesystem integrity");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-swapfile-') as folder:
    d=Path(folder);c=d/'test.c';c.write_text(code);image=d/'disk.img'
    with image.open('wb') as f:f.truncate(64*1024*1024)
    subprocess.run(['gcc','-m32','-O2','-Wall','-Wno-stringop-truncation',str(c),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test'),str(image)],check=True)
    def digest():
        with image.open('rb') as f:return hashlib.file_digest(f,'sha256').digest()
    original=digest()
    command=[sys.executable,str(root/'tools/install_swap_file.py'),str(image),'--mib','8']
    subprocess.run(command,check=True,stdout=subprocess.DEVNULL)
    assert digest()==original,'idempotent install changed reserved data or filesystem'
    sys.path.insert(0,str(root/'tools'))
    from bfs4_update import Editor
    editor=Editor(image);inode=editor.inodes[editor.find('/SYSTEM/SWAP.BKS')]
    first=editor.extents(inode)[0][0];offset=editor.base+first*4096+508;editor.fd.close()
    with image.open('r+b') as f:
        f.seek(offset);byte=f.read(1);f.seek(offset);f.write(bytes([byte[0]^1]))
    invalid=digest()
    refused=subprocess.run(command,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    assert refused.returncode and digest()==invalid,'unsigned file overwritten'
    print('PASS offline installer idempotence and invalid-file preservation')
