#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
int main() {
    HINTERNET s = WinHttpOpen(L"T/1", 0,0,0,0);
    /* Test: MultiByteToWideChar */
    wchar_t wh[256], wp[2048];
    MultiByteToWideChar(CP_UTF8, 0, "baike.baidu.com", -1, wh, 256);
    MultiByteToWideChar(CP_UTF8, 0, "/item/%E6%B8%A9%E5%BA%A6", -1, wp, 2048);
    printf("host len=%d, path len=%d\n", (int)wcslen(wh), (int)wcslen(wp));
    HINTERNET c = WinHttpConnect(s, wh, 443, 0);
    printf("C=%p err=%lu\n", c, GetLastError());
    if (!c) { WinHttpCloseHandle(s); return 1; }
    HINTERNET r = WinHttpOpenRequest(c, L"GET", wp, 0,0,0, WINHTTP_FLAG_SECURE);
    printf("R=%p err=%lu\n", r, GetLastError());
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); return 1; }
    DWORD sf=0x100|0x200|0x1000; WinHttpSetOption(r,31,&sf,4);
    if(!WinHttpSendRequest(r,0,0,0,0,0,0)) printf("Send err=%lu\n",GetLastError());
    else if(!WinHttpReceiveResponse(r,0)) printf("Recv err=%lu\n",GetLastError());
    else { DWORD st=0,sz=4;
           WinHttpQueryHeaders(r,19|0x20000000,0,&st,&sz,0);
           printf("Status=%lu\n", st); }
    WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return 0;
}
