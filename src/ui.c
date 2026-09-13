/**
 * @file ui.c
 * @brief UI界面模块 - DeepSeek风格终端界面
 */

#include "ui.h"
#include <stdarg.h>

#ifdef _WIN32
static HANDLE hConsole = NULL;
static WORD originalAttrs = 0;
#endif

void ui_init(void) {
#ifdef _WIN32
    hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
        originalAttrs = csbi.wAttributes;
    }
#endif
}

void ui_clear_screen(void) {
#ifdef _WIN32
    system("cls");
#else
    printf("\033[2J\033[H");
#endif
}

void ui_print_header(void) {
    ui_frame_title(46, "       玄枢 PivotMind - 认知引擎");
}

void ui_print_prompt(void) {
    printf("\n> ");
    fflush(stdout);
}

void ui_print_user_input(const char* input) {
    printf("\n你: %s\n", input);
}

void ui_print_thinking_start(void) {
    printf("  [思考中");
    fflush(stdout);
}

void ui_print_thinking_end(void) {
    printf("]\n");
    fflush(stdout);
}

void ui_print_thinking_line(const char* category, const char* content) {
    if (category && content) {
        printf(".");
        fflush(stdout);
    }
}

void ui_print_ai_response(const char* response) {
    printf("\nAI: %s\n", response);
    fflush(stdout);
}

void ui_print_status(const char* status) {
    printf("  %s\n", status);
}

void ui_print_learning_status(int learned, int total) {
    printf("  学习: %d/%d\n", learned, total);
}

void ui_set_color(const char* color) {
#ifdef _WIN32
    if (hConsole == NULL) ui_init();
    
    WORD attrs = originalAttrs;
    if (strstr(color, COLOR_RED) != NULL) attrs |= FOREGROUND_RED;
    else if (strstr(color, COLOR_GREEN) != NULL) attrs |= FOREGROUND_GREEN;
    else if (strstr(color, COLOR_BLUE) != NULL) attrs |= FOREGROUND_BLUE;
    else if (strstr(color, COLOR_YELLOW) != NULL) attrs |= FOREGROUND_RED | FOREGROUND_GREEN;
    else if (strstr(color, COLOR_CYAN) != NULL) attrs |= FOREGROUND_GREEN | FOREGROUND_BLUE;
    else if (strstr(color, COLOR_MAGENTA) != NULL) attrs |= FOREGROUND_RED | FOREGROUND_BLUE;
    
    if (strstr(color, COLOR_BOLD) != NULL) attrs |= FOREGROUND_INTENSITY;
    
    SetConsoleTextAttribute(hConsole, attrs);
#else
    printf("%s", color);
#endif
}

void ui_reset_color(void) {
#ifdef _WIN32
    if (hConsole == NULL) ui_init();
    SetConsoleTextAttribute(hConsole, originalAttrs);
#else
    printf(COLOR_RESET);
#endif
}

void ui_print_separator(const char* symbol, int length) {
    for (int i = 0; i < length; i++) {
        printf("%s", symbol);
    }
    printf("\n");
}

void ui_box_start(const char* title) {
    if (title) {
        printf("  ┌─ %s ", title);
        int title_len = ui_disp_width(title);   /* 同缺陷：原按字节数算 ⇒ CJK 标题横线少一截 */
        for (int i = 0; i < 50 - title_len - 4; i++) {
            printf("─");
        }
        printf("┐\n");
    } else {
        printf("  ┌─────────────────────────────────────────────────────┐\n");
    }
}

void ui_box_end(void) {
    printf("  └─────────────────────────────────────────────────────┘\n");
}

/* ==================== 显示宽度感知的制表框（v0.5.34） ==================== */

/* 码点显示宽度：东亚宽/全角与常见 emoji = 2；组合符/零宽 = 0；其余 = 1。
 * 与 wcwidth() 的 EAW=W/F 口径一致（框线 U+2500-257F、箭头 U+2190-21FF 等
 * Ambiguous 字符按 1 列，与终端默认行为一致）。 */
static int ui_cp_width(unsigned cp) {
    if (cp == 0u) return 0;
    if ((cp >= 0x0300u && cp <= 0x036Fu) ||   /* 组合附加符 */
        (cp >= 0x200Bu && cp <= 0x200Fu) ||   /* 零宽字符 */
        cp == 0xFEFFu) return 0;
    if ((cp >= 0x1100u && cp <= 0x115Fu) ||   /* Hangul Jamo */
        (cp >= 0x2E80u && cp <= 0xA4CFu) ||   /* CJK 部首 … 彝文 */
        (cp >= 0xAC00u && cp <= 0xD7A3u) ||   /* Hangul 音节 */
        (cp >= 0xF900u && cp <= 0xFAFFu) ||   /* CJK 兼容表意 */
        (cp >= 0xFE30u && cp <= 0xFE6Fu) ||   /* CJK 兼容形式 */
        (cp >= 0xFF00u && cp <= 0xFF60u) ||   /* 全角形式 */
        (cp >= 0xFFE0u && cp <= 0xFFE6u) ||
        (cp >= 0x1F300u && cp <= 0x1FAFFu) || /* emoji */
        (cp >= 0x20000u && cp <= 0x3FFFDu)) return 2;   /* CJK 扩展 B+ */
    return 1;
}

int ui_disp_width(const char* s) {
    int w = 0;
    if (s == NULL) return 0;
    while (*s != '\0') {
        const unsigned char* u = (const unsigned char*)s;
        unsigned char c = u[0];
        unsigned cp;
        int len, i;
        if (c < 0x80u)                 { cp = c;          len = 1; }
        else if ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu;  len = 2; }
        else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu;  len = 3; }
        else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u;  len = 4; }
        else { s++; continue; }               /* 非法首字节：跳过、不计宽 */
        for (i = 1; i < len; i++) {
            if ((u[i] & 0xC0u) != 0x80u) { len = i; break; }
            cp = (cp << 6) | (unsigned)(u[i] & 0x3Fu);
        }
        if (len < 1) len = 1;
        if (cp == 0u) break;
        w += ui_cp_width(cp);
        s += len;
    }
    return w;
}

#define UI_FRAME_DEF_W 44
static FILE* g_frame_out = NULL;   /* NULL ⇒ stdout */
static int   g_frame_w   = 0;

static FILE* ui_frame_fp(void) { return (g_frame_out != NULL) ? g_frame_out : stdout; }

FILE* ui_frame_stream(FILE* out) {
    FILE* old = g_frame_out;
    g_frame_out = out;
    return old;
}

static void ui_frame_rule(const char* l, const char* r) {
    FILE* fp = ui_frame_fp();
    int i;
    fputs(l, fp);
    for (i = 0; i < g_frame_w; i++) fputs("═", fp);
    fputs(r, fp);
    fputc('\n', fp);
    fflush(fp);
}

void ui_frame_begin(int inner_w) {
    g_frame_w = (inner_w > 0) ? inner_w : UI_FRAME_DEF_W;
    ui_frame_rule("╔", "╗");
}

void ui_frame_sep(void) { ui_frame_rule("╠", "╣"); }

void ui_frame_end(void) { ui_frame_rule("╚", "╝"); }

void ui_frame_sep_label(const char* label) {
    FILE* fp = ui_frame_fp();
    int lw   = ui_disp_width(label);
    int rest = g_frame_w - lw - 2;     /* 标签两侧各留一个空格 */
    int left, right, i;
    if (rest < 0) rest = 0;
    left  = rest / 2;
    right = rest - left;
    fputs("╠", fp);
    for (i = 0; i < left; i++) fputs("═", fp);
    if (lw > 0) { fputc(' ', fp); fputs(label, fp); fputc(' ', fp); }
    for (i = 0; i < right; i++) fputs("═", fp);
    fputs("╣", fp);
    fputc('\n', fp);
    fflush(fp);
}

void ui_frame_row(const char* fmt, ...) {
    FILE* fp = ui_frame_fp();
    char buf[2048];
    va_list ap;
    int w, i;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    w = ui_disp_width(buf);
    fputs("║", fp);
    fputs(buf, fp);
    if (w < g_frame_w) {
        for (i = w; i < g_frame_w; i++) fputc(' ', fp);
    } else {
        fputc(' ', fp);                /* 超宽：不截断，留一格再收边 */
    }
    fputs("║", fp);
    fputc('\n', fp);
    fflush(fp);
}

void ui_frame_title(int inner_w, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    int w;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    w = ui_disp_width(buf);
    if (inner_w <= 0) {
        inner_w = w + 4;
        if (inner_w < UI_FRAME_DEF_W) inner_w = UI_FRAME_DEF_W;
    }
    ui_frame_begin(inner_w);
    ui_frame_row("%s", buf);
    ui_frame_end();
}

void ui_print_progress_bar(int current, int total, int width) {
    float percent = (float)current / total;
    int filled = (int)(percent * width);
    
    ui_set_color(COLOR_GREEN);
    printf("  │ [");
    for (int i = 0; i < width; i++) {
        if (i < filled) printf("█");
        else printf("░");
    }
    printf("] %.1f%%", percent * 100);
    ui_reset_color();
    printf("\n");
}
