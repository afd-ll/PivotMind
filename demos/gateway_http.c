/**
 * @file gateway_http.c
 * @brief PivotMind HTTP Gateway — JSON/HTTP 工具 + HTTP 请求解析
 *
 * 由 demos/pivotmind_gateway.c 拆分而来（v0.5.25 P2-6）。
 * 共享类型与原型见 gateway_internal.h。
 */

#include "gateway_internal.h"

// ==================== JSON 工具 ====================

// 返回 UTF-8 序列长度：1/2/3/4，0=无效字节
static int utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xC0) == 0x80) return 0;   /* 孤立的续字节 */
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

static int utf8_valid(const unsigned char* s) {
    int len = utf8_seq_len(s[0]);
    if (len == 0) return 0;
    for (int k = 1; k < len; k++)
        if (!s[k] || (s[k] & 0xC0) != 0x80) return 0;
    return len;
}

// 简易 JSON 字符串转义 + UTF-8 校验 (跳过无效字节避免 JSON 崩坏)
int json_escape(const char* src, char* dst, int dst_size) {
    if (!src || !dst || dst_size < 2) return -1;
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 8; i++) {
        unsigned char c = (unsigned char)src[i];

        /* 控制字符 (0x00-0x1F, 不包括 \t \n \r) → \uXXXX */
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            j += snprintf(dst + j, 8, "\\u%04x", c);
            continue;
        }

        /* 多字节 UTF-8 校验 */
        if (c >= 0x80) {
            int len = utf8_valid((const unsigned char*)&src[i]);
            if (len <= 1) continue;  /* 无效 UTF-8 → 跳过 */
            for (int k = 0; k < len && j < dst_size - 2; k++)
                dst[j++] = src[i + k];
            i += len - 1;
            continue;
        }

        switch (src[i]) {
            case '"':  if (j + 2 >= dst_size) goto done; dst[j++] = '\\'; dst[j++] = '"';  break;
            case '\\': if (j + 2 >= dst_size) goto done; dst[j++] = '\\'; dst[j++] = '\\'; break;
            case '\n': if (j + 2 >= dst_size) goto done; dst[j++] = '\\'; dst[j++] = 'n';  break;
            case '\r': if (j + 2 >= dst_size) goto done; dst[j++] = '\\'; dst[j++] = 'r';  break;
            case '\t': if (j + 2 >= dst_size) goto done; dst[j++] = '\\'; dst[j++] = 't';  break;
            default:   dst[j++] = src[i]; break;
        }
    }
done:
    dst[j] = '\0';
    return j;
}

// 从 JSON body 提取 "key":"value" (简单实现，不处理嵌套)
char* json_extract_string(const char* json, const char* key, char* buf, int buf_size) {
    if (!json || !key || !buf || buf_size < 1) return NULL;

    // 构建 "key" 搜索模式
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* pos = strstr(json, pattern);
    if (!pos) return NULL;

    pos += strlen(pattern);
    // 跳过空白和冒号
    while (*pos == ' ' || *pos == ':' || *pos == '\t') pos++;
    if (*pos != '"') return NULL;
    pos++; // 跳过开头引号

    int i = 0;
    while (*pos && *pos != '"' && i < buf_size - 1) {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;
            /* \uXXXX → UTF-8 解码：读 4 位 hex，编码为 UTF-8 字节 */
            if (*pos == 'u') {
                unsigned int cp = 0;
                int valid = 1;
                for (int k = 1; k <= 4; k++) {
                    unsigned char hc = (unsigned char)pos[k];
                    if (!hc || !isxdigit(hc)) { valid = 0; break; }
                    cp = cp * 16 + (unsigned int)(hc >= '0' && hc <= '9' ? hc - '0' :
                                                  hc >= 'a' && hc <= 'f' ? hc - 'a' + 10 :
                                                  hc >= 'A' && hc <= 'F' ? hc - 'A' + 10 : 0);
                }
                if (valid && cp > 0) {
                    if (cp < 0x80) {
                        if (i + 1 >= buf_size) break;
                        buf[i++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (i + 2 >= buf_size) break;
                        buf[i++] = (char)(0xC0 | (cp >> 6));
                        buf[i++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (i + 3 >= buf_size) break;
                        buf[i++] = (char)(0xE0 | (cp >> 12));
                        buf[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[i++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                pos += 5;  /* 跳过 'u' + 4 位 hex */
                continue;  /* 跳过循环尾部的 pos++ */
            }
            switch (*pos) {
                case 'n':  buf[i++] = '\n'; break;
                case 'r':  buf[i++] = '\r'; break;
                case 't':  buf[i++] = '\t'; break;
                case '"':  buf[i++] = '"';  break;
                case '\\': buf[i++] = '\\'; break;
                default:   buf[i++] = *pos; break;
            }
        } else {
            buf[i++] = *pos;
        }
        pos++;
    }
    buf[i] = '\0';
    return (i > 0) ? buf : NULL;
}

// ==================== HTTP 工具 ====================

// 发送 HTTP 响应 (v0.5.1: 加固 — 检查 write 返回值, 防 EPIPE/ECONNRESET 丢日志)
void http_send(int fd, int status, const char* content_type, const char* body) {
    const char* status_text = (status == 200) ? "OK" :
                              (status == 400) ? "Bad Request" :
                              (status == 404) ? "Not Found" :
                              (status == 413) ? "Payload Too Large" :
                              "Internal Server Error";

    char header[1024];
    int body_len = (int)strlen(body);
    int hdr_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);

    /* MSG_NOSIGNAL: write 到已关闭 socket 时返回 -1 而非杀进程 */
    ssize_t sent = send(fd, header, hdr_len, MSG_NOSIGNAL);
    if (sent >= 0)
        send(fd, body, body_len, MSG_NOSIGNAL);
    /* sent < 0: 客户端已断开, 静默忽略 */
}

// 发送 JSON 响应
void http_json(int fd, int status, const char* json_body) {
    http_send(fd, status, "application/json; charset=utf-8", json_body);
}

// ==================== HTTP 请求解析 ====================

/* C1 加固: 常量时间 token 比较。
 * 旧版 strcmp 逐字节提前返回，攻击者可借响应时间逐位爆破 token；
 * 改为全程遍历 + XOR 累积，比较耗时与命中前缀长度无关。长度不同
 * 直接拒绝（64 位 hex token 的长度本身不构成可利用信息）。 */
int gw_token_equal(const char* a, const char* b) {
    if (!a || !b) return 0;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}



int parse_request(int fd, HttpRequest* req) {
    memset(req, 0, sizeof(HttpRequest));

    // 读取请求 (chunked read, 每轮 4KB)
    /* P1-4: 64KB 读缓冲从连接线程栈搬到堆（与 H2 把 HttpRequest 上堆配套，
     * 两处合起来彻底清掉每连接 ~128KB 的栈占用）。 */
    char* buf = (char*)malloc(GW_MAX_REQUEST);
    if (!buf) return -1;
    int total = 0;
    int header_end = -1;

    // 设读取超时
    struct timeval tv = { .tv_sec = GW_READ_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (total < GW_MAX_REQUEST - 1) {
        int chunk = GW_MAX_REQUEST - 1 - total;
        if (chunk > 4096) chunk = 4096;  /* 一次读 4KB, 非逐字节 */
        int n = recv(fd, buf + total, chunk, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';

        // 检查 header 结束 (\r\n\r\n)
        if (header_end < 0 && total >= 4) {
            for (int i = 0; i <= total - 4; i++) {
                if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
                    header_end = i;
                    break;
                }
            }
        }

        // header 已读完，检查 Content-Length 继续读 body
        if (header_end >= 0) {
            int header_size = header_end + 4;
            int body_received = total - header_size;

            // 找 Content-Length（只在 header 区间 [0, header_end) 内查找，
            // 防止 body 中出现同名文本被误解析为头部；数字用 strtol 校验）
            char* cl = strcasestr(buf, "Content-Length:");
            if (cl && cl < buf + header_end) {
                char* num = cl + 15;  /* "Content-Length:" 占 15 字节 */
                while (*num == ' ' || *num == '\t') num++;
                char* endp = NULL;
                long content_length = strtol(num, &endp, 10);
                if (endp == num) content_length = -1;  /* 非法数字头 */
                if (content_length > GW_MAX_REQUEST - 4096) { free(buf); return -1; } /* 保护: 留出 header 空间 */
                if (content_length >= 0 && body_received >= content_length) break; // body 完整
                /* content_length < 0（非法头）时继续收完剩余缓冲后按无 body 处理 */
            } else {
                break; // 无 body 或头不在 header 区（忽略 body 内同名文本）
            }
        }
    }

    if (total == 0 || header_end < 0) { free(buf); return -1; }

    // 解析方法
    char* p = buf;
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(req->method) - 1) req->method[i++] = *p++;
    req->method[i] = '\0';
    if (*p == ' ') p++;

    // 解析路径
    i = 0;
    while (*p && *p != ' ' && *p != '?' && i < (int)sizeof(req->path) - 1) req->path[i++] = *p++;
    req->path[i] = '\0';

    // 提取 body (带边界保护)
    int header_size = header_end + 4;
    req->body_len = total - header_size;
    if (req->body_len > 0 && req->body_len < (int)sizeof(req->body)) {
        memcpy(req->body, buf + header_size, req->body_len);
        req->body[req->body_len] = '\0';
    } else if (req->body_len >= (int)sizeof(req->body)) {
        free(buf); return -1;  /* body 溢出 */
    }

    /* C1: 提取 X-Pivot-Token 请求头（只在 header 段找，防 body 伪造） */
    char* tok = strcasestr(buf, "X-Pivot-Token:");
    if (tok && tok < buf + header_end) {
        tok += 14;  /* 跳过 "X-Pivot-Token:" */
        while (*tok == ' ' || *tok == '\t') tok++;
        int ti = 0;
        while (*tok && *tok != '\r' && *tok != '\n' && ti < (int)sizeof(req->token) - 1)
            req->token[ti++] = *tok++;
        req->token[ti] = '\0';
    }

    free(buf);
    return 0;
}
