/* Public maintenance calls execute under the VFS domain. */
bool bk_file_get_mode(const char *path,uint16_t *mode) {return vfs_get_mode(path,mode);}
bool bk_file_set_mode(const char *path,uint16_t mode) {return vfs_set_mode(path,mode);}
bool bk_file_space_path(const char *path,uint64_t *total,uint64_t *free_bytes) {
    return vfs_get_space_path(path,total,free_bytes);
}
bool bk_file_sync(void) {return vfs_sync_all();}
bool bk_device_attach_volume(const char *name) {return vfs_attach_volume(name);}
bool bk_device_unmount_volume(const char *name) {return vfs_unmount_volume(name);}
bool bk_file_touch(const char *path,bool create) {
    vfs_dir_entry_t entry;
    bk_datetime_t now;
    if(!path)return false;
    if(!vfs_stat(path,&entry)) {
        if(!create)return true;
        int fd=vfs_open(path,VFS_O_WRONLY|VFS_O_CREATE);
        if(fd<0)return false;
        if(!vfs_close(fd))return false;
    }
    if(!bk_time_datetime(&now) || now.date.year<1970 || now.date.year>9999 ||
       now.date.month<1 || now.date.month>12 || now.date.day<1 || now.date.day>31)return false;
    static const uint8_t months[]={31,28,31,30,31,30,31,31,30,31,30,31};
    uint64_t days=0;
    for(uint32_t y=1970;y<now.date.year;y++)days+=365+(y%4==0 && (y%100!=0 || y%400==0));
    for(uint32_t m=1;m<now.date.month;m++)days+=months[m-1]+(m==2 && now.date.year%4==0 && (now.date.year%100!=0 || now.date.year%400==0));
    uint64_t seconds=(days+now.date.day-1)*86400U+now.time.hour*3600U+now.time.minute*60U+now.time.second;
    return vfs_set_times(path,true,seconds,true,seconds);
}

static void command_put32(uint8_t *p,uint32_t n) {
    for(uint32_t i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));
}
int32_t bk_device_partition_edit(const char *name,uint32_t index,
    uint32_t first,uint32_t sectors,uint32_t type) {
    block_device_t *d=block_get(name);
    uint8_t mbr[512],original[512];
    fat_fs_t fs;
    if(!d || d->base_lba || d->sector_size!=512 || index<1 || index>4 ||
       (type && type!=0x0c && type!=0x0e && type!=0x83))return -1;
    if(d->read_only || !d->write)return -2;
    /* Refuse the root's entire physical backing, including partition views. */
    if(vfs_get_fs_info(&fs) && fs.device && fs.device->read==d->read &&
       fs.device->driver_data==d->driver_data)return -2;
    if(vfs_volume_busy(name))return -2;
    for(uint32_t i=0;i<block_count();i++) {
        block_device_t *v=block_at(i);
        if(v && v->read==d->read && v->driver_data==d->driver_data &&
           vfs_volume_busy(v->name))return -2;
    }
    if(type && (first<2048 || !sectors || first>=d->sector_count || sectors>d->sector_count-first))return -1;
    if(!block_read(d,0,1,mbr))return -4;
    kmemcpy(original,mbr,512);
    /* BFS4 superfloppies keep their superblock behind the boot region. */
    if(d->sector_count>BFS4_BOOT_SECTORS) {
        uint8_t super[512];
        if(!block_read(d,BFS4_BOOT_SECTORS,1,super))return -4;
        if(public_read_le32(super)==BFS4_MAGIC)return -3;
    }
    bool blank=true;
    for(uint32_t i=0;i<512;i++)if(mbr[i])blank=false;
    if(!blank && (mbr[510]!=0x55 || mbr[511]!=0xaa))return -3;
    /* A FAT superfloppy is a volume, not an empty MBR. */
    if(!blank && ((mbr[11]==0 && mbr[12]==2) ||
       kstrncmp((const char *)mbr+3,"BLES",4)==0))return -3;
    for(uint32_t i=0;i<4;i++) {
        uint8_t *e=mbr+446+i*16;
        uint32_t a=public_read_le32(e+8),n=public_read_le32(e+12);
        if(e[4]==0xee || e[4]==5 || e[4]==15 || e[4]==0x85)return -3;
        if(!e[4]) {for(uint32_t j=0;j<16;j++)if(e[j])return -3;
        continue;}
        if((e[0]!=0 && e[0]!=0x80) || !a || !n || a>=d->sector_count || n>d->sector_count-a)return -3;
        if(type && first<a+n && a<first+sectors)return -1;
    }
    uint8_t *entry=mbr+446+(index-1)*16;
    if(type) {
        if(entry[4] || first<2048 || !sectors || first>=d->sector_count ||
           sectors>d->sector_count-first)return -1;
        if(block_count()>=BLOCK_MAX_DEVICES)return -5;
    } else if(!entry[4] || first || sectors)return -1;
    kmemset(entry,0,16);
    if(type) {
        entry[1]=entry[5]=0xfe;
        entry[2]=entry[3]=entry[6]=entry[7]=0xff;
        entry[4]=(uint8_t)type;
        command_put32(entry+8,first);
        command_put32(entry+12,sectors);
    }
    mbr[510]=0x55;
    mbr[511]=0xaa;
    if(!vfs_sync_all() || !block_write(d,0,1,mbr))return -4;
    uint8_t verify[512];
    if(!block_read(d,0,1,verify) || kmemcmp(verify,mbr,512)) {
        (void)block_write(d,0,1,original);
        return -4;
    }
    char view[8];
    uint32_t len=kstrlen(name);
    if(len+2>=sizeof(view))return 1;
    kstrncpy(view,name,sizeof(view)-1);
    view[len]='p';
    view[len+1]=(char)('0'+index);
    view[len+2]=0;
    if(!type) {if(block_get(view))block_unregister(view);
    return 0;}
    return block_register_view(view,d,first,sectors)?0:1;
}
