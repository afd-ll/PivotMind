# 001: strcpy/strcat 安全替换 + volatile 修复 + UTF-8 统一

> 日期: 2026-05-25 | 关联审查: 002A-code-review.md

## 修改概览

| 文件 | 操作 | 说明 |
|------|------|------|
| src/dialog_system.c | 修改 | 17处 strcat → strncat（4096字节边界） |
| src/chinese.c | 修改 | 2处 strcpy → strncpy（len+1 边界） |
| src/vocab.c | 修改 | 3处 strcpy → strncpy（max_len/sizeof 边界） |
| src/causal_reasoning.c | 修改 | 1处 strcpy → strncpy（sizeof(buffer) 边界） |
| src/thread_pool.c | 修改 | volatile int → atomic_int（running/shutdown） |
| src/*.c (36个) | 修改 | 编码统一为 UTF-8 |
| tests/test_runner.c | 修改 | prev_failed → g_prev_failed 静态变量 |
| Makefile | 修改 | -D_USE_MATH_DEFINES 统一移至 CFLAGS |

## 详细修改

### 1. strcpy → strncpy（6处）
- `chinese.c`: `traditional_to_simplified` / `simplified_to_traditional`，result 缓冲区大小 `len+1`
- `vocab.c`: `vocab_decode`（output 由 max_len 约束），qbuf/abuf（4096 字节）
- `causal_reasoning.c`: buffer[512]

### 2. strcat → strncat（17处）
- `dialog_system.c`: response/full_response（malloc 4096），path_desc[512]，chain_desc[256]，rule_info[512]

### 3. thread_pool.c atomic 修复
- `volatile int running` → `atomic_int running`
- `volatile int shutdown` → `atomic_int shutdown`

### 4. 编码统一
- src/ 下全部 54 个 .c 文件统一为 UTF-8 编码

### 5. Makefile 整理
- `-D_USE_MATH_DEFINES` 从 7 个二进制链接行移除，统一加入 CFLAGS

### 6. test_runner.c
- `prev_failed` 从 main() 局部变量提升为文件级 `g_prev_failed` 静态变量
- RUN_TEST 宏不再依赖外部作用域
