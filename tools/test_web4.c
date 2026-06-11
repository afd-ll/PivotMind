#include "web_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main() {
    printf("=== people.com.cn 搜索 人工智能 ===\n");
    WebResult* r = web_search("http://search.people.com.cn/cnpeople/search.do?keyword=人工智能", 8000, 65536);
    if (!r) { printf("FAIL\n"); return 1; }
    printf("状态: %d, 标题: '%s'\n正文: %d字节\n", r->status_code, r->title?r->title:"-", r->body_len);
    char* text = malloc(r->body_len+10);
    int tl = web_extract_text(r->body, text, r->body_len);
    printf("提取文本(%d字): %.500s\n", tl, text);
    printf("关键词(%d): ", r->keyword_count);
    for(int i=0; i<r->keyword_count; i++) printf("%s ", r->keywords[i]);
    printf("\n");
    free(text);
    web_result_free(r);
    return 0;
}
