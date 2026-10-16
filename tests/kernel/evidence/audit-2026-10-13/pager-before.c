#include "../include/pager.h"
#include "../include/swap.h"
#include "../include/memory.h"
#include "../include/klock.h"
#include "../include/task.h"
#include "../include/vfs.h"
#include "../include/kernel_domains.h"

/* Sparse inode cache: only touched file pages get frames or chunk metadata.
 * A promoted MAP_SHARED object owns its backing; it is never reclaimed here. */
typedef struct file_chunk {
    struct file_chunk *next;
    uint32_t index,frames[64];
} file_chunk_t;
typedef struct file_source {
    struct file_source *next;
    vfs_page_reference_t *reference;
    vfs_shared_mapping_t *shared;
    file_chunk_t *chunks;
    uint32_t references;
} file_source_t;
static file_source_t *file_sources;
static uint32_t file_cache_released;

typedef struct {uint32_t frame,slot,pins,source_frame;bool alive;} pager_page_t;
typedef struct pager_region {
    struct pager_region *next;
    uint32_t pid,address,count,hand,metadata_pages;
    file_source_t *file;
    uint32_t file_offset;
    pager_page_t pages[];
} pager_region_t;
static pager_region_t *regions;
/* Hot syscall buffers repeatedly touch the same mappings. All cache access
 * uses the pager lock; unlinking a region invalidates its borrowed pointers. */
static pager_region_t *region_cache[64];
typedef struct {uint32_t pid,first,last;bool valid;} pager_miss_t;
static pager_miss_t region_misses[64];
static void region_publish(pager_region_t *r) {
    region_misses[r->pid&63U].valid=false;
    r->next=regions;regions=r;
}
static kmutex_t lock=KMUTEX_INITIALIZER;
static uint32_t fault_count,evictions,failures;
static bool draining;
typedef struct {uint32_t pid,faults,evictions,failures;} process_stats_t;
static process_stats_t process_counters[TASK_MAX];
static process_stats_t *counter(uint32_t pid,bool create) {
    process_stats_t *free=NULL;
    for(uint32_t i=0;i<TASK_MAX;i++) {
        if(process_counters[i].pid==pid && pid)return &process_counters[i];
        if(!process_counters[i].pid && !free)free=&process_counters[i];
    }
    if(create && free){free->pid=pid;return free;}return NULL;
}

static bool region_contains(pager_region_t *r,uint32_t pid,uint32_t address) {
    return r && r->pid==pid && address>=r->address &&
        (address-r->address)/4096U<r->count &&
        r->pages[(address-r->address)/4096U].alive;
}
static pager_region_t *find_region(uint32_t pid,uint32_t address) {
    uint32_t index=(pid^(address>>16))&63U;
    pager_region_t *cached=region_cache[index];
    if(region_contains(cached,pid,address))return cached;
    pager_miss_t *miss=&region_misses[pid&63U];
    if(miss->valid && miss->pid==pid && address>=miss->first && address<=miss->last)
        return NULL;
    uint32_t first=0U,last=0xffffffffU;
    for(pager_region_t *r=regions;r;r=r->next) {
        if(region_contains(r,pid,address)){region_cache[index]=r;return r;}
        if(r->pid!=pid || !r->count)continue;
        uint64_t end=(uint64_t)r->address+(uint64_t)r->count*4096U;
        if(address<r->address) {
            if(r->address-1U<last)last=r->address-1U;
        } else if((uint64_t)address>=end) {
            if(end>first)first=(uint32_t)end;
        } else {
            /* A dead page in a partially unmapped region is only a one-page
             * gap. Keep searching: a newer mapping may occupy that hole. */
            uint32_t page=address&~4095U;
            if(page>first)first=page;
            if(page+4095U<last)last=page+4095U;
        }
    }
    /* Native ELF stacks and TinyX buffers are not pager regions. Reusing the
     * entire verified gap avoids a global scan for every page of a frame. */
    *miss=(pager_miss_t){pid,first,last,true};
    return NULL;
}
static pager_page_t *find(uint32_t pid,uint32_t address) {
    pager_region_t *r=find_region(pid,address);
    return r?&r->pages[(address-r->address)/4096U]:NULL;
}
static void region_forget(pager_region_t *r) {
    region_misses[r->pid&63U].valid=false;
    for(uint32_t i=0;i<64U;i++)if(region_cache[i]==r)region_cache[i]=NULL;
}
static void *resident_alloc(uint32_t size) {
    void *p=kzalloc(size);
    if(p){(void)mm_set_allocation_owner(p,0U);(void)mm_set_allocation_subsystem(p,MM_SUBSYSTEM_VM);}
    return p;
}
static uint32_t *file_frame_slot(file_source_t *s,uint32_t offset,bool create) {
    uint32_t index=offset/(64U*4096U);
    file_chunk_t *c=s->chunks;
    while(c && c->index!=index)c=c->next;
    if(!c && create) {
        c=resident_alloc(sizeof(*c));
        if(c){c->index=index;c->next=s->chunks;s->chunks=c;}
    }
    return c?&c->frames[(offset/4096U)%64U]:NULL;
}
static void file_drop(file_source_t *s) {
    if(!s || --s->references)return;
    file_source_t **link=&file_sources;
    while(*link && *link!=s)link=&(*link)->next;
    if(*link)*link=s->next;
    while(s->chunks) {
        file_chunk_t *c=s->chunks;s->chunks=c->next;
        for(uint32_t i=0;i<64;i++)if(c->frames[i]){mm_vm_frame_free(c->frames[i],1);file_cache_released++;}
        kfree(c);
    }
    if(s->shared)(void)vfs_shared_release(s->shared);
    vfs_page_reference_release(s->reference);kfree(s);
}
/* Inspect under the pager lock, then acquire VFS before rechecking/loading.
 * No file reference survives this lookup without its region owning it. */
static bool file_guard(uint32_t pid,uint32_t address,uint32_t length) {
    bool needed=false;
    kmutex_lock(&lock);
    pager_region_t *first=find_region(pid,address);
    pager_miss_t *miss=&region_misses[pid&63U];
    /* The complete buffer lies in a verified gap: no live file mapping can
     * require VFS serialization. Buffers crossing its edge still use the scan. */
    if(length && !first && miss->valid && miss->pid==pid &&
       address>=miss->first && (uint64_t)address+length-1U<=miss->last)
        goto inspected;
    /* Only bypass the general scan when every page belongs to this mapping.
     * A partially unmapped region may contain a new file mapping in a hole. */
    if(length && first && (uint64_t)address+length<=
       (uint64_t)first->address+(uint64_t)first->count*4096U) {
        bool complete=true;
        uint32_t start=(address-first->address)/4096U;
        uint32_t end=((uint64_t)address+length-1U-first->address)/4096U;
        for(uint32_t i=start;i<=end;i++)if(!first->pages[i].alive){complete=false;break;}
        if(complete){needed=first->file!=NULL;goto inspected;}
    }
    for(pager_region_t *r=regions;r;r=r->next)
        if(r->pid==pid && r->file && (uint64_t)address<(uint64_t)r->address+r->count*4096U &&
           (uint64_t)r->address<(uint64_t)address+length){needed=true;break;}
inspected:
    kmutex_unlock(&lock);
    if(needed)kernel_domains_enter(KDOMAIN_VFS);
    return needed;
}
static void file_unguard(bool guarded) {
    if(guarded)kernel_domains_exit(KDOMAIN_VFS);
}
static bool reclaim_clean(uint32_t *frame) {
    for(file_source_t *s=file_sources;s;s=s->next) {
        if(s->shared)continue;
        for(file_chunk_t *c=s->chunks;c;c=c->next)for(uint32_t i=0;i<64;i++) {
            uint32_t physical=c->frames[i];if(!physical)continue;
            bool pinned=false,ok=true;
            for(pager_region_t *r=regions;r;r=r->next)if(r->file==s)
                for(uint32_t n=0;n<r->count;n++) {
                    pager_page_t *p=&r->pages[n];
                    if(p->alive && p->source_frame==physical && !p->frame && !p->slot && p->pins)pinned=true;
                }
            if(pinned)continue;
            for(pager_region_t *r=regions;ok && r;r=r->next)if(r->file==s)
                for(uint32_t n=0;ok && n<r->count;n++) {
                    pager_page_t *p=&r->pages[n];
                    if(p->alive && p->source_frame==physical && !p->frame && !p->slot)
                        ok=paging_pager_replace(r->pid,r->address+n*4096U,0,false);
                }
            if(!ok) {
                for(pager_region_t *r=regions;r;r=r->next)if(r->file==s)
                    for(uint32_t n=0;n<r->count;n++) {
                        pager_page_t *p=&r->pages[n];
                        if(p->alive && p->source_frame==physical && !p->frame && !p->slot)
                            (void)paging_pager_replace_cow(r->pid,r->address+n*4096U,physical,true,true);
                    }
                continue;
            }
            for(pager_region_t *r=regions;r;r=r->next)if(r->file==s)
                for(uint32_t n=0;n<r->count;n++)if(r->pages[n].source_frame==physical)r->pages[n].source_frame=0;
            c->frames[i]=0;*frame=physical;file_cache_released++;evictions++;return true;
        }
    }
    return false;
}

/* Metadata, page tables, kernel heap, shared/file/COW pages and I/O buffers
 * never enter this list. Eviction allocates neither metadata nor heap. */
static bool reclaim(uint32_t *frame) {
    if(draining)return false;
    if(reclaim_clean(frame))return true;
    for(pager_region_t *r=regions;r;r=r->next)for(uint32_t n=0;n<r->count;n++) {
        uint32_t index=r->hand++%r->count;
        pager_page_t *p=&r->pages[index];
        if(!p->alive || !p->frame || p->pins)continue;
        uint32_t slot=swap_slot_alloc();
        if(!slot)return false;
        uint32_t va=r->address+index*4096U;
        /* First remove the writable translation and wait for all CPUs. The
         * page cannot change during disk I/O, including writes from Ring 0. */
        if(!paging_pager_replace(r->pid,va,0,false)) {swap_slot_free(slot);continue;}
        if(!swap_page_write(slot,(void *)(uintptr_t)p->frame)) {
            (void)paging_pager_replace(r->pid,va,p->frame,true);
            swap_slot_free(slot);continue;
        }
        *frame=p->frame;p->frame=0;p->slot=slot;evictions++;
        process_stats_t *c=counter(r->pid,false);if(c)c->evictions++;
        return true;
    }
    return false;
}

static bool load(uint32_t pid,uint32_t va,pager_page_t *p,bool write) {
    process_stats_t *c=counter(pid,false);
    uint32_t entry=paging_pager_entry(pid,va);
    if(!(entry&4U) || (write && !(entry&(2U|0x800U))))return false;
    if(p->frame)return !write || !(entry&0x800U) || paging_pager_replace(pid,va,p->frame,true);
    if(p->source_frame && !p->slot && !write)
        return paging_pager_replace_cow(pid,va,p->source_frame,true,true);
    pager_region_t *r=find_region(pid,va);
    file_source_t *file=r?r->file:NULL;
    uint32_t offset=file?r->file_offset+va-r->address:0U;
    uint32_t *cache=NULL;
    if(file && !p->slot) {
        /* A clean shared mapping remains coherent without a copied cache. */
        if(file->shared && offset>=file->shared->length)return false;
        if(file->shared)p->source_frame=file->shared->backing+offset;
        else {cache=file_frame_slot(file,offset,!write);if(cache)p->source_frame=*cache;}
        if(!write && p->source_frame)
            return paging_pager_replace_cow(pid,va,p->source_frame,true,true);
        if(!write && !file->shared && !cache)return false;
        if(offset>=vfs_page_reference_size(file->reference))return false;
    }
    /* Reclaim must not recycle the clean source while a private copy reads it. */
    __sync_add_and_fetch(&p->pins,1U);
    uint32_t frame=0;
    /* Leave 64 KiB available for resident page tables/controller work. */
    bool allocated=mm_vm_frame_free_pages()>16U && mm_vm_frame_alloc(1,&frame);
    if(!allocated && !reclaim(&frame)) {
        /* With swap unavailable, unused physical frames remain useful. */
        if(!mm_vm_frame_alloc(1,&frame)){__sync_sub_and_fetch(&p->pins,1U);failures++;if(c)c->failures++;return false;}
    }
    bool ok=p->slot?swap_page_read(p->slot,(void *)(uintptr_t)frame):true;
    if(!p->slot) {
        if(p->source_frame)kmemcpy((void *)(uintptr_t)frame,(void *)(uintptr_t)p->source_frame,4096);
        else {
            kmemset((void *)(uintptr_t)frame,0,4096);
            if(file) {
                uint32_t got=0;
                ok=vfs_page_reference_read(file->reference,offset,(void *)(uintptr_t)frame,4096,&got) && got>0;
            }
        }
    }
    bool clean=file && !p->slot && !write;
    if(!ok || !paging_pager_replace_cow(pid,va,frame,true,clean)) {
        __sync_sub_and_fetch(&p->pins,1U);
        mm_vm_frame_free(frame,1);failures++;if(c)c->failures++;return false;
    }
    if(p->slot)swap_slot_free(p->slot);
    p->slot=0;
    if(clean){*cache=frame;p->source_frame=frame;}else p->frame=frame;
    __sync_sub_and_fetch(&p->pins,1U);
    fault_count++;if(c)c->faults++;return true;
}
static pager_region_t *region_allocate(uint32_t count) {
    uint32_t bytes=sizeof(pager_region_t)+count*sizeof(pager_page_t);
    pager_region_t *r=kzalloc(bytes);
    if(r) {
        /* Metadata outlives the thread that creates a child's mappings. */
        (void)mm_set_allocation_owner(r,0U);
        (void)mm_set_allocation_subsystem(r,MM_SUBSYSTEM_VM);
        return r;
    }
    uint32_t physical=0,pages=(bytes+4095U)/4096U;
    if(!pager_frame_alloc(pages,&physical))return NULL;
    r=(pager_region_t *)(uintptr_t)physical;r->metadata_pages=pages;return r;
}
static void region_free(pager_region_t *r) {
    file_drop(r->file);
    if(r->metadata_pages)mm_vm_frame_free((uint32_t)(uintptr_t)r,r->metadata_pages);
    else kfree(r);
}

bool pager_map(uint32_t pid,uint32_t requested,uint32_t length,uint32_t prot,bool exact,
               uint32_t *address,paging_range_skip_t skip,void *context) {
    if(!length || (length&4095U))return false;
    pid=paging_process_owner(pid);
    uint32_t count=length/4096U;
    pager_region_t *r=region_allocate(count);
    if(!r)return false;
    if(!paging_pager_reserve(pid,requested,length,prot!=0,(prot&2U)!=0,exact,address,skip,context)) {
        region_free(r);return false;
    }
    r->pid=pid;r->address=*address;r->count=count;
    for(uint32_t i=0;i<count;i++)r->pages[i].alive=true;
    kmutex_lock(&lock);(void)counter(pid,true);
    region_publish(r);kmutex_unlock(&lock);return true;
}

bool pager_map_file(uint32_t pid,uint32_t requested,uint32_t length,uint32_t prot,bool exact,
                    uint32_t *address,paging_range_skip_t skip,void *context,
                    vfs_page_reference_t *reference,uint32_t offset) {
    if(!reference || !length || ((length|offset)&4095U) || offset+length<offset)return false;
    pid=paging_process_owner(pid);
    pager_region_t *r=region_allocate(length/4096U);
    if(!r)return false;
    kmutex_lock(&lock);
    file_source_t *s=file_sources;bool created=false;
    while(s && s->reference!=reference)s=s->next;
    if(!s) {
        s=resident_alloc(sizeof(*s));created=s!=NULL;
        if(s){s->reference=reference;s->next=file_sources;file_sources=s;}
    }
    bool ok=s && paging_pager_reserve(pid,requested,length,prot!=0,(prot&2U)!=0,exact,address,skip,context);
    if(ok) {
        s->references++;
        r->pid=pid;r->address=*address;r->count=length/4096U;r->file=s;r->file_offset=offset;
        for(uint32_t i=0;i<r->count;i++)r->pages[i].alive=true;
        (void)counter(pid,true);region_publish(r);
        /* Existing source already holds its own reference. */
        if(!created)vfs_page_reference_release(reference);
    } else if(created){file_sources=s->next;kfree(s);}
    kmutex_unlock(&lock);
    if(!ok)region_free(r);
    return ok;
}
/* Called before exposing a newly acquired MAP_SHARED mapping to user code.
 * First revoke every old clean alias and acknowledge SMP TLB shootdowns;
 * only then replace backing and free the independently owned cache frames. */
bool pager_file_promote(vfs_page_reference_t *reference,vfs_shared_mapping_t *shared) {
    kmutex_lock(&lock);
    file_source_t *s=file_sources;
    while(s && s->reference!=reference)s=s->next;
    if(!s || s->shared==shared){kmutex_unlock(&lock);return true;}
    if(s->shared || shared->references==0xffffffffU){kmutex_unlock(&lock);return false;}
    bool ok=true;
    for(pager_region_t *r=regions;ok && r;r=r->next)if(r->file==s)
        for(uint32_t n=0;ok && n<r->count;n++) {
            pager_page_t *p=&r->pages[n];
            if(p->alive && !p->frame && !p->slot &&
               (p->pins || r->file_offset+n*4096U>=shared->length))ok=false;
        }
    if(!ok){kmutex_unlock(&lock);return false;}
    for(pager_region_t *r=regions;ok && r;r=r->next)if(r->file==s)
        for(uint32_t n=0;ok && n<r->count;n++) {
            pager_page_t *p=&r->pages[n];
            if(p->alive && p->source_frame && !p->frame && !p->slot)
                ok=paging_pager_replace(r->pid,r->address+n*4096U,0,false);
        }
    if(!ok) {
        for(pager_region_t *r=regions;r;r=r->next)if(r->file==s)
            for(uint32_t n=0;n<r->count;n++) {
                pager_page_t *p=&r->pages[n];
                if(p->alive && p->source_frame && !p->frame && !p->slot)
                    (void)paging_pager_replace_cow(r->pid,r->address+n*4096U,p->source_frame,true,true);
            }
        kmutex_unlock(&lock);return false;
    }
    shared->references++;s->shared=shared;
    for(pager_region_t *r=regions;r;r=r->next)if(r->file==s)
        for(uint32_t n=0;n<r->count;n++) {
            pager_page_t *p=&r->pages[n];
            bool mapped=p->source_frame && !p->frame && !p->slot;
            p->source_frame=0;
            if(p->alive && mapped) {
                p->source_frame=shared->backing+r->file_offset+n*4096U;
                (void)paging_pager_replace_cow(r->pid,r->address+n*4096U,p->source_frame,true,true);
            }
        }
    while(s->chunks) {
        file_chunk_t *c=s->chunks;s->chunks=c->next;
        for(uint32_t n=0;n<64;n++)if(c->frames[n])mm_vm_frame_free(c->frames[n],1);
        kfree(c);
    }
    kmutex_unlock(&lock);return true;
}
void pager_file_written(vfs_page_reference_t *reference,uint32_t offset,const void *data,uint32_t length) {
    if(!length || offset+length<offset)return;
    kmutex_lock(&lock);
    for(file_source_t *s=file_sources;s;s=s->next)if(s->reference==reference && !s->shared)
        for(file_chunk_t *c=s->chunks;c;c=c->next)for(uint32_t n=0;n<64;n++)if(c->frames[n]) {
            uint32_t start=(c->index*64U+n)*4096U,end=start+4096U;
            uint32_t first=offset>start?offset:start,last=offset+length<end?offset+length:end;
            if(last>first)kmemcpy((void *)(uintptr_t)(c->frames[n]+first-start),
                                 (const uint8_t *)data+first-offset,last-first);
        }
    kmutex_unlock(&lock);
}
void pager_file_cache_perf(uint32_t *entries,uint32_t *bytes,uint32_t *released) {
    *entries=*bytes=0;kmutex_lock(&lock);
    for(file_source_t *s=file_sources;s;s=s->next) {
        (*entries)++;
        for(file_chunk_t *c=s->chunks;c;c=c->next)
            for(uint32_t n=0;n<64;n++)if(c->frames[n])*bytes+=4096U;
    }
    *released=file_cache_released;kmutex_unlock(&lock);
}

bool pager_fault(uint32_t pid,uint32_t address,bool write) {
    pid=paging_process_owner(pid);address&=~4095U;
    bool guarded=file_guard(pid,address,4096U);
    kmutex_lock(&lock);
    pager_page_t *p=find(pid,address);
    bool ok=p && load(pid,address,p,write);
    kmutex_unlock(&lock);file_unguard(guarded);return ok;
}
bool pager_fault_is_bus(uint32_t pid,uint32_t address,bool write) {
    pid=paging_process_owner(pid);address&=~4095U;
    bool guarded=file_guard(pid,address,4096U);
    kmutex_lock(&lock);
    pager_region_t *r=find_region(pid,address);
    uint32_t entry=r?paging_pager_entry(pid,address):0U;
    bool bus=r && r->file && !r->file->shared && (entry&4U) &&
        (!write || (entry&(2U|0x800U))) &&
        r->file_offset+address-r->address>=vfs_page_reference_size(r->file->reference);
    kmutex_unlock(&lock);file_unguard(guarded);return bus;
}
bool pager_adopt_cow(uint32_t pid,uint32_t address,uint32_t length,uint32_t source) {
    if(!length || ((address|length|source)&4095U) ||
       address+length<address || source+length<source)return false;
    pid=paging_process_owner(pid);
    uint32_t count=length/4096U;
    pager_region_t *r=region_allocate(count);
    if(!r)return false;
    r->pid=pid;r->address=address;r->count=count;
    for(uint32_t i=0;i<count;i++) {
        r->pages[i].alive=true;r->pages[i].source_frame=source+i*4096U;
    }
    kmutex_lock(&lock);
    bool ok=true;
    for(uint32_t i=0;ok && i<count;i++)if(find(pid,address+i*4096U))ok=false;
    if(ok)ok=paging_pager_adopt(pid,address,length);
    if(ok){(void)counter(pid,true);region_publish(r);}
    kmutex_unlock(&lock);
    if(!ok)region_free(r);
    return ok;
}

bool pager_pin(uint32_t pid,uint32_t address,uint32_t length,bool write) {
    if(!length)return true;
    if(address+length<address)return false;
    pid=paging_process_owner(pid);
    uint32_t first=address&~4095U,end=address+length;
    bool guarded=file_guard(pid,first,end-first);
    kmutex_lock(&lock);
    uint32_t va;
    for(va=first;va<end;va+=4096U) {
        pager_page_t *p=find(pid,va);
        if(!p)continue;
        if(!load(pid,va,p,write))break;
        __sync_add_and_fetch(&p->pins,1U);
        if(va>0xffffffffU-4096U)break;
    }
    bool ok=va>=end;
    if(!ok)for(uint32_t undo=first;undo<va;undo+=4096U) {
        pager_page_t *p=find(pid,undo);if(p)__sync_sub_and_fetch(&p->pins,1U);
    }
    kmutex_unlock(&lock);file_unguard(guarded);return ok;
}
bool pager_pin_word(uint32_t pid,uint32_t address,void **token) {
    *token=NULL;
    if(address&3U)return false;
    pid=paging_process_owner(pid);
    bool guarded=file_guard(pid,address&~4095U,4096U);
    kmutex_lock(&lock);
    pager_page_t *p=find(pid,address&~4095U);
    bool ok=!p || load(pid,address&~4095U,p,false);
    if(ok && p){__sync_add_and_fetch(&p->pins,1U);*token=p;}
    kmutex_unlock(&lock);file_unguard(guarded);return ok;
}
void pager_release_word(void *token) {
    if(token)__sync_sub_and_fetch(&((pager_page_t *)token)->pins,1U);
}
void pager_unpin(uint32_t pid,uint32_t address,uint32_t length) {
    pid=paging_process_owner(pid);
    kmutex_lock(&lock);
    for(uint32_t va=address&~4095U;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);if(p && p->pins)__sync_sub_and_fetch(&p->pins,1U);
        if(va>0xffffffffU-4096U)break;
    }
    kmutex_unlock(&lock);
}

bool pager_unmap(uint32_t pid,uint32_t address,uint32_t length) {
    pid=paging_process_owner(pid);
    bool guarded=file_guard(pid,address,length);
    kmutex_lock(&lock);
    for(uint32_t va=address;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);
        if(p && p->pins){kmutex_unlock(&lock);file_unguard(guarded);return false;}
    }
    /* Revoke every resident translation before removing its metadata. On a
     * stalled CPU, restore the range so munmap can be retried without leaks. */
    for(uint32_t va=address;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);
        if(p && (p->frame || p->source_frame) && !paging_pager_replace(pid,va,0,false)) {
            for(uint32_t undo=address;undo<va;undo+=4096U) {
                pager_page_t *prior=find(pid,undo);
                if(prior && (prior->frame || prior->source_frame))
                    (void)paging_pager_replace_cow(pid,undo,prior->frame?prior->frame:prior->source_frame,true,!prior->frame);
            }
            kmutex_unlock(&lock);file_unguard(guarded);return false;
        }
    }
    if(!paging_process_unmap(pid,address,length)){kmutex_unlock(&lock);file_unguard(guarded);return false;}
    for(uint32_t va=address;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);if(!p)continue;
        if(p->frame)mm_vm_frame_free(p->frame,1);
        if(p->slot)swap_slot_free(p->slot);
        kmemset(p,0,sizeof(*p));
    }
    pager_region_t **link=&regions;
    while(*link) {
        pager_region_t *r=*link;bool alive=false;
        for(uint32_t i=0;i<r->count;i++)if(r->pages[i].alive){alive=true;break;}
        if(!alive){*link=r->next;region_forget(r);region_free(r);}else link=&r->next;
    }
    kmutex_unlock(&lock);file_unguard(guarded);return true;
}

void pager_destroy(uint32_t pid) {
    bool guarded=file_guard(pid,0U,0xffffffffU);
    kmutex_lock(&lock);
    process_stats_t *c=counter(pid,false);if(c)kmemset(c,0,sizeof(*c));
    pager_region_t **link=&regions;
    while(*link) {
        pager_region_t *r=*link;
        if(r->pid!=pid){link=&r->next;continue;}
        for(uint32_t i=0;i<r->count;i++) {
            if(r->pages[i].frame)mm_vm_frame_free(r->pages[i].frame,1);
            if(r->pages[i].slot)swap_slot_free(r->pages[i].slot);
        }
        *link=r->next;region_forget(r);region_free(r);
    }
    kmutex_unlock(&lock);
    file_unguard(guarded);
}

void pager_swap_ids(uint32_t a,uint32_t b) {
    kmutex_lock(&lock);
    kmemset(region_cache,0,sizeof(region_cache));
    kmemset(region_misses,0,sizeof(region_misses));
    process_stats_t *first=counter(a,false),*second=counter(b,false);
    if(first)first->pid=b;
    if(second)second->pid=a;
    for(pager_region_t *r=regions;r;r=r->next) {
        if(r->pid==a)r->pid=b;else if(r->pid==b)r->pid=a;
    }
    kmutex_unlock(&lock);
}

bool pager_swapoff(void) {
    kmutex_lock(&lock);draining=true;bool ok=true;
    for(pager_region_t *r=regions;ok && r;r=r->next)for(uint32_t i=0;ok && i<r->count;i++) {
        pager_page_t *p=&r->pages[i];
        if(!p->alive || !p->slot)continue;
        /* PROT_NONE remains protected; temporarily restore its page through
         * a physical alias without granting a user translation. */
        uint32_t frame=0;
        ok=mm_vm_frame_alloc(1,&frame);
        if(!ok)break;
        ok=swap_page_read(p->slot,(void *)(uintptr_t)frame) &&
            paging_pager_replace(r->pid,r->address+i*4096U,frame,true);
        if(!ok){mm_vm_frame_free(frame,1);break;}
        swap_slot_free(p->slot);p->slot=0;p->frame=frame;
    }
    if(ok)ok=swap_enable(false);
    draining=false;kmutex_unlock(&lock);return ok;
}

void pager_stats(uint32_t pid,pager_stats_t *out) {
    if(!out)return;
    if(pid)pid=paging_process_owner(pid);
    kmutex_lock(&lock);kmemset(out,0,sizeof(*out));
    for(pager_region_t *r=regions;r;r=r->next)if(!pid || r->pid==pid)
        for(uint32_t i=0;i<r->count;i++)if(r->pages[i].alive) {
            out->virtual_pages++;
            if(r->pages[i].frame || (r->pages[i].source_frame &&
               (paging_pager_entry(r->pid,r->address+i*4096U)&1U)))out->resident_pages++;
            if(r->pages[i].frame) {
                if(r->file)out->file_private_pages++;else out->anonymous_pages++;
            } else if(r->pages[i].source_frame &&
                      (paging_pager_entry(r->pid,r->address+i*4096U)&1U)) {
                if(r->file)out->file_clean_pages++;else out->borrowed_pages++;
            }
            if(r->pages[i].slot)out->swapped_pages++;
        }
    if(!pid){out->faults=fault_count;out->evictions=evictions;out->failures=failures;}
    else {process_stats_t *c=counter(pid,false);if(c) {
        out->faults=c->faults;out->evictions=c->evictions;out->failures=c->failures;
    }}
    kmutex_unlock(&lock);
}

bool pager_protect(uint32_t pid,uint32_t address,uint32_t length,uint32_t prot) {
    pid=paging_process_owner(pid);
    kmutex_lock(&lock);
    bool ok=true;
    for(uint32_t va=address;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);if(!p || p->pins){ok=false;break;}
    }
    for(uint32_t va=address;ok && va<address+length;) {
        pager_page_t *p=find(pid,va);
        pager_region_t *region=find_region(pid,va);
        bool cow=(p->source_frame || (region && region->file)) && !p->frame && !p->slot;
        uint32_t end=va+4096U;
        while(end<address+length) {
            pager_page_t *next=find(pid,end);
            pager_region_t *next_region=find_region(pid,end);
            if(((next->source_frame || (next_region && next_region->file)) && !next->frame && !next->slot)!=cow)break;
            end+=4096U;
        }
        if(cow)ok=paging_process_cow_access(pid,va,end-va,prot!=0,(prot&2U)!=0);
        else ok=paging_process_access(pid,va,end-va,prot!=0,(prot&2U)!=0);
        va=end;
    }
    kmutex_unlock(&lock);return ok;
}
bool pager_reset(uint32_t pid,uint32_t address,uint32_t length) {
    if(!length)return true;
    if((address|length)&4095U || address+length<address)return false;
    pid=paging_process_owner(pid);
    bool guarded=file_guard(pid,address,length);
    kmutex_lock(&lock);
    for(uint32_t va=address;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);
        if(!p || p->pins){kmutex_unlock(&lock);file_unguard(guarded);return false;}
    }
    for(uint32_t va=address;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);
        if((p->frame || p->source_frame) && !paging_pager_replace(pid,va,0,false)) {
            for(uint32_t undo=address;undo<va;undo+=4096U) {
                pager_page_t *prior=find(pid,undo);
                if(prior->frame || prior->source_frame)
                    (void)paging_pager_replace_cow(pid,undo,prior->frame?prior->frame:prior->source_frame,true,!prior->frame);
            }
            kmutex_unlock(&lock);file_unguard(guarded);return false;
        }
    }
    for(uint32_t va=address;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);
        if(p->frame)mm_vm_frame_free(p->frame,1);
        if(p->slot)swap_slot_free(p->slot);
        p->frame=p->slot=0;
        /* Borrowed cache frames remain owned by the inode cache. A private
         * file view refaults from that source; anonymous pages refault zero. */
    }
    kmutex_unlock(&lock);file_unguard(guarded);return true;
}

bool pager_discard(uint32_t pid,uint32_t address,uint32_t length) {
    if(!length)return true;
    if((address|length)&4095U || address+length<address)return false;
    pid=paging_process_owner(pid);kmutex_lock(&lock);
    bool ok=true;
    for(uint32_t va=address;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);
        if(!p || p->pins){ok=false;break;}
    }
    if(!ok){kmutex_unlock(&lock);return false;}
    for(uint32_t va=address;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);
        if(p->frame && !paging_pager_replace(pid,va,0,false)) {
            for(uint32_t undo=address;undo<va;undo+=4096U) {
                pager_page_t *prior=find(pid,undo);
                if(prior->frame)(void)paging_pager_replace(pid,undo,prior->frame,true);
            }
            kmutex_unlock(&lock);return false;
        }
    }
    if(!paging_process_access(pid,address,length,false,false)) {
        for(uint32_t va=address;va<address+length;va+=4096U) {
            pager_page_t *p=find(pid,va);
            if(p->frame)(void)paging_pager_replace(pid,va,p->frame,true);
        }
        kmutex_unlock(&lock);return false;
    }
    for(uint32_t va=address;va<address+length;va+=4096U) {
        pager_page_t *p=find(pid,va);
        if(p->frame)mm_vm_frame_free(p->frame,1);
        if(p->slot)swap_slot_free(p->slot);
        p->frame=p->slot=0;
    }
    kmutex_unlock(&lock);return true;
}

bool pager_clone(uint32_t parent,uint32_t child,uint32_t address,uint32_t length) {
    uint32_t mapped=0;
    if(!pager_map(child,address,length,3,true,&mapped,NULL,NULL))return false;
    kmutex_lock(&lock);bool ok=true;
    pager_region_t *source=find_region(parent,address),*target=find_region(child,address);
    if(source && source->file) {
        source->file->references++;
        target->file=source->file;target->file_offset=source->file_offset+address-source->address;
    }
    for(uint32_t va=address;ok && va<address+length;va+=4096U) {
        pager_page_t *src=find(parent,va),*dst=find(child,va);
        if(!src || !dst){ok=false;break;}
        uint32_t entry=paging_pager_entry(parent,va);
        dst->source_frame=src->source_frame;
        if(src->frame || src->slot) {
            __sync_add_and_fetch(&src->pins,1U);
            uint32_t frame=0;
            ok=mm_vm_frame_alloc(1,&frame) || reclaim(&frame);
            if(ok) {
                if(src->frame)kmemcpy((void *)(uintptr_t)frame,(void *)(uintptr_t)src->frame,4096);
                else ok=swap_page_read(src->slot,(void *)(uintptr_t)frame);
                if(ok)ok=paging_pager_replace(child,va,frame,true);
                if(ok)dst->frame=frame;else mm_vm_frame_free(frame,1);
            }
            __sync_sub_and_fetch(&src->pins,1U);
        }
        if(ok) {
            bool cow=(src->source_frame || (source && source->file)) && !src->frame && !src->slot;
            if(cow)ok=paging_process_cow_access(child,va,4096,(entry&4U)!=0,(entry&(2U|0x800U))!=0);
            else ok=paging_process_access(child,va,4096,(entry&4U)!=0,(entry&2U)!=0);
        }
    }
    kmutex_unlock(&lock);
    if(!ok)(void)pager_unmap(child,address,length);
    return ok;
}

/* Process/VFS context only. Page-table and IRQ allocation paths continue to
 * use mm_vm_frame_alloc directly, avoiding an inverse paging -> pager lock. */
bool pager_frame_alloc(uint32_t pages,uint32_t *physical) {
    if(physical)*physical=0;
    if(!physical || !pages || pages>mm_vm_frame_total_pages())return false;
    kmutex_lock(&lock);
    bool ok=mm_vm_frame_alloc(pages,physical);
    for(uint32_t i=0;!ok && i<mm_vm_frame_total_pages();i++) {
        uint32_t frame=0;if(!reclaim(&frame))break;
        mm_vm_frame_free(frame,1);ok=mm_vm_frame_alloc(pages,physical);
    }
    kmutex_unlock(&lock);return ok;
}
