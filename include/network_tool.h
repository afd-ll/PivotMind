#ifdef _WIN32
#ifndef NETWORK_TOOL_H
#define NETWORK_TOOL_H

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* response_body;
    int status_code;
    size_t body_length;
    int http_error;
} HttpResponse;

typedef struct {
    char* url;
    char* method;
    char* headers;
    char* body;
    int timeout_ms;
} HttpRequest;

HttpResponse* http_get(const char* url, int timeout_ms);
HttpResponse* http_post(const char* url, const char* headers, const char* body, int timeout_ms);
HttpResponse* http_request(HttpRequest* request);
void http_response_free(HttpResponse* response);
char* url_encode(const char* input);
char* url_decode(const char* input);

#endif // NETWORK_TOOL_H
#endif // _WIN32