#include "web_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main() {
    printf("=== test https://www.baidu.com/ ===\n");
    WebResult* r = web_search("https://www.baidu.com/", 10000, 32768);
    if (!r) { printf("web_search returned NULL\n"); return 1; }
    printf("OK: status=%d, title=%s, body_len=%d\n", r->status_code, r->title?r->title:"-", r->body_len);
    if (r->body_len > 0) {
        printf("BODY: %.200s\n", r->body);
    }
    web_result_free(r);
    return 0;
}
