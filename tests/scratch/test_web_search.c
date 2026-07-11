/**
 * 独立测试 web_search 模块
 * 用法: build/bin/test_web_search.exe [关键词]
 */
#include "web_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    const char* query = argc > 1 ? argv[1] : "温度";

    char url[1024];
    snprintf(url, sizeof(url), "https://baike.baidu.com/item/%s", query);
    printf("=== 测试搜索: %s ===\n", url);

    WebResult* r = web_search(url, 10000, 65536);
    if (!r) {
        printf("搜索失败: 无法连接\n");
        return 1;
    }

    printf("状态码: %d\n", r->status_code);
    printf("URL: %s\n", r->url ? r->url : "(null)");
    printf("标题: %s\n", r->title ? r->title : "(无)");
    printf("正文长度: %d 字节\n", r->body_len);
    printf("--- 前500字节 ---\n");
    if (r->body) {
        char preview[501];
        int pl = r->body_len < 500 ? r->body_len : 500;
        memcpy(preview, r->body, pl);
        preview[pl] = '\0';
        printf("%s\n", preview);
    }

    printf("--- 关键词 (%d个) ---\n", r->keyword_count);
    for (int i = 0; i < r->keyword_count; i++) {
        printf("  [%d] %s\n", i, r->keywords[i]);
    }

    web_result_free(r);
    printf("=== 测试完成 ===\n");
    return 0;
}
