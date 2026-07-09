#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
int main() {
    HINTERNET s = WinHttpOpen(L"Test/1.0", 0, 0, 0, 0);
    printf("S=%p\n", s);
    HINTERNET c = WinHttpConnect(s, L"baike.baidu.com", 443, 0);
    printf("C=%p err=%lu\n", c, GetLastError());
    if (!c) { WinHttpCloseHandle(s); return 1; }
    HINTERNET r = WinHttpOpenRequest(c, L"GET", L"/item/%E6%B8%A9%E5%BA%A6", 0,0,0, WINHTTP_FLAG_SECURE);
    printf("R=%p err=%lu\n", r, GetLastError());
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); return 1; }
    DWORD sf = 0x00000100|0x00000200|0x00001000;
    WinHttpSetOption(r, 31, &sf, 4);
    if (!WinHttpSendRequest(r, 0,0,0,0,0,0)) printf("Send err=%lu\n", GetLastError());
    else if (!WinHttpReceiveResponse(r, 0)) printf("Recv err=%lu\n", GetLastError());
    else {
        DWORD st=0,sz=4;
        WinHttpQueryHeaders(r, 19|0x20000000, 0, &st, &sz, 0);
        printf("Status=%lu\n", st);
        char b[9999]; DWORD rd;
        while(WinHttpReadData(r,b,9998,&rd)&&rd>0){b[rd]=0;printf("%.500s",b);}
    }
    WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return 0;
}
