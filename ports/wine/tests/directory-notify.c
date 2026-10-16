/* Windows integration regression for Wine's Linux inotify backend. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){printf("[WIN-NOTIFY] line=%d error=%lu FAIL\n",__LINE__,GetLastError());return 1;}}while(0)
static const char *root="Z:\\WINEGATE\\notify-win32";
static int arm(HANDLE directory,HANDLE event,OVERLAPPED *ov,void *buffer,BOOL subtree){
 memset(ov,0,sizeof *ov);ov->hEvent=event;ResetEvent(event);
 return ReadDirectoryChangesW(directory,buffer,4096,subtree,
  FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_LAST_WRITE,
  NULL,ov,NULL);
}
static int found(HANDLE directory,OVERLAPPED *ov,void *buffer,DWORD action,const wchar_t *name){
 DWORD n=0;if(WaitForSingleObject(ov->hEvent,5000)!=WAIT_OBJECT_0)return 0;
 if(!GetOverlappedResult(directory,ov,&n,FALSE) || !n)return 0;
 for(DWORD at=0;at+12<=n;){
  FILE_NOTIFY_INFORMATION *info=(void *)((BYTE *)buffer+at);
  if(info->FileNameLength>n-at-12)return 0;
  size_t len=wcslen(name);
  if(info->Action==action && info->FileNameLength==len*sizeof(wchar_t) &&
     !memcmp(info->FileName,name,info->FileNameLength))return 1;
  if(!info->NextEntryOffset || info->NextEntryOffset>n-at)break;
  at+=info->NextEntryOffset;
 }
 return 0;
}
int main(void){
 setbuf(stdout,NULL);CreateDirectoryA(root,NULL);
 char a[MAX_PATH],b[MAX_PATH],sub[MAX_PATH],nested[MAX_PATH];
 snprintf(a,sizeof a,"%s\\before.txt",root);snprintf(b,sizeof b,"%s\\after.txt",root);
 snprintf(sub,sizeof sub,"%s\\sub",root);snprintf(nested,sizeof nested,"%s\\sub\\deep.txt",root);
 DeleteFileA(a);DeleteFileA(b);DeleteFileA(nested);RemoveDirectoryA(sub);
 HANDLE dir=CreateFileA(root,FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
  NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,NULL);
 CHECK(dir!=INVALID_HANDLE_VALUE);puts("[WIN-NOTIFY] directory-open PASS");
 HANDLE event=CreateEventA(NULL,TRUE,FALSE,NULL);CHECK(event);
 OVERLAPPED ov;BYTE data[4096];
 CHECK(arm(dir,event,&ov,data,FALSE));
 HANDLE f=CreateFileA(a,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_ALWAYS,0,NULL);
 CHECK(f!=INVALID_HANDLE_VALUE);CloseHandle(f);
 CHECK(found(dir,&ov,data,FILE_ACTION_ADDED,L"before.txt"));
 puts("[WIN-NOTIFY] create / ReadDirectoryChangesW PASS");
 CHECK(arm(dir,event,&ov,data,FALSE));
 CHECK(MoveFileA(a,b));CHECK(found(dir,&ov,data,FILE_ACTION_RENAMED_OLD_NAME,L"before.txt"));
 puts("[WIN-NOTIFY] rename PASS");
 CHECK(arm(dir,event,&ov,data,FALSE));
 f=CreateFileA(b,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);CHECK(f!=INVALID_HANDLE_VALUE);
 DWORD written;CHECK(WriteFile(f,"updated",7,&written,NULL) && written==7);CloseHandle(f);
 CHECK(found(dir,&ov,data,FILE_ACTION_MODIFIED,L"after.txt"));
 puts("[WIN-NOTIFY] modification PASS");
 CHECK(arm(dir,event,&ov,data,FALSE));CHECK(DeleteFileA(b));
 CHECK(found(dir,&ov,data,FILE_ACTION_REMOVED,L"after.txt"));
 puts("[WIN-NOTIFY] delete PASS");
 /* Wine keeps the filter/subtree configuration from the first request on a handle. */
 CloseHandle(dir);
 dir=CreateFileA(root,FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
  NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,NULL);
 CHECK(dir!=INVALID_HANDLE_VALUE);
 CHECK(CreateDirectoryA(sub,NULL));CHECK(arm(dir,event,&ov,data,TRUE));
 /* Opening the existing child lets Wine register its recursive inode watch. */
 HANDLE child=CreateFileA(sub,FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
  NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);CHECK(child!=INVALID_HANDLE_VALUE);
 f=CreateFileA(nested,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_ALWAYS,0,NULL);CHECK(f!=INVALID_HANDLE_VALUE);CloseHandle(f);
 CHECK(found(dir,&ov,data,FILE_ACTION_ADDED,L"sub\\deep.txt"));
 CloseHandle(child);
 puts("[WIN-NOTIFY] recursive opened directory PASS");
 CloseHandle(event);CloseHandle(dir);DeleteFileA(nested);RemoveDirectoryA(sub);
 HANDLE change=FindFirstChangeNotificationA(root,FALSE,FILE_NOTIFY_CHANGE_FILE_NAME);CHECK(change!=INVALID_HANDLE_VALUE);
 f=CreateFileA(a,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);CHECK(f!=INVALID_HANDLE_VALUE);CloseHandle(f);
 CHECK(WaitForSingleObject(change,5000)==WAIT_OBJECT_0 && FindNextChangeNotification(change));
 CHECK(WaitForSingleObject(change,0)==WAIT_TIMEOUT);CHECK(DeleteFileA(a));
 CHECK(WaitForSingleObject(change,5000)==WAIT_OBJECT_0 && FindCloseChangeNotification(change));
 CHECK(RemoveDirectoryA(root));puts("[WIN-NOTIFY] FindFirst/NextChangeNotification and reset PASS");
 puts("[WIN-NOTIFY] ALL PASS");return 0;
}
