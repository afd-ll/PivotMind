#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
int main() {
    HINTERNET hSession = WinHttpOpen(L"Test/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    printf("Session: %p\n", hSession);
    if (!hSession) { printf("WinHttpOpen failed: %lu\n", GetLastError()); return 1; }
    
    HINTERNET hConnect = WinHttpConnect(hSession, L"www.baidu.com", 443, 0);
    printf("Connect: %p\n", hConnect);
    if (!hConnect) { printf("Connect failed: %lu\n", GetLastError()); WinHttpCloseHandle(hSession); return 1; }
    
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", L"/", NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    printf("Request: %p\n", hReq);
    if (!hReq) { printf("Request failed: %lu\n", GetLastError()); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return 1; }
    
    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    
    if (!WinHttpSendRequest(hReq, NULL, 0, NULL, 0, 0, 0)) {
        printf("Send failed: %lu\n", GetLastError());
    } else if (!WinHttpReceiveResponse(hReq, NULL)) {
        printf("Receive failed: %lu\n", GetLastError());
    } else {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status, &sz, NULL);
        printf("Status: %lu\n", status);
        
        char buf[4096]; DWORD read;
        while (WinHttpReadData(hReq, buf, sizeof(buf)-1, &read) && read > 0) {
            buf[read] = 0;
            printf("%s", buf);
        }
    }
    
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return 0;
}
