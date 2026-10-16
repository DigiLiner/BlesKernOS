/* Run with the original Quake II CD attached and mapped to D:.
 * i686-w64-mingw32-gcc -Os -fno-builtin -nostdlib -Wl,-e,_optical_main@0
 *   optical-drive.c -o optical-drive.exe -lkernel32 */
#include <windows.h>
static void report(const char *s){DWORD n;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,lstrlenA(s),&n,NULL);}
static BOOL starts_with(const char *s,const char *prefix)
{while(*prefix)if(*s++!=*prefix++)return FALSE;return TRUE;}
void WINAPI optical_main(void)
{
    char device[256]={0};
    if(GetDriveTypeA("C:\\")!=DRIVE_FIXED || GetDriveTypeA("D:\\")!=DRIVE_CDROM ||
       GetDriveTypeA("d:")!=DRIVE_CDROM){report("[OPTICAL] FAIL drive types\r\n");ExitProcess(1);}
    if(!QueryDosDeviceA("D:",device,sizeof(device)) ||
       !starts_with(device,"\\Device\\CdRom")){
        report("[OPTICAL] FAIL mount manager device: ");report(device);report("\r\n");ExitProcess(2);
    }
    HANDLE file=CreateFileA("d:\\install\\data\\quake2.exe",GENERIC_READ,FILE_SHARE_READ,
                            NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE){report("[OPTICAL] FAIL original CD file\r\n");ExitProcess(3);}
    char magic[2];DWORD count=0;BOOL ok=ReadFile(file,magic,2,&count,NULL);CloseHandle(file);
    if(!ok || count!=2 || magic[0]!='M' || magic[1]!='Z'){
        report("[OPTICAL] FAIL CD executable contents\r\n");ExitProcess(4);
    }
    report("[OPTICAL] PASS disk, CD root, legacy d:, mount manager and genuine CD read\r\n");ExitProcess(0);
}
