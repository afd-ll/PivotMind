/**
 * @file test_chinese.c
 * @brief UTF-8 / CJK 编码不变量的单元测试（原为纯 printf 演示，P1-11 改造）。
 *
 * 说明：本测试**故意不**断言项目内的 is_chinese()，因为它的语义已被认定为
 * 过宽（把一切非 ASCII 当中文，会把无效字节也当中文，属另一条待修 finding）。
 * 这里只断言与实现无关的 UTF-8 编码事实，保证测试自身可复现、不与已知 bug 耦合。
 *
 * 平台：Linux/Windows 均可编译运行（不再需要 windows.h → CI 不再需要 skip）。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        tests_run++;                                                        \
        if (!(cond)) {                                                      \
            tests_failed++;                                                 \
            printf("FAILED: %s (line %d)\n", (msg), __LINE__);               \
        }                                                                   \
    } while (0)

int main(void) {
    printf("===========================================\n");
    printf("  UTF-8 / CJK Encoding Tests\n");
    printf("===========================================\n\n");

    /* Test 1: 源码本身是合法 UTF-8，且 4 个 CJK 字符 = 12 字节。
     * 期望值心算：每个常用汉字（U+4E00..U+9FFF）在 UTF-8 中占 3 字节 → 4*3=12。 */
    const char* s = "中文测试";
    size_t n = strlen(s);
    printf("Test 1: \"%s\" strlen=%zu (expect 12)\n", s, n);
    CHECK(n == 12, "4 CJK chars must occupy 12 bytes in UTF-8");

    /* Test 2: 每个汉字 3 字节，首字节 1110xxxx，后两字节 10xxxxxx。 */
    int shape_ok = 1;
    for (size_t i = 0; i + 2 < n + 1 && i < n; i += 3) {
        unsigned char b0 = (unsigned char)s[i];
        unsigned char b1 = (unsigned char)s[i + 1];
        unsigned char b2 = (unsigned char)s[i + 2];
        if ((b0 & 0xF0) != 0xE0) shape_ok = 0;
        if ((b1 & 0xC0) != 0x80) shape_ok = 0;
        if ((b2 & 0xC0) != 0x80) shape_ok = 0;
    }
    printf("Test 2: per-char UTF-8 lead/continuation bytes (expect ok=1)\n");
    CHECK(shape_ok == 1, "each CJK char must be 3-byte UTF-8 (E0-EF / 80-BF / 80-BF)");

    /* Test 3: ASCII 与中文的字节边界：ASCII 占 1 字节，非 ASCII 首字节 >= 0x80。
     * 期望：混合串 "A中B" = 1 + 3 + 1 = 5 字节。 */
    const char* mix = "A\xE4\xB8\xAD" "B"; /* A + 中(U+4E2D) + B */
    printf("Test 3: mixed \"A<CJK>B\" strlen=%zu (expect 5)\n", strlen(mix));
    CHECK(strlen(mix) == 5, "mixed ASCII+CJK string must be 5 bytes");
    CHECK((unsigned char)mix[0] == 'A', "first byte must be ASCII 'A'");
    CHECK((unsigned char)mix[1] >= 0x80, "CJK lead byte must be >= 0x80");
    CHECK((unsigned char)mix[4] == 'B', "last byte must be ASCII 'B'");

    /* Test 4: 汉字仍在 BMP 常用区（U+4E00..U+9FFF），故最高字节 E4..E9。
     * 这是对“测试输入选得对”的自检，防止有人把表情符号塞进来当汉字。 */
    {
        unsigned char b0 = (unsigned char)s[0];
        printf("Test 4: first CJK lead byte=0x%02X (expect in E4..E9)\n", b0);
        CHECK(b0 >= 0xE4 && b0 <= 0xE9, "common CJK lead byte must be within E4..E9");
    }

    printf("\n===========================================\n");
    printf("Total: %d  Failed: %d\n", tests_run, tests_failed);
    printf("===========================================\n");
    return (tests_failed == 0) ? 0 : 1;
}
