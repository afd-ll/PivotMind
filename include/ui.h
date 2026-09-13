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

/* ---- 显示宽度感知的制表框（v0.5.34）--------------------------------------
 * 背景：全仓的框原先都按 strlen（字节数）补空格 —— CJK 宽字符占 2 列却只算 1，
 * 于是右边框一律错位（实测 42 条框内容行里 31 条错位，横跨 14 个文件），
 * 而且标题里带版本号 ⇒ 手改空格必然复发。这里统一按【终端显示列宽】计算：
 * 东亚宽/全角 = 2 列，组合符与零宽 = 0 列，其余 = 1 列。
 * ---------------------------------------------------------------------- */

/** UTF-8 字符串的显示列宽（非法字节跳过、不计数）。 */
int ui_disp_width(const char* s);

/** 框输出流（默认 stdout）。返回【之前的流】，便于成对恢复；传 NULL = 恢复到 stdout。 */
FILE* ui_frame_stream(FILE* out);

/** 开框：╔ + inner_w 个 ═ + ╗。inner_w = 内容区列数（不含左右边框）。 */
void ui_frame_begin(int inner_w);

/** 框内一行（printf 语义）：按显示宽度右补空格到 inner_w；超宽不截断、原样输出。 */
void ui_frame_row(const char* fmt, ...);

/** 分隔行 ╠═══╣ / 带居中标签的 ╠══ 标签 ══╣。 */
void ui_frame_sep(void);
void ui_frame_sep_label(const char* label);

/** 闭框：╚ + inner_w 个 ═ + ╝。 */
void ui_frame_end(void);

/** 一行式标题框：begin(inner_w) + row(fmt,…) + end；inner_w <= 0 ⇒ 自动（内容宽+4，下限 44）。 */
void ui_frame_title(int inner_w, const char* fmt, ...);

#endif
