/* Diagnostic for the owned minimal CD installation. Reports the child exit
 * code while retaining the CD data path and windowed OpenGL arguments. */
#include <windows.h>
#include <stdio.h>
int main(void) {
 STARTUPINFOA si={.cb=sizeof(si)};PROCESS_INFORMATION pi;
 char command[]="\"C:\\Quake2\\quake2.exe\" +set cddir D:\\install\\data +set basedir C:\\Quake2 +set logfile 2 +set vid_ref gl +set gl_driver opengl32 +set vid_fullscreen 0";
 setbuf(stdout,NULL);puts("[QUAKE-PROCESS] begin windowed OpenGL, CD assets");
 if(!CreateProcessA(NULL,command,NULL,NULL,FALSE,0,NULL,"C:\\Quake2",&si,&pi)){printf("[QUAKE-PROCESS] create error=%lu\n",GetLastError());return 1;}
 WaitForSingleObject(pi.hProcess,INFINITE);DWORD code;GetExitCodeProcess(pi.hProcess,&code);printf("[QUAKE-PROCESS] exited=%lu (0x%lx)\n",code,code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code;
}
