#ifndef UI_H
#define UI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifdef _WIN32
#include <windows.h>
#endif
#endif

#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"

#define COLOR_BLACK   "\033[30m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"

#define COLOR_USER    "\033[36m"
#define COLOR_AI      "\033[32m"
#define COLOR_THINK   "\033[33m"
#define COLOR_ERROR   "\033[31m"
#define COLOR_INFO    "\033[36m"
#define COLOR_DEBUG   "\033[90m"

typedef struct {
    int enabled;
    time_t start_time;
    int token_count;
    int concept_count;
    int learning_count;
    float cpu_usage;
} UIStatus;

void ui_init(void);
void ui_clear_screen(void);
void ui_print_header(void);
void ui_print_prompt(void);
void ui_print_user_input(const char* input);
void ui_print_thinking_start(void);
void ui_print_thinking_end(void);
void ui_print_thinking_line(const char* category, const char* content);
void ui_print_ai_response(const char* response);
void ui_print_status(const char* status);
void ui_print_learning_status(int learned, int total);
void ui_set_color(const char* color);
void ui_reset_color(void);
void ui_print_separator(const char* symbol, int length);

void ui_box_start(const char* title);
void ui_box_end(void);
void ui_print_progress_bar(int current, int total, int width);

#endif
