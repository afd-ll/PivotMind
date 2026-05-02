/**
 * @file ui.c
 * @brief UI界面模块 - DeepSeek风格终端界面
 */

#include "ui.h"

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
}

void ui_print_prompt(void) {
    printf("\n> ");
}

void ui_print_user_input(const char* input) {
    printf("\n你: %s\n", input);
}

void ui_print_thinking_start(void) {
}

void ui_print_thinking_end(void) {
}

void ui_print_thinking_line(const char* /*category*/, const char* /*content*/) {
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
        int title_len = strlen(title);
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
