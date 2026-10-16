#!/usr/bin/env python3
"""Real VFS read/write/cache/truncate paths over the real BFS4 block driver."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
vfs=(root/'kernel/drivers/core/vfs.c').read_text()
def function(name):
    import re
    match=re.search(r'^(?:static )?(?:bool|int|void|uint32_t|int32_t) [^\n]*\b'+name+r'\([^;]*?\)\s*\{',vfs,re.M)
    assert match,name
    start=match.start(); brace=vfs.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(vfs[end]=='{')-(vfs[end]=='}');end+=1
    return vfs[start:end]+'\n'
base=(root/'tests/storage/bfs4_host.c').read_text().split('int main(')[0]
base=base.replace('"../../kernel/', '"'+str(root/'kernel')+'/')
base=base.replace('static FILE *disk;', 'static FILE *disk;\nstatic bool fail_large_write;')
base=base.replace('    (void)device;\n    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&\n           fwrite',
                  '    (void)device;\n    if (fail_large_write && count > 8) { fail_large_write=false; return false; }\n    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&\n           fwrite')

source=base+f'\n#include "{root}/kernel/include/vfs.h"\n#include "{root}/kernel/include/iso9660.h"\n'
source+=vfs[vfs.index('typedef enum {'):vfs.index('/* Mantener la raiz')]
source+='''
#define W3P_SCOPE(a,b) ((void)0)
static vfs_file_t g_files[VFS_MAX_OPEN_FILES];
static uint32_t g_vfs_dirty_files,g_vfs_writeback_maps,g_vfs_open_files;
static uint32_t g_private_entries,g_private_age;
static const char *g_vfs_last_error;
static fat_fs_t test_fs;
void *krealloc(void *p,size_t n){return realloc(p,n);}
static bool path_component_equals_ci(const char *a,const char *b){return !strcasecmp(a,b);}
static bool vfs_select_fat(const char *full,fat_fs_t *fs,const char **inner){*fs=test_fs;*inner=full;return true;}
static vfs_shared_mapping_t *vfs_shared_for_file(vfs_file_t *f){(void)f;return NULL;}
static vfs_shared_mapping_t *vfs_private_find(vfs_file_t *f,bool idle){(void)f;(void)idle;return NULL;}
static void vfs_private_invalidate(const char *p){(void)p;}
static bool vfs_mapping_attach(vfs_shared_mapping_t *m,int fd,bool w){(void)m;(void)fd;(void)w;return false;}
bool vfs_shared_sync(vfs_shared_mapping_t *m){(void)m;return false;}
bool vfs_is_directory(int fd){return g_files[fd].entry.fat.entry.is_directory;}
static bool compat_mode_is_low_memory(void){return false;}
bool fat_resolve_path(const fat_fs_t *fs,const char *p,fat_dir_entry_t *e){return bfs4_resolve(fs,p,e);}
bool fat_read_file_at(const fat_fs_t *fs,const fat_dir_entry_t *e,uint32_t off,void *p,uint32_t n,uint32_t *got){return bfs4_read(fs,e,off,p,n,got);}
bool fat_write_path(fat_fs_t *fs,const char *p,const void *data,uint32_t n){return bfs4_write(fs,p,data,n);}
bool fat_append_path(fat_fs_t *fs,const char *p,const void *data,uint32_t old,uint32_t n){return bfs4_append(fs,p,data,old,n);}
const char *fat_last_error(void){return "test disk error";}
bool iso9660_read_at(const iso9660_entry_t *e,uint32_t off,void *data,uint32_t n,uint32_t *got){(void)e;(void)off;(void)data;(void)n;(void)got;return false;}
static bool vfs_sync_path(const char *,vfs_file_t *);
static bool vfs_stream_file(const vfs_file_t *,uint32_t);
'''
for name in ['vfs_access_mode','vfs_flags_readable','vfs_flags_writable','vfs_drop_cache',
             'vfs_drop_dir_cache','vfs_cache_reserve','vfs_cache_load','vfs_refresh_path_handles',
             'vfs_flush_file','vfs_sync_path','vfs_stream_file','vfs_stream_write',
             'vfs_write','vfs_read','vfs_size','vfs_seek','vfs_tell','vfs_truncate','vfs_flush','vfs_close']:
    source+=function(name)
source+=r'''
static void open_alias(int fd) {
    vfs_file_t *f=&g_files[fd];memset(f,0,sizeof(*f));
    f->used=true;f->references=1;f->flags=VFS_O_RDWR;f->source=VFS_FILE_FAT;
    strcpy(f->path,"/large.bin");f->entry.fat.fs=test_fs;
    require(bfs4_resolve(&test_fs,f->path,&f->entry.fat.entry),"open alias");g_vfs_open_files++;
}
static unsigned char pattern(uint32_t p){return (p*37U)^(p>>9)^(p>>17);}
int main(int argc,char **argv) {
    require(argc==2 && (disk=fopen(argv[1],"r+b")),"host image");
    fseek(disk,0,SEEK_END);
    block_device_t dev={0};dev.sector_count=ftell(disk)/512U;dev.sector_size=512;
    dev.read=host_read;dev.write=host_write;
    require(bfs4_format(&dev,"LARGEFILES") && bfs4_mount(&test_fs,&dev,0),"format/mount");
    uint64_t total,free_before,free_after;
    require(bfs4_space(&test_fs,&total,&free_before),"initial space");
    require(bfs4_write(&test_fs,"/large.bin",NULL,0),"create");
    open_alias(0);open_alias(1);
    unsigned char data[65536],out[65536];
    const uint32_t length=183997730U; /* Quake II pak0.pak size */
    for(uint32_t position=0;position<length;) {
        uint32_t n=length-position;if(n>sizeof(data))n=sizeof(data);
        for(uint32_t j=0;j<n;j++)data[j]=pattern(position+j);
        require(vfs_write(0,data,n)==(int)n,"write beyond old 64 MiB cap");
        require(g_files[0].cache_capacity<=VFS_STREAM_THRESHOLD,"bounded VFS cache");
        position+=n;
        if(position==65536 || position==64U*1024U*1024U) {
            require(vfs_seek(1,position-20,0)==(int)(position-20),"alias seek");
            require(vfs_read(1,out,20)==20,"alias read");
            for(unsigned j=0;j<20;j++)require(out[j]==pattern(position-20+j),"alias coherence");
        }
    }
    require(vfs_size(0)==(int)length && vfs_size(1)==(int)length,"visible size");
    require(vfs_flush(0) && vfs_close(0) && vfs_close(1),"close large writer");
    open_alias(0); /* read/write reopen must not eagerly load the entire file */
    for(uint32_t position=0;position<length;) {
        uint32_t n=length-position;if(n>sizeof(out))n=sizeof(out);
        require(vfs_read(0,out,n)==(int)n,"read whole large file after reopen");
        for(uint32_t j=0;j<n;j++)require(out[j]==pattern(position+j),"large file byte integrity");
        position+=n;
    }
    puts("PASS: 183997730 bytes streamed, reopened read/write and verified; VFS cache <=1 MiB");
    uint64_t before_failure,after_failure;
    require(bfs4_space(&test_fs,&total,&before_failure),"space before I/O failure");
    fail_large_write=true;
    require(vfs_write(0,data,sizeof(data))<0 && !fail_large_write,"injected block write failure");
    require(vfs_size(0)==(int)length && vfs_tell(0)==(int)length,"I/O failure preserves EOF/offset");
    require(bfs4_space(&test_fs,&total,&after_failure) && before_failure==after_failure,
            "I/O failure releases new reservations");
    fat_check_report_t failed_report;
    require(bfs4_check(&test_fs,&failed_report) && !failed_report.errors,"I/O failure preserves consistency");
    uint32_t patch=64U*1024U*1024U-17;
    memset(data,0x5a,91);require(vfs_seek(0,patch,0)==(int)patch,"random seek");
    require(vfs_write(0,data,91)==91,"overwrite across block boundary");
    require(vfs_seek(0,patch-16,0)==(int)patch-16 && vfs_read(0,out,123)==123,"read patch boundary");
    for(unsigned j=0;j<123;j++)require(out[j]==(j>=16 && j<107?0x5a:pattern(patch-16+j)),"preserve neighbours");
    require(vfs_seek(0,length+4099,0)==(int)(length+4099) && vfs_write(0,"XYZ",3)==3,"write after hole");
    require(vfs_seek(0,length,0)==(int)length && vfs_read(0,out,4102)==4102,"read zero gap");
    for(unsigned j=0;j<4099;j++)require(!out[j],"extension gap zeroed");
    require(!memcmp(out+4099,"XYZ",3),"extension data");
    int size=vfs_size(0);
    require(!vfs_truncate(0,0x7fffffffU) && vfs_size(0)==size,"ENOSPC preserves EOF");
    require(!vfs_truncate(0,0x80000000U),"reject unsupported signed size");
    require(vfs_seek(0,0x7fffffff-10,0)>=0 && vfs_write(0,data,32)==-27,"overflow returns EFBIG");
    require(vfs_truncate(0,3),"shrink large file");
    require(vfs_truncate(0,2U*1024U*1024U+7),"extend truncated file");
    require(vfs_seek(0,0,0)==0 && vfs_read(0,out,65536)==65536,"read resized file");
    for(unsigned j=0;j<65536;j++)require(out[j]==(j<3?pattern(j):0),"truncate/grow zeros old data");
    require(vfs_truncate(0,0) && vfs_flush(0) && vfs_close(0),"truncate empty");
    require(bfs4_remove(&test_fs,"/large.bin"),"remove large file");
    require(bfs4_space(&test_fs,&total,&free_after) && free_after==free_before,"space restored");
    fat_check_report_t report;
    require(bfs4_check(&test_fs,&report) && report.completed && !report.errors,"BFS4 consistent");
    require(!g_vfs_dirty_files && bfs4_sync(&test_fs),"clean shutdown");fclose(disk);
    puts("PASS: random writes, alias reads, zero gaps, truncate/grow, ENOSPC, failed I/O rollback, overflow and free-space accounting");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bk-large-files-') as directory:
    work=Path(directory);(work/'test.c').write_text(source)
    subprocess.run(['gcc','-m32','-O2','-Wall','-Wextra','-Wno-unused-function',str(work/'test.c'),
                    str(root/'kernel/drivers/storage/bfs4.c'),'-o',str(work/'test')],check=True)
    with (work/'disk.img').open('wb') as f:f.truncate(384*1024*1024)
    subprocess.run([str(work/'test'),str(work/'disk.img')],check=True)
