#include "web_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main() {
    WebResult* r = web_search("http://www.people.com.cn/", 5000, 32768);
    if (!r) { printf("FAIL\n"); return 1; }
    printf("状态: %d, 标题: '%s'\n正文: %d字节\n", r->status_code, r->title?r->title:"-", r->body_len);
    if (r->body) {
        char* text = malloc(r->body_len+10);
        web_extract_text(r->body, text, r->body_len);
        printf("文本: %.300s\n", text);
        printf("关键词(%d): ", r->keyword_count);
        for(int i=0; i<r->keyword_count && i<10; i++) printf("%s ", r->keywords[i]);
        printf("\n");
        free(text);
    }
    web_result_free(r);
    return 0;
}
