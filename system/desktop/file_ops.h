#ifndef BK_DESKTOP_FILE_OPS_H
#define BK_DESKTOP_FILE_OPS_H
#include "kernel/include/api.h"
#include <stdio.h>

#define BK_DESKTOP_FILES "/SYSTEM/DESKTOP"
#define BK_TRASH_FILES "/SYSTEM/TRASH"

/* Optional per-operation observer. No global callback: concurrent transfers
 * retain separate counters, destinations and lifetimes. */
typedef void (*bk_shell_progress_fn)(void *context,const char *path,
                                      uint32_t bytes,bool completed);
typedef struct {bk_shell_progress_fn notify;void *context;} bk_shell_progress_t;
static inline void bk_shell_report(bk_shell_progress_t *progress,const char *path,
                                   uint32_t bytes,bool completed) {
    if(progress && progress->notify)progress->notify(progress->context,path,bytes,completed);
}

static inline bool bk_shell_inside(const char *path, const char *parent) {
    unsigned n = bk_runtime_strlen(parent);
    return !bk_runtime_strncmp(path, parent, n) && (!path[n] || path[n] == '/');
}

static inline const char *bk_shell_basename(const char *path) {
    const char *name = path;
    for (; *path; ++path) if (*path == '/') name = path + 1;
    return name;
}

static inline bool bk_shell_read_directory(const char *path, vfs_dir_entry_t **entries,
                                           uint32_t *count) {
    for (uint32_t capacity=64;capacity<=8192;capacity*=2) {
        vfs_dir_entry_t *list=bk_sys_alloc_zero(capacity*sizeof(*list));
        if(!list)return false;
        uint32_t found=0;
        bool ok=bk_file_list_dir(path,list,capacity,&found);
        if(ok && found<capacity) {*entries=list;*count=found;return true;}
        bk_sys_free(list);
        if(found<capacity)return false;
    }
    return false;
}

static inline bool bk_shell_remove_tree_progress(const char *path, unsigned depth,bk_shell_progress_t *progress) {
    vfs_dir_entry_t info;
    if(depth>32 || !bk_file_stat(path,&info))return false;
    bk_shell_report(progress,path,0,false);
    if(info.type==VFS_NODE_DIR) {
        vfs_dir_entry_t *entries;uint32_t count;
        if(!bk_shell_read_directory(path,&entries,&count))return false;
        for(uint32_t i=0;i<count;++i) {
            if(!bk_runtime_strcmp(entries[i].name,".") || !bk_runtime_strcmp(entries[i].name,".."))continue;
            char child[VFS_MAX_PATH];
            if(snprintf(child,sizeof(child),"%s/%s",path,entries[i].name)>=(int)sizeof(child) ||
                !bk_shell_remove_tree_progress(child,depth+1,progress)) {bk_sys_free(entries);return false;}
        }
        bk_sys_free(entries);
    }
    bool ok=bk_file_remove(path);
    if(ok)bk_shell_report(progress,path,0,true);
    return ok;
}
static inline bool bk_shell_remove_tree(const char *path,unsigned depth) {
    return bk_shell_remove_tree_progress(path,depth,NULL);
}

static inline bool bk_shell_copy_tree_progress(const char *source,const char *target,unsigned depth,bk_shell_progress_t *progress) {
    vfs_dir_entry_t info;
    if(depth>16 || !bk_file_stat(source,&info))return false;
    bk_shell_report(progress,source,0,false);
    if(info.type==VFS_NODE_DIR) {
        if(!bk_file_mkdir(target))return false;
        vfs_dir_entry_t *entries;uint32_t count;
        if(!bk_shell_read_directory(source,&entries,&count))return false;
        for(uint32_t i=0;i<count;++i) {
            if(!bk_runtime_strcmp(entries[i].name,".") || !bk_runtime_strcmp(entries[i].name,".."))continue;
            char input[VFS_MAX_PATH],output[VFS_MAX_PATH];
            if(snprintf(input,sizeof(input),"%s/%s",source,entries[i].name)>=(int)sizeof(input) ||
                snprintf(output,sizeof(output),"%s/%s",target,entries[i].name)>=(int)sizeof(output) ||
                !bk_shell_copy_tree_progress(input,output,depth+1,progress)) {bk_sys_free(entries);return false;}
        }
        bk_sys_free(entries);bk_shell_report(progress,source,0,true);return true;
    }
    int input=bk_file_open(source,VFS_O_RDONLY);
    if(input<0)return false;
    int output=bk_file_open(target,VFS_O_WRONLY|VFS_O_CREATE|VFS_O_TRUNC);
    if(output<0){bk_file_close(input);return false;}
    uint8_t *buffer=bk_sys_alloc_zero(16384);bool ok=buffer!=NULL;
    while(ok) {
        int bytes=bk_file_read(input,buffer,16384);
        if(bytes<0){ok=false;break;}
        if(!bytes)break;
        uint32_t written=0;
        while(written<(uint32_t)bytes) {
            int n=bk_file_write(output,buffer+written,bytes-written);
            if(n<=0){ok=false;break;} written+=n;
            bk_shell_report(progress,source,n,false);
        }
    }
    bk_sys_free(buffer);bk_file_close(input);
    if(!bk_file_close(output))ok=false;
    if(ok)bk_shell_report(progress,source,0,true);
    return ok;
}

static inline bool bk_shell_copy_tree(const char *source,const char *target,unsigned depth) {
    return bk_shell_copy_tree_progress(source,target,depth,NULL);
}

static inline bool bk_shell_move_progress(const char *source, const char *directory,bk_shell_progress_t *progress) {
    char target[VFS_MAX_PATH];
    vfs_dir_entry_t info;
    if (!source || !source[0] || !directory || !directory[0] ||
        !bk_file_stat(source, &info) || bk_shell_inside(directory, source)) return false;
    if (!bk_file_stat(directory, &info) || info.type != VFS_NODE_DIR) return false;
    if (snprintf(target, sizeof(target), "%s%s%s", directory,
        directory[bk_runtime_strlen(directory)-1] == '/' ? "" : "/",
        bk_shell_basename(source)) >= (int)sizeof(target)) return false;
    if(!bk_runtime_strcmp(source,target)){bk_shell_report(progress,source,0,true);return true;}
    if (bk_file_stat(target, &info)) return false; /* Never replace another file. */
    int relation=bk_file_transfer_check(source,directory);
    if(relation<0)return false;
    if(bk_file_rename(source,target)){bk_shell_report(progress,source,0,true);return true;}
    if(relation>0)return false;
    /* Across volumes (including a read-only CD), copy into a private staging
     * directory, publish only when complete, and keep the source intact. */
    char staging[VFS_MAX_PATH],payload[VFS_MAX_PATH];bool reserved=false;
    for(unsigned n=0;n<1000;++n) {
        if(snprintf(staging,sizeof(staging),"%s/.bk-transfer-%u-%u",directory,
            bk_sys_getpid(),n)>=(int)sizeof(staging))return false;
        if(bk_file_mkdir(staging)){reserved=true;break;}
        if(!bk_file_stat(staging,&info))return false;
    }
    if(!reserved)return false;
    bool ok=snprintf(payload,sizeof(payload),"%s/item",staging)<(int)sizeof(payload) &&
        bk_shell_copy_tree_progress(source,payload,0,progress);
    if(ok)ok=!bk_file_stat(target,&info) && bk_file_rename(payload,target);
    (void)bk_shell_remove_tree(staging,0);
    return ok;
}

static inline bool bk_shell_move(const char *source,const char *directory) {
    return bk_shell_move_progress(source,directory,NULL);
}

static inline bool bk_shell_trash(const char *source) {
    char target[VFS_MAX_PATH], metadata[VFS_MAX_PATH], reservation[VFS_MAX_PATH];
    vfs_dir_entry_t info;
    if (!source || !source[0] || !bk_file_stat(source, &info) ||
        bk_shell_inside(source, BK_TRASH_FILES) ||
        !bk_runtime_strcmp(source,"/") || !bk_runtime_strcmp(source,"/SYSTEM") ||
        !bk_runtime_strcmp(source,"/WINEGATE") || !bk_runtime_strcmp(source,"/OPT")) return false;
    if (!bk_file_stat(BK_TRASH_FILES,&info) && !bk_file_mkdir(BK_TRASH_FILES)) return false;
    for (unsigned n = 0; n < 1000; ++n) {
        if (snprintf(target,sizeof(target), "%s/%s%s%u", BK_TRASH_FILES,
            bk_shell_basename(source), n ? "." : ".bk", n) >= (int)sizeof(target)) return false;
        /* Use the original name for the first item, a suffix for duplicates. */
        if (!n && snprintf(target,sizeof(target),"%s/%s",BK_TRASH_FILES,
            bk_shell_basename(source)) >= (int)sizeof(target)) return false;
        if (snprintf(metadata,sizeof(metadata),"%s/.bk-trash-%s",BK_TRASH_FILES,
            bk_shell_basename(target)) >= (int)sizeof(metadata) ||
            snprintf(reservation,sizeof(reservation),"%s/.bk-hold-%s",BK_TRASH_FILES,
            bk_shell_basename(target)) >= (int)sizeof(reservation)) return false;
        if (!bk_file_mkdir(reservation)) {
            if(!bk_file_stat(reservation,&info))return false;
            continue; /* Existing atomic name reservation. */
        }
        if (bk_file_stat(target,&info) || bk_file_stat(metadata,&info)) {
            bk_file_remove(reservation); continue;
        }
        bool saved = bk_file_write_all(metadata,source,bk_runtime_strlen(source)+1);
        bool moved = saved && bk_file_rename(source,target);
        if (!moved && saved) bk_file_remove(metadata);
        bk_file_remove(reservation);
        return moved;
    }
    return false;
}

static inline bool bk_shell_restore(const char *source) {
    char metadata[VFS_MAX_PATH], original[VFS_MAX_PATH];
    vfs_dir_entry_t info;
    if (!bk_shell_inside(source,BK_TRASH_FILES) ||
        snprintf(metadata,sizeof(metadata),"%s/.bk-trash-%s",BK_TRASH_FILES,
            bk_shell_basename(source)) >= (int)sizeof(metadata)) return false;
    int fd = bk_file_open(metadata,VFS_O_RDONLY);
    if (fd < 0) return false;
    int count = bk_file_read(fd,original,sizeof(original)); bk_file_close(fd);
    if (count <= 1 || count > (int)sizeof(original) || original[count-1] || original[0] != '/' ||
        bk_shell_inside(original,BK_TRASH_FILES) || bk_file_stat(original,&info)) return false;
    if (!bk_file_rename(source,original)) return false;
    bk_file_remove(metadata);
    return true;
}

static inline void bk_shell_remove_trash_metadata(const char *source) {
    char metadata[VFS_MAX_PATH];
    if (!bk_shell_inside(source,BK_TRASH_FILES) ||
        bk_runtime_strlen(source)<=bk_runtime_strlen(BK_TRASH_FILES)+1 ||
        bk_runtime_strcmp(source+bk_runtime_strlen(BK_TRASH_FILES)+1,bk_shell_basename(source))) return;
    if (snprintf(metadata,sizeof(metadata),"%s/.bk-trash-%s",BK_TRASH_FILES,
        bk_shell_basename(source)) < (int)sizeof(metadata)) bk_file_remove(metadata);
}
#endif
