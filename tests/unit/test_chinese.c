#include <stdio.h>
#include <locale.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
void set_console_utf8(void) {
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
}
#endif

int main(void) {
#ifdef _WIN32
    set_console_utf8();
    setlocale(LC_CTYPE, "Chinese_China.65001");
#endif
    setlocale(LC_ALL, ".UTF-8");

    printf("===========================================\n");
    printf("Chinese display test\n");
    printf("===========================================\n\n");

    printf("Test 1: Basic Chinese\n");
    printf("Hello! This is a Chinese test.\n\n");

    printf("Test 2: Mixed text\n");
    printf("Hello ni-hao Welcome huan-ying\n\n");

    printf("Test 3: Special characters\n");
    printf("!= <= >= +- */ inf sqrt sum prod\n\n");

    printf("Test 4: Long text\n");
    printf("This is a test program to check whether the console can correctly display Chinese. If this text displays correctly it means Chinese support is properly configured.\n\n");

    printf("===========================================\n");
    printf("Test complete\n");
    printf("===========================================\n");

    return 0;
}
