#pragma once  
#ifdef _WIN32  
#include <windows.h>  
#include <stdio.h>  
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {  
EXCEPTION_RECORD* er = ep->ExceptionRecord;  
fprintf(stderr, "CRASH code=0x%%08lX addr=%%p\n", er->ExceptionCode, er->ExceptionAddress);  
CONTEXT* ctx = ep->ContextRecord;  
fprintf(stderr, "RIP=%%p RSP=%%p\n", (void*)ctx->Rip, (void*)ctx->Rsp);  
fflush(stderr);  
return EXCEPTION_EXECUTE_HANDLER;  
}  
static void install_crash_handler(void) { SetUnhandledExceptionFilter(crash_handler); }  
#else  
static void install_crash_handler(void) {}  
#endif 
