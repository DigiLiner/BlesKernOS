/* Wine X11 milestone, GPL-3.0-or-later. */
#include <windows.h>

static DWORD WINAPI close_dialog(LPVOID unused)
{
    (void)unused;
    Sleep(25000);
    for (int i = 0; i < 50; ++i) {
        HWND dialog = FindWindowA("#32770", "Wine 11.0 sobre BlesKernOS");
        if (dialog) {
            PostMessageA(dialog, WM_COMMAND, IDOK, 0);
            return 0;
        }
        Sleep(100);
    }
    return 1;
}

static void marker(const char *path, const char *value)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, value, lstrlenA(value), &written, NULL);
        CloseHandle(file);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command,
                   int show)
{
    (void)instance; (void)previous; (void)command; (void)show;
    HANDLE closer = CreateThread(NULL, 0, close_dialog, NULL, 0, NULL);
    marker("Z:\\WINEGATE\\messagebox.enter", "MessageBoxA entered\n");
    int result = MessageBoxA(NULL,
        "Primer test grafico Win32 ejecutado por Wine 11.0 y TinyX",
        "Wine 11.0 sobre BlesKernOS", MB_OK | MB_ICONINFORMATION);
    if (closer) CloseHandle(closer);
    marker("Z:\\WINEGATE\\messagebox.done", result == IDOK ? "IDOK\n" : "FAILED\n");
    return result == IDOK ? 0 : 1;
}
