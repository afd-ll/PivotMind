#include "web_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main() {
    printf("=== HTTPS example.com ===\n");
    WebResult* r = web_search("https://example.com/", 10000, 16384);
    if (!r) { printf("FAIL\n"); return 1; }
    printf("状态: %d, 标题: '%s', 正文: %d字节\n", r->status_code, r->title?r->title:"-", r->body_len);
    if (r->body) {
        char* text = malloc(r->body_len+10);
        web_extract_text(r->body, text, r->body_len);
        printf("文本: %.200s\n", text);
        free(text);
    }
    printf("关键词(%d): ", r->keyword_count);
    for(int i=0; i<r->keyword_count && i<10; i++) printf("%s ", r->keywords[i]);
    printf("\n");
    web_result_free(r);
    
    printf("\n=== HTTPS 百度百科 温度 ===\n");
    r = web_search("https://baike.baidu.com/item/%E6%B8%A9%E5%BA%A6", 15000, 131072);
    if (!r) { printf("FAIL\n"); return 1; }
    printf("状态: %d, 标题: '%s', 正文: %d字节\n", r->status_code, r->title?r->title:"-", r->body_len);
    if (r->body) {
        char* text = malloc(r->body_len+10);
        int tl = web_extract_text(r->body, text, r->body_len);
        printf("文本(%d字): %.400s\n", tl, text);
        printf("关键词(%d): ", r->keyword_count);
        for(int i=0; i<r->keyword_count && i<15; i++) printf("%s ", r->keywords[i]);
        printf("\n");
        free(text);
    }
    web_result_free(r);
    return 0;
}
