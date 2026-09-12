/**
 * @file test_paths_unit.c
 * @brief pivotmind_paths 契约单测（10 条）—— 逐条直接断言 include/pivotmind_paths.h 的定稿契约。
 *
 * 设计要点（为什么这么写，别改成别的样子）：
 *  ① pm_home() 是 pthread_once「解析一次并缓存」⇒ **同一进程里改环境变量不再生效**。
 *     故凡「换个环境再问一次」的用例（1/2/3/10）一律 fork 子进程：在子进程里设好环境再调用，
 *     结果（home 字符串 + ensure 返回值）与 stderr（WARN 捕获）经管道带回父进程判定。
 *  ② 子进程用例必须在**父进程第一次调用 pm_home() 之前**跑完（fork 会复制已解析的缓存）。
 *  ③ 不写「只建一次」这类断言 —— 契约是「最终就绪」，不是「只调用一次 mkdir」。
 */

#include "pivotmind_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define T_START(name) do { printf("Running: %s...", name); fflush(stdout); tests_run++; } while (0)
#define T_END() do { tests_passed++; printf(" PASSED\n"); } while (0)
#define T_FAIL(...) do { tests_failed++; printf(" FAILED: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { T_FAIL(__VA_ARGS__); return; } } while (0)

/* ============================ 子进程环境探针 ============================ */

typedef struct {
    char     home[PM_PATH_MAX];   /* 子进程 pm_home() 的结果 */
    int      ensure_rc;           /* pm_ensure_dirs 的返回值；-1 = 未调用 */
    unsigned ensure_mask;
    char     log[8192];           /* 子进程 stderr 全文（WARN 捕获） */
    size_t   log_len;
    int      status;              /* waitpid 的 status */
} child_result;

/* 在子进程里：设环境 → pm_home()（可选 pm_ensure_dirs()）→ 回传结果 + stderr 文本。
   返回 0 = 父子通信正常（子进程退出码另行在 r->status 里判）。 */
static int child_run(const char *pvm_home, int pvm_set,
                     const char *home, int home_set,
                     unsigned ensure_mask, child_result *r)
{
    int dp[2], ep[2];
    pid_t pid;
    ssize_t n;
    size_t total;
    char buf[PM_PATH_MAX + 64];
    char *nl;

    memset(r, 0, sizeof *r);
    r->ensure_rc = -1;
    if (pipe(dp) != 0 || pipe(ep) != 0) return -1;

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        const char *h;
        unsigned rc = 0u;
        int has_rc = 0;
        char line[PM_PATH_MAX + 64];
        int len;

        (void)close(dp[0]);
        (void)close(ep[0]);
        if (dup2(ep[1], STDERR_FILENO) < 0) _exit(97);
        (void)close(ep[1]);

        if (pvm_set) (void)setenv("PIVOTMIND_HOME", pvm_home, 1);
        else         (void)unsetenv("PIVOTMIND_HOME");
        if (home_set) (void)setenv("HOME", home, 1);
        else          (void)unsetenv("HOME");

        h = pm_home();                                  /* 契约：永不为 NULL */
        if (h != NULL && ensure_mask != 0u) {
            rc = pm_ensure_dirs(ensure_mask);
            has_rc = 1;
        }
        len = snprintf(line, sizeof line, "%s\n%d %u\n", (h != NULL) ? h : "(NULL)", has_rc, rc);
        if (len > 0 && write(dp[1], line, (size_t)len) != len) _exit(98);
        (void)close(dp[1]);
        fflush(NULL);
        _exit(0);
    }

    (void)close(dp[1]);
    (void)close(ep[1]);

    total = 0;
    while (total < sizeof buf - 1u && (n = read(dp[0], buf + total, sizeof buf - 1u - total)) > 0) {
        total += (size_t)n;
    }
    buf[total] = '\0';
    (void)close(dp[0]);

    total = 0;
    while (total < sizeof r->log - 1u && (n = read(ep[0], r->log + total, sizeof r->log - 1u - total)) > 0) {
        total += (size_t)n;
    }
    r->log[total] = '\0';
    r->log_len = total;
    (void)close(ep[0]);

    (void)waitpid(pid, &r->status, 0);

    nl = strchr(buf, '\n');
    if (nl == NULL) return -1;
    *nl = '\0';
    snprintf(r->home, sizeof r->home, "%s", buf);
    {
        int has_rc = 0;
        unsigned mask = 0u;
        if (sscanf(nl + 1, "%d %u", &has_rc, &mask) != 2) return -1;
        r->ensure_rc = has_rc ? (int)mask : -1;
        r->ensure_mask = mask;
    }
    return 0;
}

static int child_exited_clean(const child_result *r) {
    return WIFEXITED(r->status) && WEXITSTATUS(r->status) == 0;
}

/* 父进程侧临时目录（每个子进程用例一个，互不干扰） */
static int make_home(char *dst, size_t n, const char *tag) {
    snprintf(dst, n, "/tmp/pm_paths_%s_XXXXXX", tag);
    return (mkdtemp(dst) != NULL) ? 0 : -1;
}

/* ============================ 用例 1 ============================ */

static void t1_pivotmind_home_unset_uses_home_level2(void) {
    char th[256];
    char want[PM_PATH_MAX];
    child_result r;

    T_START("1. $PIVOTMIND_HOME 未设 ⇒ 走第 2 级 $HOME/pivotmind");
    CHECK(make_home(th, sizeof th, "t1") == 0, "mkdtemp 失败: %s", strerror(errno));
    CHECK(child_run(NULL, 0, th, 1, 0u, &r) == 0, "子进程通信失败");
    CHECK(child_exited_clean(&r), "子进程异常退出 status=0x%x", r.status);
    snprintf(want, sizeof want, "%s/pivotmind", th);
    CHECK(strcmp(r.home, want) == 0, "期望 \"%s\"，实际 \"%s\"", want, r.home);
    CHECK(r.log_len == 0u, "纯查询用例不应有任何 WARN，实际 stderr %zu 字节: %s", r.log_len, r.log);
    (void)rmdir(th);
    T_END();
}

/* ============================ 用例 2 ============================ */

static void t2_invalid_pivotmind_home_falls_to_level2(void) {
    /* 空串 / 全空白 / 相对路径 / ~ / / —— 各自都必须「视为未设置 + 一条 WARN」 */
    static const char *const bad[] = { "", "   ", "\t \t", "foo", "~", "/" };
    const size_t NB = sizeof bad / sizeof bad[0];
    size_t i;

    T_START("2. $PIVOTMIND_HOME 非法（空串/全空白/相对/~//）⇒ 各自视为未设置 + WARN ⇒ 走第 2 级");
    for (i = 0; i < NB; i++) {
        char th[256];
        char want[PM_PATH_MAX];
        child_result r;

        CHECK(make_home(th, sizeof th, "t2") == 0, "[%zu] mkdtemp 失败", i);
        CHECK(child_run(bad[i], 1, th, 1, 0u, &r) == 0, "[%zu] 子进程通信失败", i);
        CHECK(child_exited_clean(&r), "[%zu] 子进程异常退出 status=0x%x", i, r.status);
        snprintf(want, sizeof want, "%s/pivotmind", th);
        CHECK(strcmp(r.home, want) == 0, "[%zu] 非法值 \"%s\"：期望第 2 级 \"%s\"，实际 \"%s\"",
              i, bad[i], want, r.home);
        CHECK(strstr(r.log, "WARN") != NULL, "[%zu] 非法值 \"%s\" 未产生 WARN（stderr %zu 字节：%s）",
              i, bad[i], r.log_len, r.log);
        CHECK(strstr(r.log, "PIVOTMIND_HOME") != NULL, "[%zu] WARN 未指明 PIVOTMIND_HOME（stderr：%s）",
              i, r.log);
        (void)rmdir(th);
    }
    T_END();
}

/* ============================ 用例 3 ============================ */

static void t3_home_missing_uses_pm_home_default(void) {
    T_START("3. $HOME 缺失 ⇒ 第 3 级 PM_HOME_DEFAULT；$HOME 存在 ⇒ 第 2 级且第 3 级不触发");
#ifdef PM_HOME_DEFAULT
    {
        char th[256];
        char want2[PM_PATH_MAX];
        child_result r;

        CHECK(make_home(th, sizeof th, "t3") == 0, "mkdtemp 失败");
        snprintf(want2, sizeof want2, "%s/pivotmind", th);
        CHECK(strcmp(PM_HOME_DEFAULT, want2) != 0,
              "前置条件不成立：PM_HOME_DEFAULT(\"%s\") 与第 2 级(\"%s\") 语义相同，无法区分",
              PM_HOME_DEFAULT, want2);

        /* (a) $HOME 缺失 ⇒ 第 3 级 */
        CHECK(child_run(NULL, 0, NULL, 0, 0u, &r) == 0, "子进程通信失败");
        CHECK(child_exited_clean(&r), "子进程异常退出 status=0x%x", r.status);
        CHECK(strcmp(r.home, PM_HOME_DEFAULT) == 0,
              "$HOME 缺失时应走第 3 级 \"%s\"，实际 \"%s\"", PM_HOME_DEFAULT, r.home);

        /* (b) $HOME 存在 ⇒ 第 2 级，且第 3 级不触发 */
        CHECK(child_run(NULL, 0, th, 1, 0u, &r) == 0, "子进程通信失败");
        CHECK(child_exited_clean(&r), "子进程异常退出 status=0x%x", r.status);
        CHECK(strcmp(r.home, want2) == 0, "$HOME 存在时应走第 2 级 \"%s\"，实际 \"%s\"", want2, r.home);
        CHECK(strcmp(r.home, PM_HOME_DEFAULT) != 0,
              "第 3 级被触发（拿到了 PM_HOME_DEFAULT=\"%s\"，第 2 级应命中）", r.home);
        (void)rmdir(th);
    }
    T_END();
#else
    (void)t3_home_missing_uses_pm_home_default;
    T_FAIL("本编译单元没有 PM_HOME_DEFAULT（Makefile 未挂接 -DPM_HOME_DEFAULT）⇒ 第 3 级无法证");
#endif
}

/* ============================ 用例 10（子进程族，必须先于父进程首次 pm_home 调用）===== */

static void t10_sandbox_end_to_end(void) {
    static const char *const sub[6] = { NULL, "data", "log", "session", "run", "corpus" };
    char th[256];
    char want[PM_PATH_MAX];
    child_result r;
    int i;

    T_START("10. 沙箱端到端：HOME=<临时目录> + PIVOTMIND_HOME 未设 ⇒ ensure(ALL)==0 且目录树真实存在");
    CHECK(make_home(th, sizeof th, "t10") == 0, "mkdtemp 失败");
    CHECK(child_run(NULL, 0, th, 1, PM_DIR_ALL, &r) == 0, "子进程通信失败");
    CHECK(child_exited_clean(&r), "子进程异常退出 status=0x%x", r.status);
    snprintf(want, sizeof want, "%s/pivotmind", th);
    CHECK(strcmp(r.home, want) == 0, "home 应为 \"%s\"，实际 \"%s\"", want, r.home);
    CHECK(r.ensure_rc == 0, "pm_ensure_dirs(PM_DIR_ALL) 应返回 0，实际 %d (0x%x)",
          r.ensure_rc, (unsigned)r.ensure_rc);

    /* 父进程独立核实目录树真的存在（不信子进程的一面之词） */
    for (i = 0; i < 6; i++) {
        char p[PM_PATH_MAX];
        struct stat st;
        if (i == 0) snprintf(p, sizeof p, "%s", want);
        else        snprintf(p, sizeof p, "%s/%s", want, sub[i]);
        CHECK(lstat(p, &st) == 0, "%s 不存在（目录树未真正建立）", p);
        CHECK(S_ISDIR(st.st_mode), "%s 不是目录", p);
        CHECK((st.st_mode & 07777u) == 0700u, "%s mode=0%03o，期望 0700", p,
              (unsigned)(st.st_mode & 07777u));
    }

    /* 自底向上清理 */
    for (i = 5; i >= 1; i--) {
        char p[PM_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", want, sub[i]);
        (void)rmdir(p);
    }
    (void)rmdir(want);
    (void)rmdir(th);
    T_END();
}

/* ============================ 父进程沙箱（用例 4-9 共用） ============================ */

static char g_tmproot[256];
static char g_want_home[PM_PATH_MAX];

/* ============================ 用例 4 ============================ */

static void t4_ensure_creates_0700(void) {
    unsigned rc;
    int i;

    T_START("4. pm_ensure_dirs(PM_DIR_ALL) 空目录世界 ⇒ 0，且每个新建目录 mode==0700");
    rc = pm_ensure_dirs(PM_DIR_ALL);
    CHECK(rc == 0u, "返回值应为 0（全部就绪），实际 0x%x", rc);
    CHECK(strcmp(pm_home(), g_want_home) == 0, "home 应为 \"%s\"，实际 \"%s\"", g_want_home, pm_home());
    for (i = 0; i < 6; i++) {
        unsigned bit = 1u << i;
        const char *d = pm_dir(bit);
        struct stat st;
        CHECK(d != NULL && d[0] != '\0', "pm_dir(0x%x) 为空", bit);
        CHECK(lstat(d, &st) == 0, "lstat(%s) 失败: %s", d, strerror(errno));
        CHECK(S_ISDIR(st.st_mode), "%s 不是目录", d);
        CHECK((st.st_mode & 07777u) == 0700u, "%s mode=0%03o，期望 0700", d,
              (unsigned)(st.st_mode & 07777u));
    }
    T_END();
}

/* ============================ 用例 5 ============================ */

static void t5_existing_dir_mode_untouched(void) {
    const char *d;
    struct stat before, after;
    unsigned rc;

    T_START("5. 已存在目录（含 chmod 0555）⇒ 返回位被置起，且 stat 前后 mode 不变（不 chmod）");
    d = pm_dir(PM_DIR_LOG);
    CHECK(d != NULL, "pm_dir(PM_DIR_LOG) 为 NULL");
    CHECK(chmod(d, 0555) == 0, "chmod(%s, 0555) 失败: %s", d, strerror(errno));
    CHECK(lstat(d, &before) == 0, "lstat 前置失败");

    rc = pm_ensure_dirs(PM_DIR_LOG);

    CHECK(lstat(d, &after) == 0, "lstat 后置失败");
    CHECK(before.st_mode == after.st_mode,
          "mode 被改了：0%o → 0%o（契约：已存在目录绝不 chmod）",
          (unsigned)before.st_mode, (unsigned)after.st_mode);
    CHECK((after.st_mode & 07777u) == 0555u, "mode 应为 0555，实际 0%03o",
          (unsigned)(after.st_mode & 07777u));
    if (geteuid() != 0) {
        CHECK(rc == PM_DIR_LOG, "只读(0555)目录应返回未就绪位 0x%x，实际 0x%x", PM_DIR_LOG, rc);
    } else {
        printf(" [以 root 运行：faccessat(W_OK) 恒真，「不可写⇒位置起」这一支无法证，如实标注] ");
    }
    CHECK(chmod(d, 0700) == 0, "恢复 0700 失败");
    T_END();
}

/* ============================ 用例 6 ============================ */

static void t6_path_is_file_not_dir(void) {
    const char *d;
    struct stat st;
    unsigned rc;
    FILE *f;

    T_START("6. 路径是个【文件】而非目录 ⇒ 对应位置起、不崩");
    d = pm_dir(PM_DIR_RUN);
    CHECK(d != NULL, "pm_dir(PM_DIR_RUN) 为 NULL");
    CHECK(rmdir(d) == 0, "rmdir(%s) 失败（前置）: %s", d, strerror(errno));
    f = fopen(d, "w");
    CHECK(f != NULL, "无法在 %s 位置建文件: %s", d, strerror(errno));
    (void)fputs("not a dir\n", f);
    (void)fclose(f);
    CHECK(lstat(d, &st) == 0 && !S_ISDIR(st.st_mode), "前置：%s 应为普通文件", d);

    rc = pm_ensure_dirs(PM_DIR_RUN);

    CHECK(rc == PM_DIR_RUN, "文件占位应返回未就绪位 0x%x，实际 0x%x", PM_DIR_RUN, rc);
    CHECK(lstat(d, &st) == 0 && S_ISREG(st.st_mode), "契约：不删除、不覆盖既有文件（%s 不见了）", d);
    CHECK(remove(d) == 0, "清理 %s 失败", d);
    T_END();
}

/* ============================ 用例 7 ============================ */

typedef struct {
    const char *home;
    unsigned    rc;
    int         iters;
    int         bad;
} th_arg;

static void *th_body(void *p) {
    th_arg *a = (th_arg *)p;
    int i;
    for (i = 0; i < a->iters; i++) {
        const char *h = pm_home();
        if (h == NULL || h[0] == '\0') { a->bad = 1; return NULL; }
        a->home = h;
        a->rc = pm_ensure_dirs(PM_DIR_ALL);
    }
    return NULL;
}

static void t7_concurrent_ensure(void) {
    enum { N = 8 };
    pthread_t tid[N];
    th_arg arg[N];
    int i;

    T_START("7. 并发：8 线程 pm_home()+pm_ensure_dirs(ALL) ⇒ 最终目录存在、无崩溃（TSan 干净）");
    for (i = 0; i < N; i++) {
        arg[i].home = NULL;
        arg[i].rc = 0xffffffffu;
        arg[i].iters = 200;
        arg[i].bad = 0;
    }
    for (i = 0; i < N; i++) CHECK(pthread_create(&tid[i], NULL, th_body, &arg[i]) == 0,
                                  "pthread_create(%d) 失败", i);
    for (i = 0; i < N; i++) CHECK(pthread_join(tid[i], NULL) == 0, "pthread_join(%d) 失败", i);
    for (i = 0; i < N; i++) {
        CHECK(arg[i].bad == 0, "线程 %d 拿到 NULL/空 home", i);
        CHECK(arg[i].home == pm_home(), "线程 %d 拿到的 home 指针与 pm_home() 不一致", i);
        CHECK(arg[i].rc == 0u, "线程 %d pm_ensure_dirs(ALL)=0x%x（期望 0）", i, arg[i].rc);
    }
    /* 最终态断言：目录真实存在（契约是「最终就绪」，不是「只建一次」） */
    for (i = 0; i < 6; i++) {
        unsigned bit = 1u << i;
        const char *d = pm_dir(bit);
        struct stat st;
        CHECK(d != NULL && lstat(d, &st) == 0 && S_ISDIR(st.st_mode),
              "最终态：%s 不存在或不是目录", (d != NULL) ? d : "(NULL)");
    }
    T_END();
}

/* ============================ 用例 8 ============================ */

static void t8_pm_path_bounds(void) {
    char buf[PM_PATH_MAX];
    const char *data = pm_dir(PM_DIR_DATA);
    char want[PM_PATH_MAX];
    size_t L;
    int n;

    T_START("8. pm_path 边界：NULL/\"\"→目录本身；\"..\"/\"/abs\"→-1；\"a/b\" 允许；n 太小→-1 且 buffer 未坏");
    CHECK(data != NULL && data[0] == '/', "pm_dir(PM_DIR_DATA) 非法: %s", data ? data : "(NULL)");

    /* name == NULL ⇒ 目录本身（不带尾斜杠），返回长度不含 '\0' */
    n = pm_path(buf, sizeof buf, PM_DIR_DATA, NULL);
    CHECK(n == (int)strlen(data), "NULL: 期望长度 %zu，实际 %d", strlen(data), n);
    CHECK(strcmp(buf, data) == 0, "NULL: 期望 \"%s\"，实际 \"%s\"", data, buf);
    CHECK(buf[strlen(data)] == '\0', "NULL: 未正确终止");

    /* name == "" ⇒ 目录本身 */
    n = pm_path(buf, sizeof buf, PM_DIR_DATA, "");
    CHECK(n == (int)strlen(data) && strcmp(buf, data) == 0, "\"\": 期望目录本身，实际 \"%s\"(n=%d)", buf, n);

    /* 越权/穿越：一律 -1 */
    n = pm_path(buf, sizeof buf, PM_DIR_DATA, "..");
    CHECK(n == -1, "\"..\" 应 -1，实际 %d（\"%s\"）", n, buf);
    n = pm_path(buf, sizeof buf, PM_DIR_DATA, "../x");
    CHECK(n == -1, "\"../x\" 应 -1，实际 %d", n);
    n = pm_path(buf, sizeof buf, PM_DIR_DATA, "a/../b");
    CHECK(n == -1, "\"a/../b\" 应 -1，实际 %d", n);
    n = pm_path(buf, sizeof buf, PM_DIR_DATA, ".../x");
    CHECK(n == 0 + (int)(strlen(data) + 1u + strlen(".../x")), "\".../x\" 不含 .. 段，应允许，实际 %d", n);
    n = pm_path(buf, sizeof buf, PM_DIR_DATA, "/abs");
    CHECK(n == -1, "\"/abs\" 应 -1，实际 %d", n);

    /* 相对子路径：允许，结果以 /a/b 结尾 */
    snprintf(want, sizeof want, "%s/a/b", data);
    n = pm_path(buf, sizeof buf, PM_DIR_DATA, "a/b");
    CHECK(n == (int)strlen(want), "\"a/b\": 期望长度 %zu，实际 %d", strlen(want), n);
    CHECK(strcmp(buf, want) == 0, "\"a/b\": 期望 \"%s\"，实际 \"%s\"", want, buf);
    CHECK(strlen(buf) >= 4u && strcmp(buf + strlen(buf) - 4u, "/a/b") == 0,
          "\"a/b\": 结果应以 /a/b 结尾，实际 \"%s\"", buf);

    /* 零分配接口的容量边界：恰够 / 差一 / n=1，前后 canary 比对 */
    L = strlen(want);
    memset(buf, 0x5a, sizeof buf);
    n = pm_path(buf, L + 1u, PM_DIR_DATA, "a/b");
    CHECK(n == (int)L && strcmp(buf, want) == 0, "恰好容量(=%zu)应成功，实际 n=%d buf=\"%s\"", L + 1u, n, buf);

    memset(buf, 0x5a, sizeof buf);
    n = pm_path(buf, L, PM_DIR_DATA, "a/b");
    CHECK(n == -1, "容量差一应 -1，实际 %d", n);
    CHECK(buf[L] == 0x5a && buf[L + 1u] == 0x5a && buf[L + 2u] == 0x5a,
          "容量不足时越界写了 buffer[n..]（canary 被改：0x%02x 0x%02x 0x%02x）",
          (unsigned char)buf[L], (unsigned char)buf[L + 1u], (unsigned char)buf[L + 2u]);

    memset(buf, 0x5a, sizeof buf);
    n = pm_path(buf, 1u, PM_DIR_DATA, "a/b");
    CHECK(n == -1, "n=1 应 -1，实际 %d", n);
    CHECK(buf[1] == 0x5a && buf[2] == 0x5a, "n=1 越界写（canary 被改）");

    memset(buf, 0x5a, sizeof buf);
    n = pm_path(buf, 3u, PM_DIR_DATA, NULL);
    CHECK(n == -1, "目录本身 + n=3 应 -1，实际 %d", n);
    CHECK(buf[3] == 0x5a && buf[4] == 0x5a, "目录本身容量不足时越界写（canary 被改）");
    T_END();
}

/* ============================ 用例 9 ============================ */

static void t9_pm_dir_invalid_which(void) {
    T_START("9. pm_dir(非法 which) ⇒ NULL（绝不悄悄返回 home）");
    CHECK(pm_dir(0u) == NULL, "pm_dir(0) 应为 NULL");
    CHECK(pm_dir(1u << 6) == NULL, "pm_dir(1u<<6) 应为 NULL（越界位）");
    CHECK(pm_dir(1u << 31) == NULL, "pm_dir(1u<<31) 应为 NULL（越界位）");
    CHECK(pm_dir(PM_DIR_DATA | PM_DIR_LOG) == NULL, "pm_dir(多 bit) 应为 NULL");
    CHECK(pm_dir(0xffffffffu) == NULL, "pm_dir(全 1) 应为 NULL");
    CHECK(pm_dir(PM_DIR_DATA) != NULL && pm_dir(PM_DIR_DATA)[0] == '/', "阴性对照失败：合法位应给路径");
    T_END();
}

/* ============================ main ============================ */

int main(void) {
    printf("\n=== PivotMind 路径 SSOT（pivotmind_paths）契约单测 ===\n\n");
#ifdef PM_HOME_DEFAULT
    printf("编译期缺省 PM_HOME_DEFAULT = %s\n\n", PM_HOME_DEFAULT);
#else
    printf("编译期缺省 PM_HOME_DEFAULT = (未定义！)\n\n");
#endif

    /* ⚠️ 以下 4 条必须在父进程第一次 pm_home() 之前跑（fork 复制已解析的 once 缓存） */
    t1_pivotmind_home_unset_uses_home_level2();
    t2_invalid_pivotmind_home_falls_to_level2();
    t3_home_missing_uses_pm_home_default();
    t10_sandbox_end_to_end();

    /* 父进程沙箱：HOME=<临时目录>，$PIVOTMIND_HOME 未设 ⇒ 第 2 级 */
    snprintf(g_tmproot, sizeof g_tmproot, "/tmp/pm_paths_test_XXXXXX");
    if (mkdtemp(g_tmproot) == NULL) {
        printf("FATAL: mkdtemp(%s) 失败: %s\n", g_tmproot, strerror(errno));
        return 2;
    }
    if (setenv("HOME", g_tmproot, 1) != 0) { printf("FATAL: setenv(HOME) 失败\n"); return 2; }
    (void)unsetenv("PIVOTMIND_HOME");
    snprintf(g_want_home, sizeof g_want_home, "%s/pivotmind", g_tmproot);

    t4_ensure_creates_0700();
    t5_existing_dir_mode_untouched();
    t6_path_is_file_not_dir();
    t7_concurrent_ensure();
    t8_pm_path_bounds();
    t9_pm_dir_invalid_which();

    /* 清理沙箱（自底向上，全部为空目录） */
    {
        static const char *const sub[6] = { NULL, "data", "log", "session", "run", "corpus" };
        int i;
        char p[PM_PATH_MAX];
        for (i = 5; i >= 1; i--) {
            snprintf(p, sizeof p, "%s/%s", g_want_home, sub[i]);
            (void)rmdir(p);
        }
        (void)rmdir(g_want_home);
        (void)rmdir(g_tmproot);
    }

    printf("\n=== Results: %d run, %d passed, %d failed ===\n", tests_run, tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
