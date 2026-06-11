#include "web_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main() {
    printf("=== example.com ===\n");
    WebResult* r = web_search("http://example.com/", 5000, 16384);
    if (!r) { printf("FAIL\n"); return 1; }
    printf("状态: %d\n标题: '%s'\n正文: %d字节\n", r->status_code, r->title?r->title:"-", r->body_len);
    printf("---提取文本---\n");
    if (r->body) {
        char* text = malloc(r->body_len + 10);
        int tl = web_extract_text(r->body, text, r->body_len);
        printf("%.500s\n", text);
        free(text);
    }
    printf("---关键词(%d)---\n", r->keyword_count);
    for(int i=0; i<r->keyword_count; i++) printf("  [%d] %s\n", i, r->keywords[i]);
    web_result_free(r);
    return 0;
}
