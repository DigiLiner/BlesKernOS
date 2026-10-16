#include "../include/swap.h"
#include "../include/block.h"
#include "../include/memory.h"
#include "../include/klock.h"
#include "../include/vga.h"
#include "../include/bfs4.h"

/* Explicit BKSWAP01 storage only: dedicated disks or a pinned BFS4 file.
 * No signature is written by the kernel. The first 4 KiB has no swap slot. */
static block_device_t *device;
static uint32_t storage_lba;
static uint8_t *bitmap;
static uint32_t hint;
static swap_stats_t stats;
static kmutex_t lock = KMUTEX_INITIALIZER;

static uint32_t crc32(const uint8_t *bytes, uint32_t length) {
    uint32_t crc=0xffffffffU;
    while(length--) {
        crc ^= *bytes++;
        for(uint32_t bit=0;bit<8;bit++) crc=(crc>>1)^((0U-(crc&1U))&0xedb88320U);
    }
    return ~crc;
}

static bool activate(block_device_t *candidate,uint32_t base,uint32_t sectors) {
    uint8_t header[512];
    if(!candidate || candidate->type!=BLOCK_DEVICE_ATA || candidate->read_only ||
       !candidate->write || candidate->sector_size!=512U || sectors<16U ||
       base>=candidate->sector_count || sectors>candidate->sector_count-base ||
       !block_read(candidate,base,1,header) || kmemcmp(header,"BKSWAP01",8))return false;
    uint32_t *fields=(uint32_t *)(void *)(header+8),slots=fields[2];
    if(fields[0]!=1U || fields[1]!=4096U || !slots || slots>262144U ||
       fields[3]!=8U || fields[4]!=sectors || slots>(sectors-8U)/8U ||
       *(uint32_t *)(void *)(header+508)!=crc32(header,508)) {
        kprintf("[SWAP] encabezado invalido en %s; almacenamiento ignorado\n",candidate->name);
        return false;
    }
    uint32_t physical=0;
    if(!mm_vm_frame_alloc(((slots+7U)/8U+4095U)/4096U,&physical))return false;
    bitmap=(uint8_t *)(uintptr_t)physical;
    storage_lba=base;device=candidate;stats.slots=slots;stats.enabled=true;
    return true;
}

void swap_init(void) {
    if(device) return;
    for(uint32_t i=0;i<block_count();i++) {
        block_device_t *candidate=block_at(i);
        if(!candidate || candidate->base_lba || !activate(candidate,0,candidate->sector_count))continue;
        kprintf("[SWAP] %s validado: %u slots de 4 KiB\n",device->name,stats.slots);
        return;
    }
}

void swap_init_file(void) {
    fat_fs_t fs;block_device_t *candidate;uint32_t base,sectors;
    if(device || !fat_get_current(&fs) ||
       !bfs4_swap_pin(&fs,"/SYSTEM/SWAP.BKS",&candidate,&base,&sectors))return;
    if(!activate(candidate,base,sectors)){bfs4_swap_unpin(&fs);return;}
    kprintf("[SWAP] /SYSTEM/SWAP.BKS en %s validado: %u slots de 4 KiB\n",device->name,stats.slots);
}

bool swap_enable(bool enabled) {
    kmutex_lock(&lock);
    bool ok=device && (enabled || !stats.used);
    if(ok) stats.enabled=enabled;
    kmutex_unlock(&lock);
    return ok;
}

uint32_t swap_slot_alloc(void) {
    uint32_t result=0;
    kmutex_lock(&lock);
    if(stats.enabled && stats.used<stats.slots) {
        for(uint32_t i=0;i<stats.slots;i++) {
            uint32_t index=(hint+i)%stats.slots;
            if(bitmap[index>>3]&(1U<<(index&7))) continue;
            bitmap[index>>3]|=1U<<(index&7);stats.used++;
            hint=(index+1U)%stats.slots;result=index+1U;break;
        }
    }
    kmutex_unlock(&lock);return result;
}

void swap_slot_free(uint32_t slot) {
    kmutex_lock(&lock);
    if(slot && slot<=stats.slots) {
        uint32_t index=slot-1U;
        if(bitmap[index>>3]&(1U<<(index&7))) {
            bitmap[index>>3]&=~(1U<<(index&7));stats.used--;hint=index;
        }
    }
    kmutex_unlock(&lock);
}

static bool page_io(uint32_t slot, void *page, bool write) {
    kmutex_lock(&lock);
    bool ok=false;
    if(page && stats.enabled && slot && slot<=stats.slots &&
       (bitmap[(slot-1U)>>3]&(1U<<((slot-1U)&7)))) {
        uint32_t lba=storage_lba+8U+(slot-1U)*8U;
        ok=write?block_write_volatile(device,lba,8,page):block_read(device,lba,8,page);
        if(ok) {if(write)stats.writes++;else stats.reads++;}
        else stats.io_errors++;
    }
    kmutex_unlock(&lock);return ok;
}
bool swap_page_write(uint32_t slot,const void *page){return page_io(slot,(void *)page,true);}
bool swap_page_read(uint32_t slot,void *page){return page_io(slot,page,false);}
void swap_stats(swap_stats_t *out){if(!out)return;kmutex_lock(&lock);*out=stats;kmutex_unlock(&lock);}
