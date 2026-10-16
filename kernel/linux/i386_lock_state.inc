/* Locks POSIX por proceso e identidad VFS; protegido por KDOMAIN_VFS.
 * ABI de referencia: Linux man-pages fcntl_locking(2), sin codigo copiado.
 */
typedef struct {
 uint64_t device,inode;
 int64_t first,last;
 uint32_t owner;
 uint16_t type;
} linux_file_lock;
static linux_file_lock *linux_file_locks;
#define LINUX_LOCK_LIMIT 128
static void linux_unlock_file(uint32_t owner,int fd) {
 uint64_t dev=0,ino=0;
 if(!linux_file_locks)return;
 if(fd>=0 && (fd>=LINUX_IO_BASE || !vfs_fd_identity(fd,&dev,&ino)))return;
 for(unsigned i=0;i<LINUX_LOCK_LIMIT;i++) {
  linux_file_lock *l=&linux_file_locks[i];
  if(l->owner==owner && (fd<0 || (l->device==dev && l->inode==ino)))l->owner=0;
 }
}
static int32_t linux_lock_apply(linux_file_lock *want,bool query,uint32_t *blocked) {
 unsigned free_count=0,needed=want->type==2?0:1;
 *blocked=0;
 if(!linux_file_locks) {
  if(query || want->type==2){want->type=2;return 0;}
  linux_file_locks=mm_alloc_guarded_stack(sizeof(*linux_file_locks)*LINUX_LOCK_LIMIT,false,NULL,NULL);
  if(!linux_file_locks)return -37;
  kmemset(linux_file_locks,0,sizeof(*linux_file_locks)*LINUX_LOCK_LIMIT);
 }
 for(unsigned i=0;i<LINUX_LOCK_LIMIT;i++) {
  linux_file_lock *l=&linux_file_locks[i];
  if(!l->owner){free_count++;continue;}
  if(l->device!=want->device || l->inode!=want->inode || l->last<want->first || l->first>want->last)continue;
  if(l->owner!=want->owner) {
   if(want->type!=2 && (l->type==1 || want->type==1)) {
    *blocked=l->owner;if(query){*want=*l;return 0;}return -11;
   }
  } else {
   free_count++;
   if(l->first<want->first)needed++;
   if(l->last>want->last)needed++;
  }
 }
 if(query){want->type=2;return 0;}
 if(free_count<needed)return -37;
 /* Recortar las regiones propias solapadas; a lo sumo una se divide en dos. */
 for(unsigned i=0;i<LINUX_LOCK_LIMIT;i++) {
  linux_file_lock *l=&linux_file_locks[i];
  if(l->owner!=want->owner || l->device!=want->device || l->inode!=want->inode || l->last<want->first || l->first>want->last)continue;
  if(l->first<want->first && l->last>want->last) {
   unsigned j=0;while(linux_file_locks[j].owner)j++;
   linux_file_locks[j]=*l;linux_file_locks[j].first=want->last+1;
   l->last=want->first-1;
  } else if(l->first<want->first)l->last=want->first-1;
  else if(l->last>want->last)l->first=want->last+1;
  else l->owner=0;
 }
 if(want->type!=2) {
  /* Unir regiones adyacentes del mismo tipo evita consumir slots por escrituras repetidas. */
  for(unsigned i=0;i<LINUX_LOCK_LIMIT;i++) {
   linux_file_lock *l=&linux_file_locks[i];
   if(l->owner==want->owner && l->device==want->device && l->inode==want->inode && l->type==want->type &&
      (l->last==want->first-1 || (want->last!=0x7fffffffffffffffLL && l->first==want->last+1))) {
    if(l->first<want->first)want->first=l->first;
    if(l->last>want->last)want->last=l->last;
    l->owner=0;i=(unsigned)-1;
   }
  }
  unsigned i=0;while(linux_file_locks[i].owner)i++;linux_file_locks[i]=*want;
 }
 return 0;
}


