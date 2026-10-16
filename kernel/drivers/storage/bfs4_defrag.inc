/* Called only under the VFS/driver domain. Each step transfers at most 32
 * blocks. Source extents remain live until the verified destination is
 * published in the inode sector; cancellation never discards source data. */
typedef struct {
    bfs4_volume_t *volume;
    bk_defrag_info_t info;
    uint8_t *verified;
    uint32_t next_inode, inode, blocks, position, destination;
    bool uncertain;
    bfs4_inode_t original;
    bfs4_extent_t extents[BFS4_EXTENTS + BFS4_INDIRECT_EXTENTS];
    uint8_t source[BFS4_BLOCK_SIZE], target[BFS4_BLOCK_SIZE];
} bfs4_defrag_job_t;
static bfs4_defrag_job_t g_defrag;

static bool bfs4_defrag_allows_clean(bfs4_volume_t *v) {
    return g_defrag.volume!=v || (!g_defrag.destination && !g_defrag.uncertain);
}
static void bfs4_defrag_forget(bfs4_volume_t *v) {
    if (g_defrag.volume!=v) return;
    if (g_defrag.verified) kfree(g_defrag.verified);
    kmemset(&g_defrag,0,sizeof(g_defrag));
}
static void bfs4_defrag_map(void) {
    bfs4_volume_t *v=g_defrag.volume;
    if (!v || !g_defrag.verified) return;
    g_defrag.info.free_blocks=v->super.free_blocks;
    for (uint32_t cell=0;cell<BK_DEFRAG_CELLS;cell++) {
        uint32_t first=(uint32_t)((uint64_t)cell*v->super.total_blocks/BK_DEFRAG_CELLS);
        uint32_t end=(uint32_t)((uint64_t)(cell+1)*v->super.total_blocks/BK_DEFRAG_CELLS);
        bool used=false, pending=false;
        for (uint32_t b=first;b<end;b++) {
            if (b<v->super.data_start || !bfs4_bit(v->block_bitmap,b)) continue;
            used=true;
            if (!bfs4_bit(g_defrag.verified,b)) pending=true;
        }
        uint8_t state=end<=v->super.data_start ? BK_DEFRAG_RESERVED
            : !used ? BK_DEFRAG_FREE : pending ? BK_DEFRAG_PENDING : BK_DEFRAG_PROCESSED;
        if (g_defrag.info.error==BK_DEFRAG_IO_ERROR &&
            g_defrag.info.failed_block>=first && g_defrag.info.failed_block<end)
            state=BK_DEFRAG_FAILED;
        g_defrag.info.cells[cell]=state;
    }
}
static void bfs4_defrag_invalidate(bfs4_volume_t *v,uint32_t first,uint32_t count) {
    if (g_defrag.volume!=v || !g_defrag.verified) return;
    for (uint32_t i=0;i<count && first+i<v->super.total_blocks;i++)
        bfs4_set_bit(g_defrag.verified,first+i,false);
}
static bool bfs4_defrag_release_destination(void) {
    if (!g_defrag.destination) return true;
    uint32_t first=g_defrag.destination;
    /* Mark reservations as retained on failure, rather than risking reuse. */
    if (!bfs4_free_extent(g_defrag.volume,first,g_defrag.blocks)) {
        for (uint32_t i=0;i<g_defrag.blocks;i++)
            bfs4_set_bit(g_defrag.volume->block_bitmap,first+i,true);
        g_defrag.destination=0;
        g_defrag.uncertain=true;
        return false;
    }
    g_defrag.destination=0;
    return bfs4_write_super(g_defrag.volume);
}
static void bfs4_defrag_fail(uint32_t error,uint32_t block) {
    g_defrag.info.state=BK_DEFRAG_ERROR;
    g_defrag.info.error=error;
    g_defrag.info.failed_block=block;
    (void)bfs4_defrag_release_destination();
    bfs4_defrag_map();
}
/* A normal writer wins: abandon its incomplete relocation before modifying
 * the live inode. This also handles rename, truncate, unlink and metadata. */
static bool bfs4_defrag_before_mutation(bfs4_volume_t *v,uint32_t inode) {
    if(inode && inode==v->swap_inode)return false;
    if (g_defrag.volume!=v || !g_defrag.inode || g_defrag.inode!=inode ||
        g_defrag.info.state!=BK_DEFRAG_RUNNING) return true;
    if (!bfs4_defrag_release_destination()) {
        bfs4_defrag_fail(BK_DEFRAG_IO_ERROR,0);
        return false;
    }
    for (uint32_t i=0;i<g_defrag.original.extent_count;i++)
        bfs4_defrag_invalidate(v,g_defrag.extents[i].start,g_defrag.extents[i].count);
    g_defrag.info.processed_blocks+=g_defrag.blocks-g_defrag.position;
    g_defrag.info.skipped_files++;
    g_defrag.inode=0;
    g_defrag.info.current_inode=0;
    return true;
}
static bool bfs4_defrag_reserve(uint32_t count,uint32_t *first_out) {
    bfs4_volume_t *v=g_defrag.volume;
    uint32_t run=0;
    *first_out=0;
    if (count>v->super.free_blocks) return true;
    for (uint32_t b=v->super.data_start;b<v->super.total_blocks;b++) {
        run=bfs4_bit(v->block_bitmap,b) ? 0 : run+1;
        if (run!=count) continue;
        uint32_t first=b+1-count;
        if (!bfs4_begin_change(v)) return false;
        for (uint32_t j=first;j<=b;j++) bfs4_set_bit(v->block_bitmap,j,true);
        for (uint32_t j=first/32768U;j<=b/32768U;j++)
            if (!bfs4_write_bitmap_byte(v,false,j*32768U)) {
                /* Restore every affected bitmap page, including any already
                 * persisted before the failing page. Old extents are intact. */
                for (uint32_t k=first;k<=b;k++) bfs4_set_bit(v->block_bitmap,k,false);
                bool restored=true;
                for (uint32_t k=first/32768U;k<=b/32768U;k++)
                    if (!bfs4_write_bitmap_byte(v,false,k*32768U)) restored=false;
                if (!restored) {
                    g_defrag.uncertain=true;
                    for (uint32_t k=first;k<=b;k++) bfs4_set_bit(v->block_bitmap,k,true);
                    v->super.free_blocks-=count;
                    (void)bfs4_write_super(v);
                }
                return false;
            }
        v->super.free_blocks-=count;
        g_defrag.destination=first;
        if (!bfs4_write_super(v)) return false;
        bfs4_defrag_invalidate(v,first,count);
        *first_out=first;
        return true;
    }
    return true; /* No contiguous free run: report an honest skipped file. */
}
static bool bfs4_defrag_commit(void) {
    bfs4_volume_t *v=g_defrag.volume;
    bfs4_inode_t *inode=&v->inodes[g_defrag.inode];
    *inode=g_defrag.original;
    kmemset(inode->extents,0,sizeof(inode->extents));
    inode->extents[0]=(bfs4_extent_t){g_defrag.destination,g_defrag.blocks};
    inode->extent_count=1;
    inode->indirect=0;
    inode->generation++;
    if (!bfs4_write_inode(v,g_defrag.inode)) {
        /* A failed sector write has uncertain persistence. Keep BOTH sets of
         * blocks reserved; do not free a destination the disk may reference. */
        *inode=g_defrag.original;
        g_defrag.uncertain=true;
        g_defrag.destination=0;
        bfs4_defrag_fail(BK_DEFRAG_IO_ERROR,g_defrag.info.current_block);
        return false;
    }
    g_defrag.destination=0; /* The live inode now owns the new extent. */
    for (uint32_t i=0;i<g_defrag.original.extent_count;i++) {
        bfs4_extent_t old=g_defrag.extents[i];
        bfs4_cache_invalidate(v,old.start,old.count);
        if (!bfs4_free_extent(v,old.start,old.count)) {
            g_defrag.uncertain=true;
            for (uint32_t j=0;j<old.count;j++) bfs4_set_bit(v->block_bitmap,old.start+j,true);
            bfs4_defrag_fail(BK_DEFRAG_IO_ERROR,old.start);
            return false;
        }
    }
    if (g_defrag.original.indirect) {
        uint32_t old=g_defrag.original.indirect;
        bfs4_cache_invalidate(v,old,1);
        if (!bfs4_free_extent(v,old,1)) {
            g_defrag.uncertain=true;
            bfs4_set_bit(v->block_bitmap,old,true);
            bfs4_defrag_fail(BK_DEFRAG_IO_ERROR,old); return false;
        }
        if (v->indirect_tables[g_defrag.inode]) kfree(v->indirect_tables[g_defrag.inode]);
        v->indirect_tables[g_defrag.inode]=NULL;
    }
    if (!bfs4_write_super(v)) {
        bfs4_defrag_fail(BK_DEFRAG_IO_ERROR,0); return false;
    }
    g_defrag.info.moved_files++;
    return true;
}
bool bfs4_defrag_cancel(const fat_fs_t *fs,uint32_t owner) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v!=g_defrag.volume || (owner && owner!=g_defrag.info.owner)) return false;
    if (g_defrag.info.state==BK_DEFRAG_RUNNING) {
        if (!bfs4_defrag_release_destination()) {
            bfs4_defrag_fail(BK_DEFRAG_IO_ERROR,0); return false;
        }
        g_defrag.inode=0;
        g_defrag.info.state=BK_DEFRAG_CANCELLED;
        g_defrag.info.current_inode=0;
    }
    bfs4_defrag_map();
    return true;
}
bool bfs4_defrag_begin(const fat_fs_t *fs,uint32_t owner,bool optimize) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || !owner || (optimize && v->device->read_only) ||
        g_defrag.info.state==BK_DEFRAG_RUNNING || g_defrag.uncertain) return false;
    if (g_defrag.verified) kfree(g_defrag.verified);
    kmemset(&g_defrag,0,sizeof(g_defrag));
    g_defrag.volume=v;
    g_defrag.info.owner=owner;
    g_defrag.info.optimize=optimize;
    fat_check_report_t check;
    if (!bfs4_check(fs,&check) || check.errors) {
        g_defrag.info.state=BK_DEFRAG_ERROR;
        g_defrag.info.error=BK_DEFRAG_CORRUPT_VOLUME;
        return false;
    }
    g_defrag.verified=kzalloc(v->super.block_bitmap_blocks*BFS4_BLOCK_SIZE);
    if (!g_defrag.verified) {
        g_defrag.info.state=BK_DEFRAG_ERROR;
        g_defrag.info.error=BK_DEFRAG_NO_MEMORY;
        return false;
    }
    (void)mm_set_allocation_owner(g_defrag.verified,0U);
    g_defrag.info.state=BK_DEFRAG_RUNNING;
    g_defrag.info.total_blocks=v->super.total_blocks;
    g_defrag.info.block_size=BFS4_BLOCK_SIZE;
    g_defrag.info.data_start=v->super.data_start;
    g_defrag.info.files=check.files;
    g_defrag.info.work_blocks=check.allocated_clusters;
    g_defrag.info.fragmented_before=check.fragmented_files;
    g_defrag.info.fragmented_after=check.fragmented_files;
    g_defrag.next_inode=1;
    bfs4_defrag_map();
    return true;
}
bool bfs4_defrag_status(const fat_fs_t *fs,bk_defrag_info_t *out) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || !out) return false;
    if (g_defrag.volume==v) { bfs4_defrag_map(); *out=g_defrag.info; }
    else {
        kmemset(out,0,sizeof(*out));
        out->total_blocks=v->super.total_blocks; out->free_blocks=v->super.free_blocks;
        out->data_start=v->super.data_start; out->block_size=BFS4_BLOCK_SIZE;
        for (uint32_t cell=0;cell<BK_DEFRAG_CELLS;cell++) {
            uint32_t a=(uint32_t)((uint64_t)cell*v->super.total_blocks/BK_DEFRAG_CELLS);
            uint32_t b=(uint32_t)((uint64_t)(cell+1)*v->super.total_blocks/BK_DEFRAG_CELLS);
            uint8_t state=b<=v->super.data_start ? BK_DEFRAG_RESERVED : BK_DEFRAG_FREE;
            for (uint32_t i=a;i<b;i++) if (i>=v->super.data_start && bfs4_bit(v->block_bitmap,i)) {
                state=BK_DEFRAG_PENDING; break;
            }
            out->cells[cell]=state;
        }
    }
    return true;
}
bool bfs4_defrag_step(const fat_fs_t *fs,uint32_t owner) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v!=g_defrag.volume || owner!=g_defrag.info.owner ||
        g_defrag.info.state!=BK_DEFRAG_RUNNING) return false;
    if (!g_defrag.inode) {
        for (uint32_t scanned=0;scanned<32 && g_defrag.next_inode<v->super.inode_count;scanned++) {
            uint32_t number=g_defrag.next_inode++;
            if(number==v->swap_inode){g_defrag.info.skipped_files++;continue;}
            if (!bfs4_bit(v->inode_bitmap,number)) continue;
            bfs4_inode_t *inode=&v->inodes[number];
            if (!inode->extent_count) continue;
            g_defrag.original=*inode; g_defrag.blocks=0; g_defrag.position=0;
            for (uint32_t i=0;i<inode->extent_count;i++) {
                bfs4_extent_t *extent=bfs4_extent(v,inode,i);
                if (!extent || !extent->count || extent->start<v->super.data_start ||
                    extent->start>=v->super.total_blocks || extent->count>v->super.total_blocks-extent->start) {
                    bfs4_defrag_fail(BK_DEFRAG_CORRUPT_VOLUME,0); return false;
                }
                g_defrag.extents[i]=*extent; g_defrag.blocks+=extent->count;
            }
            g_defrag.inode=number; g_defrag.info.current_inode=number;
            kstrncpy(g_defrag.info.current_file,inode->name,sizeof(g_defrag.info.current_file)-1);
            if (inode->indirect) {
                if (!bfs4_read_block(v,inode->indirect,g_defrag.source)) {
                    bfs4_defrag_fail(BK_DEFRAG_IO_ERROR,inode->indirect); return false;
                }
                bfs4_set_bit(g_defrag.verified,inode->indirect,true);
                g_defrag.info.processed_blocks++; g_defrag.info.verified_blocks++;
            }
            if (g_defrag.info.optimize && inode->extent_count>1) {
                uint32_t destination;
                if (!bfs4_defrag_reserve(g_defrag.blocks,&destination)) {
                    bfs4_defrag_fail(BK_DEFRAG_IO_ERROR,0); return false;
                }
                if (!destination) g_defrag.info.skipped_files++;
            }
            break;
        }
        if (!g_defrag.inode) {
            if (g_defrag.next_inode>=v->super.inode_count) {
                fat_check_report_t check;
                if (!bfs4_check(fs,&check) || check.errors) {
                    bfs4_defrag_fail(BK_DEFRAG_CORRUPT_VOLUME,0); return false;
                }
                g_defrag.info.fragmented_after=check.fragmented_files;
                g_defrag.info.current_inode=0;
                g_defrag.info.current_file[0]=0;
                g_defrag.info.state=BK_DEFRAG_COMPLETE;
            }
            bfs4_defrag_map(); return true;
        }
    }
    for (uint32_t done=0;done<32 && g_defrag.position<g_defrag.blocks;done++) {
        uint32_t block=bfs4_map(v,&g_defrag.original,g_defrag.position);
        g_defrag.info.current_block=block;
        if (!block || !bfs4_read_block(v,block,g_defrag.source)) {
            bfs4_defrag_fail(BK_DEFRAG_IO_ERROR,block); return false;
        }
        if (g_defrag.destination) {
            uint32_t target=g_defrag.destination+g_defrag.position;
            bfs4_cache_invalidate(v,target,1);
            if (!bfs4_write_block(v,target,g_defrag.source) ||
                !bfs4_read_block(v,target,g_defrag.target) ||
                kmemcmp(g_defrag.source,g_defrag.target,BFS4_BLOCK_SIZE)) {
                bfs4_defrag_fail(BK_DEFRAG_IO_ERROR,target); return false;
            }
            bfs4_set_bit(g_defrag.verified,target,true);
        }
        bfs4_set_bit(g_defrag.verified,block,true);
        g_defrag.info.processed_blocks++; g_defrag.info.verified_blocks++;
        g_defrag.position++;
    }
    if (g_defrag.position==g_defrag.blocks) {
        if (g_defrag.destination && !bfs4_defrag_commit()) return false;
        g_defrag.inode=0;
        g_defrag.info.current_inode=0;
    }
    bfs4_defrag_map();
    return true;
}
