/* A real GUI-subsystem Wine client, without a console or a visible window.
 * Success means user32 can create a window and GDI can allocate a bitmap.
 * This primes the Windows desktop runtime without launching cmd/conhost. */
#include <windows.h>

void WINAPI wine_ready_entry(void)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"BlesWineReady";
    if (!RegisterClassW(&wc)) ExitProcess(1);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
                                 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (!window) ExitProcess(2);
    HDC dc = GetDC(window);
    HDC memory = dc ? CreateCompatibleDC(dc) : NULL;
    HBITMAP bitmap = dc ? CreateCompatibleBitmap(dc, 1, 1) : NULL;
    int result = dc && memory && bitmap ? 0 : 3;
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (dc) ReleaseDC(window, dc);
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ExitProcess(result);
}
