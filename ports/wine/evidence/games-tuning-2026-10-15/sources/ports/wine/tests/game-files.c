#include <windows.h>
#include <stdio.h>
int main(void){
 setvbuf(stdout,NULL,_IONBF,0);char cwd[MAX_PATH]={0};GetCurrentDirectoryA(sizeof(cwd),cwd);printf("[GAME-FILES] cwd=%s drive-D=%u d-colon=%u\n",cwd,GetDriveTypeA("D:\\"),GetDriveTypeA("d:"));
 const char *paths[]={"D:\\install\\data\\quake2.exe","D:\\install\\data\\baseq2\\pak0.pak","D:\\fifa2000.exe","C:\\Program Files\\EA SPORTS\\FIFA 2000\\fifa2000.exe"};
 for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
  HANDLE f=CreateFileA(paths[i],GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
  printf("[GAME-FILES] path=%s handle=%p error=%lu",paths[i],f,GetLastError());
  if(f!=INVALID_HANDLE_VALUE){BYTE bytes[4]={0};DWORD n=0;DWORD size=GetFileSize(f,NULL);BOOL ok=ReadFile(f,bytes,sizeof(bytes),&n,NULL);printf(" size=%lu read=%d/%lu head=%02x%02x%02x%02x",size,ok,n,bytes[0],bytes[1],bytes[2],bytes[3]);CloseHandle(f);}puts("");
 }
 return 0;
}
