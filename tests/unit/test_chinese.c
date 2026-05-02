#include <stdio.h>
#include <windows.h>
#include <locale.h>

// 设置控制台编码为UTF-8(Windows)
void set_console_utf8() {
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
}

int main() {
    // 尝试多种方法设置中文支持
    set_console_utf8();
    
    setlocale(LC_ALL, ".UTF-8");
    setlocale(LC_CTYPE, "Chinese_China.65001");
    
    printf("===========================================\n");
    printf("中文显示测试\n");
    printf("===========================================\n\n");
    
    printf("测试1: 基本中文\n");
    printf("你好！这是中文测试。\n\n");
    
    printf("测试2: 混合文本\n");
    printf("Hello 你好 Welcome 欢迎\n\n");
    
    printf("测试3: 特殊字符\n");
    printf("≠ ≤ ≥ ± × ÷ ∞ √ ∑ ∏\n\n");
    
    printf("测试4: 长文本\n");
    printf("这是一个测试程序，用于检查控制台是否能够正确显示中文字符。如果这段文字能够正常显示，说明中文支持已经配置正确。\n\n");
    
    printf("===========================================\n");
    printf("测试完成\n");
    printf("===========================================\n");
    
    return 0;
}
