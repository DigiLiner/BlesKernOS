/* Small Win32 playback probe, without CRT dependencies.
 * i686-w64-mingw32-gcc -Os -fno-builtin -nostdlib -Wl,-e,_audio_main@0
 *   audio-compat.c -o audio.exe -lkernel32 -ladvapi32 -lwinmm */
#include <windows.h>
#include <mmsystem.h>
static void log_text(const char *s) {
    DWORD written,n=0;while(s[n])++n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&written,NULL);
}
static void log_number(const char *label,DWORD value) {
    char digits[16];unsigned i=0;log_text(label);
    do{digits[i++]=(char)('0'+value%10);value/=10;}while(value);
    while(i){char c[2]={digits[--i],0};log_text(c);}log_text("\r\n");
}
void __stdcall audio_main(void) {
    WAVEFORMATEX format={WAVE_FORMAT_PCM,1,11025,11025,1,8,0};
    HWAVEOUT output;MMRESULT result;static unsigned char samples[5512];WAVEHDR header={0};
    HKEY key;char driver[64]={0};DWORD bytes=sizeof(driver);
    if(!RegOpenKeyExA(HKEY_CURRENT_USER,"Software\\Wine\\Drivers",0,KEY_READ,&key)) {
        RegQueryValueExA(key,"Audio",NULL,NULL,(BYTE *)driver,&bytes);RegCloseKey(key);
    }
    /* Force this backend in the disposable test prefix, independently of theme import. */
    if(!RegCreateKeyExA(HKEY_CURRENT_USER,"Software\\Wine\\Drivers",0,NULL,0,
                       KEY_SET_VALUE,NULL,&key,NULL)) {
        RegSetValueExA(key,"Audio",0,REG_SZ,(const BYTE *)"oss",4);RegCloseKey(key);
    }
    log_text("[AUDIO] configured driver=");log_text(driver);log_text("\r\n");
    HMODULE module=LoadLibraryA("wineoss.drv");
    log_number("[AUDIO] driver load error=",module?0:GetLastError());
    log_number("[AUDIO] waveOut devices=",waveOutGetNumDevs());
    result=waveOutOpen(&output,WAVE_MAPPER,&format,0,0,CALLBACK_NULL);
    if(result){log_number("[AUDIO] FAIL open=",result);ExitProcess(1);}
    for(unsigned i=0;i<sizeof(samples);++i)samples[i]=(i%25)<12?160:96;
    header.lpData=(char *)samples;header.dwBufferLength=sizeof(samples);
    result=waveOutPrepareHeader(output,&header,sizeof(header));
    if(!result)result=waveOutWrite(output,&header,sizeof(header));
    if(result){log_number("[AUDIO] FAIL submit=",result);waveOutReset(output);waveOutClose(output);ExitProcess(2);}
    unsigned wait;
    for(wait=0;wait<500 && !(header.dwFlags&WHDR_DONE);++wait)Sleep(10);
    if(!(header.dwFlags&WHDR_DONE))waveOutReset(output);
    waveOutUnprepareHeader(output,&header,sizeof(header));waveOutClose(output);
    log_text(wait<500?"[AUDIO] PASS playback completion\r\n":"[AUDIO] FAIL playback timeout\r\n");
    ExitProcess(wait<500?0:3);
}
