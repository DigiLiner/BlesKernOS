/* Minimal PE -> ntdll -> kernel32 -> wineserver gate, GPL-3.0-or-later. */
#include <windows.h>
int main(void)
{
    DWORD written = 0;
    const char text[] = "[WINE:X11] console Win32 OK\r\n";
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    return WriteFile(output, text, sizeof(text) - 1, &written, NULL) &&
           written == sizeof(text) - 1 ? 0 : 1;
}
