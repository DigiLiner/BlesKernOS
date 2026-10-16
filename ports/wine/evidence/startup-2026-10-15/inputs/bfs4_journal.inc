/* Optional metadata undo log in the BIOS reserved region. Legacy BFS4 images
 * have zeroes here. Stage2/kernel reserve ends at sector 2569 (source media
 * reserve at 3593); the log occupies 3840..4081, below the BFS superblock.
 * ATA/AHCI callbacks complete only after cache flush. Each old sector and
 * descriptor becomes durable before publishing its count, then its overwrite.
 * Two CRC headers retain the previous valid checkpoint after a torn header.
 * No allocation, disk format version change, or file-data journaling. */
#define BFS4_JOURNAL_LBA 3840U
#define BFS4_JOURNAL_MAX 120U
#define BFS4_JOURNAL_MAGIC 0x314a3442U
#define BFS4_JOURNAL_RECORD 0x524a3442U
#define BFS4_JOURNAL_BATCH 4U
/* The global recursive BFS4 mutex protects this resident scratch space. Four
 * adjacent records need one durable write, then one durable count checkpoint.
 * The on-disk format and the undo-before-overwrite ordering are unchanged. */
static uint8_t bfs4_journal_batch[BFS4_JOURNAL_BATCH*1024U];

typedef struct __attribute__((packed)) {
    uint32_t magic, sequence, epoch, active, count, start, sectors;
    uint8_t padding[480];
    uint32_t crc;
} bfs4_journal_header_t;
typedef struct __attribute__((packed)) {
    uint32_t magic, epoch, target, data_crc;
    uint8_t padding[492];
    uint32_t crc;
} bfs4_journal_record_t;
typedef char bfs4_journal_header_size[(sizeof(bfs4_journal_header_t)==512)?1:-1];
typedef char bfs4_journal_record_size[(sizeof(bfs4_journal_record_t)==512)?1:-1];

static bool bfs4_journal_valid(bfs4_journal_header_t *h,block_device_t *d,uint32_t p) {
    return h->magic==BFS4_JOURNAL_MAGIC && h->active<=1U &&
        h->count<=BFS4_JOURNAL_MAX && (h->active || !h->count) &&
        h->start==p+BFS4_BOOT_SECTORS && h->sectors==d->sector_count &&
        h->crc==bfs4_crc(h,508U);
}
static bool bfs4_journal_header(block_device_t *d,uint32_t p,bfs4_journal_header_t *h) {
    bfs4_journal_header_t a,b;
    if(p>d->sector_count || BFS4_BOOT_SECTORS>d->sector_count-p ||
       !block_read(d,p+BFS4_JOURNAL_LBA,1,&a) ||
       !block_read(d,p+BFS4_JOURNAL_LBA+1U,1,&b))return false;
    bool va=bfs4_journal_valid(&a,d,p),vb=bfs4_journal_valid(&b,d,p);
    if(va || vb) {
        *h=(!va || (vb && (int32_t)(b.sequence-a.sequence)>0))?b:a;
        return true;
    }
    /* Unknown contents are never reclassified as free journal storage. */
    uint8_t raw[512];
    for(uint32_t i=0;i<2U+2U*BFS4_JOURNAL_MAX;i++) {
        if(!block_read(d,p+BFS4_JOURNAL_LBA+i,1,raw))return false;
        for(uint32_t j=0;j<512U;j++)if(raw[j])return false;
    }
    kmemset(h,0,sizeof(*h));h->magic=BFS4_JOURNAL_MAGIC;
    h->start=p+BFS4_BOOT_SECTORS;h->sectors=d->sector_count;
    return true;
}
static bool bfs4_journal_publish(block_device_t *d,uint32_t p,bfs4_journal_header_t *h) {
    ++h->sequence;h->crc=bfs4_crc(h,508U);
    return block_write(d,p+BFS4_JOURNAL_LBA+(h->sequence&1U),1,h);
}
static bool bfs4_journal_record_read(block_device_t *d,uint32_t p,
        bfs4_journal_header_t *h,uint32_t i,bfs4_journal_record_t *r,uint8_t *raw) {
    uint32_t lba=p+BFS4_JOURNAL_LBA+2U+i*2U;
    return block_read(d,lba,1,r) && block_read(d,lba+1U,1,raw) &&
        r->magic==BFS4_JOURNAL_RECORD && r->epoch==h->epoch &&
        r->target>=h->start && r->target<d->sector_count &&
        r->crc==bfs4_crc(r,508U) && r->data_crc==bfs4_crc(raw,512U);
}
static bool bfs4_journal_recover(block_device_t *d,uint32_t p) {
    bfs4_journal_header_t h;bfs4_journal_record_t r;uint8_t raw[512];
    if(!bfs4_journal_header(d,p,&h))return false;
    if(!h.active)return true;
    if(d->read_only || !d->write)return false;
    /* Validate the entire undo set before altering any sector. A damaged log
     * requires explicit inspection; no guessed repair or lost-block deletion. */
    for(uint32_t i=0;i<h.count;i++)
        if(!bfs4_journal_record_read(d,p,&h,i,&r,raw))return false;
    for(uint32_t i=h.count;i;i--) {
        if(!bfs4_journal_record_read(d,p,&h,i-1U,&r,raw) ||
           !block_write(d,r.target,1,raw))return false;
    }
    h.active=0;h.count=0;
    if(!bfs4_journal_publish(d,p,&h))return false;
    kprintf("[BFS4:JOURNAL] recuperada transaccion epoch=%u\n",h.epoch);
    return true;
}
/* Clear the fixed on-disk journal with 4 KiB durable transactions.
 * The previous code issued one CACHE FLUSH for each of 242 sectors. Keep
 * full clearing: mounting an invalid header explicitly checks every byte. */
static uint8_t bfs4_journal_format_zero[BFS4_BLOCK_SIZE];
static bool bfs4_journal_format(block_device_t *d) {
    const uint32_t total = 2U + 2U * BFS4_JOURNAL_MAX;
    kmemset(bfs4_journal_format_zero, 0, sizeof(bfs4_journal_format_zero));
    for (uint32_t i = 0; i < total;) {
        uint32_t run = total - i;
        if (run > BFS4_BLOCK_SIZE / 512U) run = BFS4_BLOCK_SIZE / 512U;
        if (!block_write(d, BFS4_JOURNAL_LBA + i, (uint8_t)run,
                         bfs4_journal_format_zero)) {
            if (run == 1U) return false;
            for (uint32_t j = 0; j < run; ++j)
                if (!block_write(d, BFS4_JOURNAL_LBA + i + j, 1U,
                                 bfs4_journal_format_zero)) return false;
        }
        i += run;
    }
    return true;
}
static bool bfs4_journal_load(bfs4_volume_t *v) {
    bfs4_journal_header_t h;
    if(!bfs4_journal_header(v->device,v->partition_lba,&h) || h.active)return false;
    v->journal_ready=true;v->journal_sequence=h.sequence;v->journal_epoch=h.epoch;
    return true;
}
static bool bfs4_journal_save(bfs4_volume_t *v,uint32_t lba,uint32_t count) {
    if(!v->tx_depth)return true; /* Recovery and legacy defrag manage their own checkpoint. */
    if(v->write_failed || !v->journal_ready)return false;
    bfs4_journal_header_t h;
    kmemset(&h,0,sizeof(h));h.magic=BFS4_JOURNAL_MAGIC;
    h.start=v->start_lba;h.sectors=v->device->sector_count;
    h.sequence=v->journal_sequence;h.epoch=v->journal_epoch;
    h.active=1;h.count=v->journal_count;
    if(!v->journal_active) {
        ++h.epoch;
        if(!bfs4_journal_publish(v->device,v->partition_lba,&h))goto fail;
        v->journal_active=true;v->journal_epoch=h.epoch;v->journal_sequence=h.sequence;
    }
    for(uint32_t i=0;i<count;) {
        uint32_t targets[BFS4_JOURNAL_BATCH],batch=0;
        while(i<count && batch<BFS4_JOURNAL_BATCH) {
            bool saved=false;
            for(uint32_t j=0;j<v->journal_count;j++)
                if(v->journal_targets[j]==lba+i){saved=true;break;}
            if(saved){++i;continue;}
            if(v->journal_count+batch==BFS4_JOURNAL_MAX)goto fail;
            uint8_t *raw=bfs4_journal_batch+batch*1024U;
            bfs4_journal_record_t r;
            if(!block_read(v->device,lba+i,1,raw+512U))goto fail;
            kmemset(&r,0,sizeof(r));r.magic=BFS4_JOURNAL_RECORD;
            r.epoch=v->journal_epoch;r.target=lba+i;r.data_crc=bfs4_crc(raw+512U,512U);
            r.crc=bfs4_crc(&r,508U);
            kmemcpy(raw,&r,512U);targets[batch++]=lba+i++;
        }
        if(!batch)continue;
        uint32_t target=v->partition_lba+BFS4_JOURNAL_LBA+2U+v->journal_count*2U;
        if(!block_write(v->device,target,(uint8_t)(batch*2U),bfs4_journal_batch))goto fail;
        h.count=v->journal_count+batch;
        if(!bfs4_journal_publish(v->device,v->partition_lba,&h))goto fail;
        for(uint32_t j=0;j<batch;j++)v->journal_targets[v->journal_count++]=targets[j];
        v->journal_sequence=h.sequence;
    }
    return true;
fail:
    v->write_failed=true;return false;
}
static bool bfs4_journal_reload(bfs4_volume_t *v);
static bool bfs4_journal_finish(bfs4_volume_t *v,bool success) {
    if(!v->journal_active)return !v->write_failed;
    if(!success || v->write_failed) {
        /* RAM metadata may already have changed. Quarantine this mount; a
         * fresh mount replays the validated undo log before exposing files. */
        if(bfs4_journal_recover(v->device,v->partition_lba) &&
           bfs4_journal_reload(v) && bfs4_journal_load(v)) {
            v->journal_active=false;v->journal_count=0;v->write_failed=false;
            return true; /* caller still returns the original operation error */
        }
        v->write_failed=true;v->super.state=BFS4_DIRTY;return false;
    }
    bfs4_journal_header_t h;kmemset(&h,0,sizeof(h));
    h.magic=BFS4_JOURNAL_MAGIC;h.sequence=v->journal_sequence;
    h.epoch=v->journal_epoch;h.start=v->start_lba;h.sectors=v->device->sector_count;
    if(!bfs4_journal_publish(v->device,v->partition_lba,&h)) {
        v->write_failed=true;return false;
    }
    v->journal_sequence=h.sequence;v->journal_active=false;v->journal_count=0;
    return true;
}
