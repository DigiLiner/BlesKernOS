typedef struct __attribute__((packed)) {int16_t type,whence;int32_t start,length,pid;} linux_flock32;
typedef struct __attribute__((packed)) {int16_t type,whence;int64_t start,length;int32_t pid;} linux_flock64;
static int32_t linux_fcntl_lock(registers_t *r) {
 uint32_t command=r->ecx;bool wide=command>=12,query=command==5 || command==12,wait=command==7 || command==14;
 linux_flock64 f;linux_flock32 small;
 if(wide){if(!copy_from_user(&f,(void *)r->edx,sizeof f))return -BK_EFAULT;}
 else {if(!copy_from_user(&small,(void *)r->edx,sizeof small))return -BK_EFAULT;
  f=(linux_flock64){small.type,small.whence,small.start,small.length,small.pid};}
 if(query && !user_mapping_write_ok(r,(void *)r->edx,wide?sizeof f:sizeof small))return -BK_EFAULT;
 if(f.type<0 || f.type>2 || f.whence<0 || f.whence>2)return -BK_EINVAL;
 if(query && f.type==2)return -BK_EINVAL;
 kernel_domains_enter(KDOMAIN_VFS);
 uint32_t owner=task_current_process_id();syscall_resources_t *resource=resource_for(owner,false);
 int raw=resource_fd(resource,r->ebx);int32_t result=0;
 linux_file_lock want={.owner=owner,.type=f.type};
 if(raw<0){result=-BK_EBADF;goto done;}
 if(raw>=LINUX_IO_BASE){result=-BK_EBADF;goto done;}
 if(!vfs_fd_identity(raw,&want.device,&want.inode)){result=-BK_EBADF;goto done;}
 int mode=vfs_status_flags(raw)&3;
 if(!query && ((f.type==0 && mode==1) || (f.type==1 && mode==0))){result=-BK_EBADF;goto done;}
 int64_t base=f.whence==1?vfs_tell(raw):f.whence==2?vfs_size(raw):0;
 if(base<0 || (f.start>0 && base>0x7fffffffffffffffLL-f.start)){result=-75;goto done;}
 want.first=base+f.start;
 if(f.length<0) {
  if(want.first<=0 || f.length < -want.first){result=-BK_EINVAL;goto done;}
  want.last=want.first-1;want.first+=f.length;
 } else {
  if(want.first<0){result=-BK_EINVAL;goto done;}
  if(f.length && f.length-1>0x7fffffffffffffffLL-want.first){result=-75;goto done;}
  want.last=f.length?want.first+f.length-1:0x7fffffffffffffffLL;
 }
 for(;;) {
  uint32_t blocked;linux_file_lock attempt=want;
  result=linux_lock_apply(&attempt,query,&blocked);
  if(!result && query) {
   if(attempt.type==2)f.type=2;
   else f=(linux_flock64){attempt.type,0,attempt.first,attempt.last==0x7fffffffffffffffLL?0:attempt.last-attempt.first+1,attempt.owner};
   if(wide)result=copy_to_user((void *)r->edx,&f,sizeof f)?0:-BK_EFAULT;
   else if(f.start>0x7fffffffLL || f.length>0x7fffffffLL)result=-75;
   else {small=(linux_flock32){f.type,f.whence,f.start,f.length,f.pid};result=copy_to_user((void *)r->edx,&small,sizeof small)?0:-BK_EFAULT;}
  }
  if(result!=-11 || !wait)break;
  uint32_t dependency=blocked;
  for(unsigned i=0;dependency && i<SYSCALL_RESOURCE_SLOTS;i++) {
   if(dependency==owner){result=-35;break;}
   syscall_resources_t *other=resource_for(dependency,false);
   dependency=other?other->lock_wait_owner:0;
  }
  if(result==-35)break;
  resource->lock_wait_owner=blocked;
  if(task_linux_signal_pending()){result=-4;break;}
  linux_io_yield();
  if(resource_fd(resource,r->ebx)!=raw){result=-BK_EBADF;break;}
 }
done:
 if(resource)resource->lock_wait_owner=0;
 kernel_domains_exit(KDOMAIN_VFS);return result;
}
