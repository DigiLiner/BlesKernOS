#!/usr/bin/env python3
"""Compile actual swap and pager with bounded RAM, disk faults and TLB mocks."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
def source(path):
 return re.sub(r'^#include[^\n]*\n','',(root/path).read_text(),flags=re.M)
headers=source('kernel/include/swap.h')+source('kernel/include/pager.h')
swap=re.sub(r'\block\b','swap_lock',source('kernel/mm/swap.c'))
pager=source('kernel/mm/pager.c')
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint32_t (*paging_range_skip_t)(void *,uint32_t,uint32_t);
typedef int kmutex_t;
#define KMUTEX_INITIALIZER 0
#define kmemset memset
#define kmemcpy memcpy
#define kmemcmp memcmp
#define kprintf(...) ((void)0)
#define MM_SUBSYSTEM_VM 3
static unsigned heap_nodes;
static bool fail_metadata_heap;
static void *kzalloc(uint32_t n){if(fail_metadata_heap)return NULL;void *p=calloc(1,n);if(p)heap_nodes++;return p;}
static void kfree(void *p){assert(p && heap_nodes);heap_nodes--;free(p);}
static bool mm_set_allocation_owner(void *p,uint32_t pid){assert(p && !pid);return true;}
static bool mm_set_allocation_subsystem(void *p,uint8_t subsystem){assert(p && subsystem==MM_SUBSYSTEM_VM);return true;}
static void kmutex_lock(kmutex_t *p){(void)p;}
static void kmutex_unlock(kmutex_t *p){(void)p;}
#define TASK_MAX 128
#define BLOCK_DEVICE_ATA 1
static uint8_t disk[129*4096],main_disk[512];
static bool disk_error,tlb_error;
static uint32_t disk_writes;
typedef struct {int unused;} fat_fs_t;
typedef struct {uint32_t type,base_lba,sector_count,sector_size;bool read_only;void *write;char name[8];} block_device_t;
static block_device_t disks[2]={{1,0,129*8,512,false,(void*)1,"main"},{1,0,129*8,512,false,(void*)1,"swap"}};
static uint32_t block_count(void){return 2;}
static block_device_t *block_at(uint32_t i){return &disks[i];}
static bool fat_get_current(fat_fs_t *fs){(void)fs;return false;}
static bool bfs4_swap_pin(const fat_fs_t *fs,const char *p,block_device_t **d,uint32_t *l,uint32_t *s){
 (void)fs;(void)p;(void)d;(void)l;(void)s;return false;
}
static void bfs4_swap_unpin(const fat_fs_t *fs){(void)fs;}
static bool block_read(block_device_t *d,uint32_t lba,unsigned n,void *out){
 if(d==&disks[0]){assert(lba==0&&n==1);memcpy(out,main_disk,512);return true;}
 if(disk_error)return false;
 assert((lba+n)*512<=sizeof(disk));memcpy(out,disk+lba*512,n*512);return true;
}
static bool block_write_volatile(block_device_t *d,uint32_t lba,unsigned n,const void *in){
 assert(d==&disks[1]&&lba>=8&&(lba+n)*512<=sizeof(disk));
 if(disk_error)return false;
 memcpy(disk+lba*512,in,n*512);disk_writes++;return true;
}
static uint8_t frames[48][4096] __attribute__((aligned(4096)));
static bool used[48];
static uint32_t mm_vm_frame_total_pages(void){return 48;}
static uint32_t mm_vm_frame_free_pages(void){uint32_t n=0;for(unsigned i=0;i<48;i++)n+=!used[i];return n;}
static bool mm_vm_frame_alloc(uint32_t count,uint32_t *out){
 for(unsigned i=0;i+count<=48;i++) {
  bool free_run=true;for(unsigned j=0;j<count;j++)if(used[i+j])free_run=false;
  if(!free_run)continue;
  for(unsigned j=0;j<count;j++)used[i+j]=true;
  *out=(uint32_t)(uintptr_t)frames[i];memset((void *)(uintptr_t)*out,0,count*4096);return true;
 }
 return false;
}
static void mm_vm_frame_free(uint32_t address,uint32_t count){
 unsigned i=(address-(uint32_t)(uintptr_t)frames)/4096;
 assert(i+count<=48);for(unsigned j=0;j<count;j++){assert(used[i+j]);used[i+j]=false;}
}
static uint32_t entries[4][128];
static uint32_t paging_process_owner(uint32_t p){return p==3?1:p;}
static bool paging_pager_reserve(uint32_t pid,uint32_t hint,uint32_t len,bool access,bool write,bool exact,uint32_t *out,paging_range_skip_t skip,void *ctx){
 (void)exact;(void)skip;(void)ctx;assert(pid<4);*out=hint?hint:0x60000000;
 unsigned first=(*out-0x60000000)/4096;assert(first+len/4096<=128);
 for(unsigned i=first;i<first+len/4096;i++)assert(!entries[pid][i]);
 for(unsigned i=first;i<first+len/4096;i++)entries[pid][i]=0x500|(access?4:0x200)|(write?2:0);
 return true;
}
static uint32_t paging_pager_entry(uint32_t pid,uint32_t va){return entries[pid][(va-0x60000000)/4096];}
static bool paging_pager_replace(uint32_t pid,uint32_t va,uint32_t frame,bool present){
 if(!present&&tlb_error)return false;
 uint32_t *e=&entries[pid][(va-0x60000000)/4096];assert(*e&0x100);
 uint32_t access=*e&0xe06U;
 if(access&0x800U)access=(access&~0x800U)|2U;
 *e=access|0x500|(present?(frame|1):0);return true;
}
static bool paging_process_unmap(uint32_t pid,uint32_t va,uint32_t length){
 for(unsigned i=0;i<length/4096;i++)entries[pid][(va-0x60000000)/4096+i]=0;return true;
}
static bool paging_process_access(uint32_t pid,uint32_t va,uint32_t len,bool access,bool write){
 for(unsigned i=0;i<len/4096;i++) {
  uint32_t *e=&entries[pid][(va-0x60000000)/4096+i];
  *e=(*e&~0x206U)|(access?4:0x200)|(write?2:0);
 }
 return true;
}
static bool paging_pager_adopt(uint32_t pid,uint32_t va,uint32_t len){
 for(unsigned i=0;i<len/4096;i++)entries[pid][(va-0x60000000)/4096+i]|=0x100;
 return true;
}
static bool paging_pager_replace_cow(uint32_t pid,uint32_t va,uint32_t frame,bool present,bool cow){
 uint32_t *e=&entries[pid][(va-0x60000000)/4096];
 bool write=(*e&(2|0x800))!=0;
 if(!paging_pager_replace(pid,va,frame,present))return false;
 *e=(*e&~(2U|0x800U))|(write?(cow?0x800U:2U):0U);return true;
}
static bool paging_process_cow_access(uint32_t pid,uint32_t va,uint32_t len,bool access,bool write){
 if(!paging_process_access(pid,va,len,access,false))return false;
 for(unsigned i=0;i<len/4096;i++){
  uint32_t *e=&entries[pid][(va-0x60000000)/4096+i];
  *e=(*e&~0x800U)|(write?0x800U:0U);
 }
 return true;
}

#define KDOMAIN_VFS 2
static unsigned vfs_depth;
static void kernel_domains_enter(uint32_t mask){assert(mask==KDOMAIN_VFS);vfs_depth++;}
static void kernel_domains_exit(uint32_t mask){assert(mask==KDOMAIN_VFS && vfs_depth);vfs_depth--;}
typedef struct vfs_page_reference {unsigned references,size,reads;unsigned char data[3*4096];bool io_error;} vfs_page_reference_t;
typedef struct vfs_shared_mapping {uint32_t backing,length,size,references;} vfs_shared_mapping_t;
static void vfs_page_reference_release(vfs_page_reference_t *r){assert(r && r->references && vfs_depth);r->references--;}
static uint32_t vfs_page_reference_size(vfs_page_reference_t *r){return r->size;}
static bool vfs_page_reference_read(vfs_page_reference_t *r,uint32_t offset,void *out,uint32_t length,uint32_t *got){
 assert(vfs_depth && offset<r->size);r->reads++;if(r->io_error)return false;
 if(length>r->size-offset)length=r->size-offset;
 memcpy(out,r->data+offset,length);*got=length;return true;
}
static bool vfs_shared_release(vfs_shared_mapping_t *s){assert(s->references && vfs_depth);s->references--;return true;}
'''+headers+swap+pager+r'''
static void header(void){
 memcpy(disk,"BKSWAP01",8);uint32_t *f=(uint32_t *)(disk+8);
 f[0]=1;f[1]=4096;f[2]=128;f[3]=8;f[4]=129*8;
 *(uint32_t *)(disk+508)=crc32(disk,508);
}
static void fill(uint32_t pid,uint32_t va,uint8_t value){
 assert(pager_pin(pid,va,4096,true));pager_page_t *p=find(pid,va);
 memset((void *)(uintptr_t)p->frame,value,4096);pager_unpin(pid,va,4096);
}
static void verify(uint32_t pid,uint32_t va,uint8_t value){
 assert(pager_pin(pid,va,4096,false));pager_page_t *p=find(pid,va);
 for(unsigned i=0;i<4096;i++)assert(((uint8_t *)(uintptr_t)p->frame)[i]==value);
 pager_unpin(pid,va,4096);
}
int main(void){
 memset(main_disk,0xa5,sizeof(main_disk));header();disk[508]^=1;
 swap_init();assert(!stats.enabled&&!disk_writes);disk[508]^=1;
 swap_init();assert(stats.enabled&&stats.slots==128);
 uint32_t slots[128];for(unsigned i=0;i<128;i++){slots[i]=swap_slot_alloc();assert(slots[i]);}
 assert(!swap_slot_alloc()&&!swap_enable(false));
 for(unsigned i=0;i<128;i++)swap_slot_free(slots[i]);assert(!stats.used);
 for(unsigned i=0;i<sizeof(main_disk);i++)assert(main_disk[i]==0xa5);
 uint32_t va;assert(pager_map(1,0,64*4096,3,false,&va,NULL,NULL));
 for(unsigned i=0;i<64;i++)fill(1,va+i*4096,(uint8_t)i);
 assert(stats.used&&evictions&&disk_writes);
 uint32_t frame_count=mm_vm_frame_free_pages();
 assert(!pager_swapoff()&&stats.enabled);assert(mm_vm_frame_free_pages()<=frame_count);
 for(unsigned i=0;i<64;i++)verify(1,va+i*4096,(uint8_t)i);
 pager_page_t *swapped=NULL;unsigned index=0;
 for(;index<64;index++)if(find(1,va+index*4096)->slot){swapped=find(1,va+index*4096);break;}
 assert(swapped);uint32_t original_slot=swapped->slot;
 disk_error=true;assert(!pager_fault(1,va+index*4096,false));
 assert(swapped->slot==original_slot&&!swapped->frame);disk_error=false;
 verify(1,va+index*4096,(uint8_t)index);
 tlb_error=true;uint32_t saved_out=evictions;
 /* TLB failure never recycles a victim's frame or publishes a slot. */
 uint32_t reclaimed_frame;assert(!reclaim(&reclaimed_frame));assert(evictions==saved_out);tlb_error=false;
 assert(pager_protect(1,va,64*4096,1));assert(!pager_fault(1,va,true));
 assert(pager_protect(1,va,64*4096,3));
 assert(!pager_pin(1,va,64*4096,true)); /* bounded RAM; all prior pins roll back */
 for(unsigned i=0;i<64;i++)assert(!find(1,va+i*4096)->pins);
 disk_error=true;assert(!pager_clone(1,2,va,64*4096));disk_error=false;
 assert(!find(2,va));for(unsigned i=0;i<64;i++)verify(1,va+i*4096,(uint8_t)i);
 assert(pager_clone(1,2,va,64*4096));
 fill(2,va,0xee);verify(1,va,0);verify(2,va,0xee);
 pager_stats_t own,child,all,borrowed;
 pager_stats(1,&own);pager_stats(2,&child);pager_stats(0,&all);pager_stats(3,&borrowed);
 assert(own.virtual_pages==64&&child.virtual_pages==64&&all.virtual_pages==128);
 assert(own.faults&&own.evictions&&own.failures);
 assert(all.faults==own.faults+child.faults&&all.evictions==own.evictions+child.evictions);
 assert(!memcmp(&own,&borrowed,sizeof(own)));
 pager_swap_ids(1,2);verify(1,va,0xee);verify(2,va,0);
 pager_stats(2,&borrowed);assert(borrowed.faults>=own.faults);
 pager_swap_ids(1,2);
 tlb_error=true;assert(!pager_unmap(2,va,64*4096));tlb_error=false;
 verify(2,va,0xee);
 assert(pager_unmap(2,va,64*4096));
 /* Shrinking brk keeps the reservation but returns both RAM and swap. */
 void *word=NULL;
 assert(!pager_pin_word(1,va+1,&word));
 assert(pager_pin_word(1,va+16*4096,&word) && word);
 assert(!pager_discard(1,va+16*4096,4096));
 assert(!pager_unmap(1,va+16*4096,4096));
 assert(pager_pin(1,va+16*4096,4096,false));
 pager_release_word(word);
 assert(find(1,va+16*4096)->pins==1);
 pager_unpin(1,va+16*4096,4096);
 assert(!find(1,va+16*4096)->pins);
 pager_release_word(NULL);
 assert(pager_pin(1,va+16*4096,4096,false));
 assert(!pager_discard(1,va+16*4096,4096));
 pager_unpin(1,va+16*4096,4096);
 tlb_error=true;assert(!pager_discard(1,va+16*4096,32*4096));tlb_error=false;
 verify(1,va+16*4096,16);
 assert(pager_discard(1,va+16*4096,32*4096));
 for(unsigned n=16;n<48;n++)assert(find(1,va+n*4096)->alive && !find(1,va+n*4096)->frame && !find(1,va+n*4096)->slot);
 assert(!pager_fault(1,va+16*4096,true));
 assert(pager_protect(1,va+16*4096,32*4096,3));
 verify(1,va+16*4096,0);verify(1,va+47*4096,0);
 assert(pager_unmap(1,va+16*4096,32*4096));verify(1,va,0);verify(1,va+63*4096,63);
 pager_destroy(1);assert(!stats.used&&mm_vm_frame_free_pages()==47);
 memset(entries[1],0,sizeof(entries[1])); /* scheduler destroys the old CR3 */
 pager_stats(1,&own);assert(!own.virtual_pages&&!own.faults&&!own.evictions&&!own.failures);
 assert(pager_map(3,0,4096,3,false,&va,NULL,NULL));fill(1,va,0x77);
 pager_destroy(3);verify(1,va,0x77);assert(pager_unmap(3,va,4096));pager_destroy(1);
 assert(pager_swapoff()&&!stats.enabled);assert(swap_enable(true));
 /* File views detach a single page, preserve protection through fork, and
    swap only private dirty copies. The inode owns the two source frames. */
 uint32_t source=0;assert(mm_vm_frame_alloc(2,&source));
 memset((void *)(uintptr_t)source,'A',4096);
 memset((void *)(uintptr_t)(source+4096),'B',4096);
 va=0x60000000;
 entries[1][0]=source|0xc05U;entries[1][1]=(source+4096)|0xc05U;
 assert(pager_adopt_cow(1,va,8192,source));
 uint32_t clean_free=mm_vm_frame_free_pages();
 assert(pager_pin(1,va,8192,false));pager_unpin(1,va,8192);
 assert(mm_vm_frame_free_pages()==clean_free);
 assert(pager_fault(1,va,true));
 assert(mm_vm_frame_free_pages()==clean_free-1);
 assert(find(1,va)->frame && !find(1,va+4096)->frame);
 memset((void *)(uintptr_t)find(1,va)->frame,'P',4096);
 assert(*(unsigned char *)(uintptr_t)source=='A');
 assert(pager_protect(1,va+4096,4096,1));
 assert(!pager_fault(1,va+4096,true));
 assert(pager_clone(1,2,va,8192));
 assert(find(2,va)->frame && !find(2,va+4096)->frame);
 assert(*(unsigned char *)(uintptr_t)find(2,va)->frame=='P');
 assert(!pager_fault(2,va+4096,true));
 assert(pager_fault(2,va+4096,false));
 assert(pager_protect(2,va+4096,4096,3));
 assert(pager_fault(2,va+4096,true));
 memset((void *)(uintptr_t)find(2,va+4096)->frame,'C',4096);
 assert(*(unsigned char *)(uintptr_t)(source+4096)=='B');
 uint32_t recycled=0;assert(reclaim(&recycled));mm_vm_frame_free(recycled,1);
 assert(pager_fault(2,va,false) && pager_fault(2,va+4096,false));
 assert(*(unsigned char *)(uintptr_t)find(2,va)->frame=='P');
 assert(*(unsigned char *)(uintptr_t)find(2,va+4096)->frame=='C');
 assert(pager_unmap(2,va,8192) && pager_unmap(1,va,8192));
 pager_destroy(1);pager_destroy(2);mm_vm_frame_free(source,2);
 assert(!stats.used && mm_vm_frame_free_pages()==47);
 assert(!heap_nodes);

 /* Demand file mapping: no eager frames or disk reads, shared clean page,
    first-write private detach, sparse cache reclaim, failed I/O/TLB rollback,
    permissions/fork, promotion to MAP_SHARED, and all references released. */
 vfs_page_reference_t inode={.references=1,.size=2*4096+77};
 memset(inode.data,'F',sizeof(inode.data));
 kernel_domains_enter(KDOMAIN_VFS);
 assert(pager_map_file(1,0x60000000,3*4096,3,true,&va,NULL,NULL,&inode,0));
 kernel_domains_exit(KDOMAIN_VFS);
 /* No file data is read eagerly. Two supervisor pages hold the region
    and file-source metadata; neither is an allocation in the user heap. */
 assert(!inode.reads && mm_vm_frame_free_pages()==45 && !heap_nodes);
 assert(regions->metadata_pages && file_sources);
 assert(pager_fault(1,va,false));
 uint32_t file_frame=find(1,va)->source_frame;
 assert(file_frame && !find(1,va)->frame && inode.reads==1);
 assert(pager_clone(1,2,va,3*4096));
 assert(pager_fault(2,va,false) && find(2,va)->source_frame==file_frame && inode.reads==1);
 assert(pager_fault(2,va,true));memset((void *)(uintptr_t)find(2,va)->frame,'D',4096);
 assert(*(unsigned char *)(uintptr_t)file_frame=='F' && inode.data[0]=='F');
 assert(pager_pin(1,va,4096,false));uint32_t clean_victim;
 assert(!reclaim_clean(&clean_victim));pager_unpin(1,va,4096);
 tlb_error=true;assert(!reclaim_clean(&clean_victim));tlb_error=false;
 assert(find(1,va)->source_frame==file_frame && (entries[1][0]&1));
 assert(reclaim_clean(&clean_victim) && clean_victim==file_frame);
 mm_vm_frame_free(clean_victim,1);
 assert(!find(1,va)->source_frame && !find(2,va)->source_frame);
 assert(pager_fault(1,va,false) && inode.reads==2);
 assert(*(unsigned char *)(uintptr_t)find(2,va)->frame=='D');
 const unsigned char changed='W';inode.data[12]='W';pager_file_written(&inode,12,&changed,1);
 assert(((unsigned char *)(uintptr_t)find(1,va)->source_frame)[12]=='W');
 assert(((unsigned char *)(uintptr_t)find(2,va)->frame)[12]=='D');
 inode.io_error=true;uint32_t available=mm_vm_frame_free_pages();
 assert(!pager_fault(1,va+4096,false) && mm_vm_frame_free_pages()==available);
 inode.io_error=false;
 assert(pager_protect(1,va+4096,4096,1));assert(!pager_fault(1,va+4096,true));
 assert(pager_fault(1,va+4096,false));assert(pager_fault(1,va+8192,false));
 uint8_t *tail=(uint8_t *)(uintptr_t)find(1,va+8192)->source_frame;
 assert(tail[76]=='F' && tail[77]==0 && tail[4095]==0);
 uint32_t shared_backing;assert(mm_vm_frame_alloc(3,&shared_backing));
 memcpy((void *)(uintptr_t)shared_backing,inode.data,inode.size);
 vfs_shared_mapping_t shared={shared_backing,3*4096,inode.size,1};
 kernel_domains_enter(KDOMAIN_VFS);
 assert(pager_pin(1,va,4096,false));assert(!pager_file_promote(&inode,&shared));pager_unpin(1,va,4096);
 tlb_error=true;assert(!pager_file_promote(&inode,&shared));tlb_error=false;
 assert(pager_file_promote(&inode,&shared));kernel_domains_exit(KDOMAIN_VFS);
 assert(shared.references==2 && find(1,va)->source_frame==shared_backing);
 ((uint8_t *)(uintptr_t)shared_backing)[20]='S';assert(((uint8_t *)(uintptr_t)find(1,va)->source_frame)[20]=='S');
 assert(((uint8_t *)(uintptr_t)find(2,va)->frame)[20]=='D');
 /* Discarding a private copy restores the inode's shared backing, rather
    than zeroing the file or exposing another process's private writes. */
 assert(pager_reset(2,va,4096) && !find(2,va)->frame && !find(2,va)->slot);
 assert(pager_fault(2,va,false) && find(2,va)->source_frame==shared_backing);
 assert(((uint8_t *)(uintptr_t)find(2,va)->source_frame)[20]=='S');
 assert(pager_unmap(1,va+4096,4096));assert(pager_unmap(1,va,4096));
 assert(inode.references==1);assert(pager_unmap(1,va+8192,4096));
 assert(pager_unmap(2,va,3*4096));pager_destroy(1);pager_destroy(2);
 assert(!inode.references && shared.references==1 && !file_sources && !vfs_depth);
 mm_vm_frame_free(shared_backing,3);assert(!heap_nodes && mm_vm_frame_free_pages()==47);
 vfs_page_reference_t short_inode={.references=1,.size=77};
 memset(short_inode.data,'S',sizeof(short_inode.data));
 kernel_domains_enter(KDOMAIN_VFS);
 assert(pager_map_file(1,0x60000000,2*4096,3,true,&va,NULL,NULL,&short_inode,0));
 kernel_domains_exit(KDOMAIN_VFS);
 assert(pager_fault(1,va,false) && !pager_fault_is_bus(1,va,false));
 uint32_t short_frame=find(1,va)->source_frame;
 assert(((unsigned char *)(uintptr_t)short_frame)[76]=='S' && ((unsigned char *)(uintptr_t)short_frame)[77]==0);
 assert(!pager_fault(1,va+4096,false) && pager_fault_is_bus(1,va+4096,false));
 assert(pager_fault_is_bus(1,va+4096,true));
 assert(pager_protect(1,va+4096,4096,0) && !pager_fault_is_bus(1,va+4096,false));
 assert(pager_protect(1,va+4096,4096,1) && !pager_fault_is_bus(1,va+4096,true));
 assert(pager_fault_is_bus(1,va+4096,false));
 assert(pager_unmap(1,va,2*4096));pager_destroy(1);assert(!short_inode.references && !file_sources && !heap_nodes && mm_vm_frame_free_pages()==47);
 fail_metadata_heap=true;
 assert(pager_map(1,0,4096,3,false,&va,NULL,NULL));
 assert(regions->metadata_pages && !heap_nodes);
 fill(1,va,0x35);assert(pager_unmap(1,va,4096));pager_destroy(1);
 fail_metadata_heap=false;
 assert(!heap_nodes && mm_vm_frame_free_pages()==47);
 assert(pager_map(1,0,3*4096,3,false,&va,NULL,NULL));
 fill(1,va,71);fill(1,va+4096,72);fill(1,va+8192,73);
 assert(pager_pin(1,va,4096,false));assert(!pager_reset(1,va,3*4096));pager_unpin(1,va,4096);
 tlb_error=true;assert(!pager_reset(1,va,3*4096));tlb_error=false;
 verify(1,va,71);verify(1,va+8192,73);
 assert(pager_protect(1,va,3*4096,1));
 unsigned reset_free=mm_vm_frame_free_pages();assert(pager_reset(1,va,3*4096));
 assert(mm_vm_frame_free_pages()==reset_free+3 && !pager_fault(1,va,true));
 verify(1,va,0);verify(1,va+8192,0);pager_destroy(1);
 assert(!heap_nodes && mm_vm_frame_free_pages()==47);
 /* Supervisor metadata must reclaim data before consuming table headroom.
    Reproduce the fork regression: 16 free frames, then an 8-page record. */
 memset(entries[1],0,sizeof(entries[1]));
 assert(pager_map(1,0,32*4096,3,false,&va,NULL,NULL));
 for(unsigned i=0;i<32;i++)fill(1,va+i*4096,i);
 assert(mm_vm_frame_free_pages()==16);
 void *metadata=resident_alloc(8*4096);assert(metadata && !heap_nodes);
 assert(mm_vm_frame_free_pages()>=16);
 verify(1,va,0);verify(1,va+31*4096,31);
 resident_free(metadata,8*4096);
 assert(pager_unmap(1,va,32*4096));pager_destroy(1);
 assert(!heap_nodes && mm_vm_frame_free_pages()==47 && !stats.used);
 puts("PASS supervisor metadata/table reserve, validated disk, slot exhaustion/reuse, actual pressure/integrity, failed swapoff, I/O rollback, TLB refusal, permissions, pin rollback, fork isolation, split unmap and process cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-swap-pager-') as folder:
 d=Path(folder);(d/'test.c').write_text(code)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Wno-misleading-indentation',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
