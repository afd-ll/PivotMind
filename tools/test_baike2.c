#include "web_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main() {
    printf("=== 百度百科: 温度 ===\n");
    WebResult* r = web_search("https://baike.baidu.com/item/%E6%B8%A9%E5%BA%A6", 15000, 65536);
    if (!r) { printf("FAIL\n"); return 1; }
    printf("状态: %d\n标题: '%s'\n正文: %d字节\n",
        r->status_code, r->title?r->title:"-", r->body_len);
    if (r->body_len > 0) {
        char* text = malloc(r->body_len+10);
        int tl = web_extract_text(r->body, text, r->body_len);
        printf("文本(%d字): %.400s\n", tl, text);
        printf("关键词(%d): ", r->keyword_count);
        for(int i=0; i<r->keyword_count && i<15; i++)
            printf("%s ", r->keywords[i]);
        printf("\n");
        free(text);
    }
    web_result_free(r);
    return 0;
}
