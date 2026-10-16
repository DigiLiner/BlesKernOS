/* Stress the Windows interfaces used by large installers/editors. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
static DWORD WINAPI worker(void *data){
    unsigned seed=(unsigned)(ULONG_PTR)data;
    BYTE *p=VirtualAlloc(NULL,16*1024*1024,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!p)return 1;
    for(unsigned i=0;i<16*1024*1024;i+=4096)p[i]=(BYTE)(i/4096+seed);
    for(unsigned i=0;i<16*1024*1024;i+=4096)if(p[i]!=(BYTE)(i/4096+seed)){VirtualFree(p,0,MEM_RELEASE);return 2;}
    VirtualFree(p,0,MEM_RELEASE);return 0;
}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);DWORD began=GetTickCount();
    HANDLE threads[8];unsigned count=0;
    for(;count<8;count++){threads[count]=CreateThread(NULL,0,worker,(void *)(ULONG_PTR)count,0,NULL);if(!threads[count])break;}
    int failed=count!=8;
    if(count && WaitForMultipleObjects(count,threads,TRUE,120000)!=WAIT_OBJECT_0)failed=1;
    for(unsigned i=0;i<count;i++){DWORD result;if(!GetExitCodeThread(threads[i],&result)||result)failed=1;CloseHandle(threads[i]);}
    printf("[COMPAT:HEAVY] %s 8-threads-128MiB error=%lu\n",failed?"FAIL":"PASS",GetLastError());if(failed)return 1;
    HANDLE events[512];count=0;
    for(;count<512;count++){events[count]=CreateEventA(NULL,TRUE,FALSE,NULL);if(!events[count])break;}
    failed=count!=512;
    for(unsigned i=0;i<count;i++){
        if(!SetEvent(events[i])||WaitForSingleObject(events[i],1000)!=WAIT_OBJECT_0||!ResetEvent(events[i]))failed=1;
        CloseHandle(events[i]);
    }
    printf("[COMPAT:HEAVY] %s 512-windows-handles\n",failed?"FAIL":"PASS");if(failed)return 2;
    char temp[MAX_PATH],name[MAX_PATH];BYTE bytes[65536],check[65536];
    for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]=(BYTE)(i*17+37);
    if(!GetTempPathA(sizeof(temp),temp)||!GetTempFileNameA(temp,"bkc",0,name))return 3;
    HANDLE f=CreateFileA(name,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)return 4;
    DWORD n=0;
    for(unsigned i=0;i<64;i++)if(!WriteFile(f,bytes,sizeof(bytes),&n,NULL)||n!=sizeof(bytes)){failed=1;break;}
    if(!FlushFileBuffers(f))failed=1;
    HANDLE map=CreateFileMappingA(f,NULL,PAGE_WRITECOPY,0,0,NULL);
    BYTE *view=map?MapViewOfFile(map,FILE_MAP_COPY,0,0,65536):NULL;
    if(!view || view[0]!=37)failed=1;
    else {view[0]=99;if(view[0]!=99)failed=1;UnmapViewOfFile(view);}
    if(map)CloseHandle(map);
    SetFilePointer(f,0,NULL,FILE_BEGIN);
    if(!ReadFile(f,check,sizeof(check),&n,NULL)||n!=sizeof(check)||memcmp(check,bytes,sizeof(bytes)))failed=1;
    CloseHandle(f);if(!DeleteFileA(name))failed=1;
    printf("[COMPAT:HEAVY] %s 4MiB-file-flush-private-mapping elapsed_ms=%lu\n",failed?"FAIL":"PASS",GetTickCount()-began);
    return failed?5:0;
}
