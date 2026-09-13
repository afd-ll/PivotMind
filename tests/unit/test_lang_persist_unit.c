/**
 * @file test_lang_persist_unit.c
 * @brief 节点语种标签落盘契约单测（第 29 支）—— v0.5.36「lang 落盘 / fmt_ver 9→10」
 *
 * 被测契约：
 *   ① 状态文件每节点记录尾部，在 dist_sig_count 之后追加【1 字节 lang】
 *      （取值 PmLang 0..5；见 src/multi_topology.c 的两处写端）。
 *   ② 该字节只是「物化缓存」，权威永远是 pm_lang_of(concept)：加载期无条件按
 *      SSOT 重算定值，落盘值仅用于比对计数（不符则纠偏 + 收尾 WARN 记账）。
 *   ③ fmt_ver<10 的旧文件（v2..v9）没有这 1 字节 ⇒ 加载后由 concept 现算，
 *      前向兼容、零迁移。
 *   ④ 两条写路径（流式批 master_serialize_batch / 锁内 master_save_state_locked）
 *      必须同布局 —— 否则同一份状态会因 PIVOTMIND_SAVE 开关写出两种文件。
 *
 * ─────────── 为什么不能用「存盘 → 读回 → 断言 lang 正确」了事 ───────────
 * 加载端是 `huarong_net_add_node(net, concept, ...)` 重建节点，而
 * create_reasoning_node() 会按 SSOT 给 lang 定标 —— 也就是说，即使落盘那段字节
 * 完全没被写、甚至被当成垃圾跳过，读回来的 lang 也照样「正确」。
 * **纯往返断言证明不了持久化存在。** 故本单测用三个能真正变红的探针：
 *   · 文件字节探针（T1/T5）：把 dist_sig_count 写成唯一哨兵，在文件里定位该节点
 *     记录尾部，直接看紧随其后的那 1 字节 —— 布局与取值都在【文件字节】上验，
 *     不经过内存对象。
 *   · 故意写脏（T3）：把某节点的 lang 字节改成错值，要求加载后 ① 内存值仍是 SSOT、
 *     ② stderr 出现「lang 一致性: 1 个节点」——这句话只有在「字节确实被读到且不等」
 *     时才会打印，因而能证「读端真的在读」。
 *   · 抽掉字节（T4）：按哨兵位置删掉每节点的 lang 字节、把 fmt_ver 改回 9，要求
 *     加载后仍全部对齐、语言值仍正确 —— 证 fmt_ver<10 分支（旧文件零迁移）。
 */

#include "multi_topology.h"
#include "huarong_topology.h"
#include "lang.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define T_START(name) do { printf("Running: %s...", name); fflush(stdout); tests_run++; } while (0)
#define T_END()  do { tests_passed++; printf(" PASSED\n"); } while (0)
#define T_FAIL(...) do { tests_failed++; printf(" FAILED: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { T_FAIL(__VA_ARGS__); return; } } while (0)

/* ── 落盘格式版本的【契约值】。宏 STATE_FORMAT_VERSION 在 src/multi_topology.c 里
 *    （非头文件），此处固化一份：改版本就必须同步改本单测（= 有意识的变更）。 */
#define EXPECT_FMT_VER 10

/* ── 哨兵：把 dist_sig_count 写成独一无二的值，作为「在文件里定位该节点记录尾部」
 *    的锚点。lang 字节恒紧跟在 dist_sig_count 之后（两处写端都是这个次序）。
 *    八节点八哨兵互不混淆；这些值远离任何正常字段（node_id / 长度 / 计数都是小
 *    整数），也不会与 0 填充的 features / dist_sig 撞。 */
#define SENT_BASE 0x7A5A0000

/* ─────────────── 夹具：覆盖 PmLang 全部 6 个取值 ───────────────
 * 概念串一律用 UTF-8 字节转义写死（不依赖源码编码、不受 EOL 转换影响）。 */
typedef struct {
    const char* concept;   /* 写入节点的概念（UTF-8 字节转义） */
    const char* human;     /* 报错用可读名 */
    PmLang      want;      /* 期望语种 = pm_lang_of(concept) */
} fix_t;

static const fix_t FIX[] = {
    { "\xE4\xBD\xA0\xE5\xA5\xBD",                                     "你好",       PM_LANG_ZH      },
    { "\xE4\xBA\xBA\xE5\xB7\xA5\xE6\x99\xBA\xE8\x83\xBD",             "人工智能",   PM_LANG_ZH      },
    { "hello",                                                        "hello",      PM_LANG_EN      },
    { "AI",                                                           "AI",         PM_LANG_EN      },
    { "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF", "こんにちは", PM_LANG_JA      },
    { "\xEA\xB0\x80",                                                 "가",         PM_LANG_KO      },
    { "\xD0\x96",                                                     "Ж",          PM_LANG_OTHER   },
    { "3.14",                                                         "3.14",       PM_LANG_UNKNOWN }
};
#define NFIX ((int)(sizeof FIX / sizeof FIX[0]))

/* ══════════════════════════ 小工具 ══════════════════════════ */

static int host_is_le(void) {
    unsigned x = 1u;
    return *(const uint8_t*)&x == 1;
}

/* 读整个文件到 malloc 缓冲（恒 NUL 终止；失败返回 NULL）。 */
static uint8_t* slurp(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    long n;
    uint8_t* b;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    b = (uint8_t*)malloc((size_t)n + 1u);
    if (!b) { fclose(f); return NULL; }
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    b[n] = 0;
    *out_len = (size_t)n;
    return b;
}

static int write_all(const char* path, const uint8_t* b, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    if (n > 0 && fwrite(b, 1, n, f) != n) { fclose(f); return -1; }
    if (fclose(f) != 0) return -1;
    return 0;
}

static void mk_pat(int sent, uint8_t pat[4]) {
    pat[0] = (uint8_t)((unsigned)sent & 0xFFu);
    pat[1] = (uint8_t)(((unsigned)sent >> 8) & 0xFFu);
    pat[2] = (uint8_t)(((unsigned)sent >> 16) & 0xFFu);
    pat[3] = (uint8_t)(((unsigned)sent >> 24) & 0xFFu);
}

/* 首个命中偏移；-1 = 未找到。 */
static long find_sent(const uint8_t* b, size_t n, int sent) {
    uint8_t pat[4];
    size_t i;
    mk_pat(sent, pat);
    if (n < 4u) return -1;
    for (i = 0; i + 4u <= n; i++) {
        if (b[i] == pat[0] && b[i + 1u] == pat[1] && b[i + 2u] == pat[2] && b[i + 3u] == pat[3])
            return (long)i;
    }
    return -1;
}

static int count_sent(const uint8_t* b, size_t n, int sent) {
    uint8_t pat[4];
    size_t i;
    int c = 0;
    mk_pat(sent, pat);
    if (n < 4u) return 0;
    for (i = 0; i + 4u <= n; i++) {
        if (b[i] == pat[0] && b[i + 1u] == pat[1] && b[i + 2u] == pat[2] && b[i + 3u] == pat[3]) c++;
    }
    return c;
}

/* ── stderr 捕获：把加载日志引到文件，事后回读断言（不改 p 的解析行为） ── */
static int    g_saved_err_fd = -1;
static char   g_err_path[512];

static int err_capture_begin(const char* path) {
    int fd;
    fflush(stderr);
    g_saved_err_fd = dup(STDERR_FILENO);
    if (g_saved_err_fd < 0) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { close(g_saved_err_fd); g_saved_err_fd = -1; return -1; }
    if (dup2(fd, STDERR_FILENO) < 0) { close(fd); close(g_saved_err_fd); g_saved_err_fd = -1; return -1; }
    close(fd);
    snprintf(g_err_path, sizeof g_err_path, "%s", path);
    return 0;
}

static char* err_capture_end(void) {
    fflush(stderr);
    if (g_saved_err_fd >= 0) {
        (void)dup2(g_saved_err_fd, STDERR_FILENO);
        close(g_saved_err_fd);
        g_saved_err_fd = -1;
    }
    { size_t n; return (char*)slurp(g_err_path, &n); }
}

/* ══════════════════════════ 夹具构建 ══════════════════════════ */

/* 建「一个词汇子拓扑 + NFIX 个混合语种节点」，并把每个节点的 dist_sig_count 写成
 * 唯一哨兵（供文件字节探针定位）。失败返回 NULL。 */
static MasterTopology* build_fixture(void) {
    MasterTopology* m;
    HuarongTopologyNet* net;
    int i;

    m = master_topology_create(4);
    if (!m) return NULL;
    master_add_sub_topology(m, TOPO_VOCABULARY, "vocab", 64, 10);
    if (m->sub_topo_count != 1 || !m->sub_topologies[0] || !m->sub_topologies[0]->net) {
        master_topology_destroy(m);
        return NULL;
    }
    net = m->sub_topologies[0]->net;
    for (i = 0; i < NFIX; i++) {
        ReasoningNode* n = huarong_net_add_node(net, FIX[i].concept, NULL, 0);
        if (!n) { master_topology_destroy(m); return NULL; }
        n->dist_sig_count = SENT_BASE + i;   /* 定位锚（会被落盘，见 serialize_batch） */
    }
    return m;
}

/* 只建同形的空壳（同类型子拓扑、0 节点），用于加载。 */
static MasterTopology* build_shell(void) {
    MasterTopology* m = master_topology_create(4);
    if (!m) return NULL;
    master_add_sub_topology(m, TOPO_VOCABULARY, "vocab", 64, 10);
    if (m->sub_topo_count != 1 || !m->sub_topologies[0] || !m->sub_topologies[0]->net) {
        master_topology_destroy(m);
        return NULL;
    }
    return m;
}

static HuarongTopologyNet* shell_net(MasterTopology* m) {
    return m->sub_topologies[0]->net;
}

/* 在已加载的拓扑里按概念名找节点；找不到返回 NULL。 */
static ReasoningNode* find_node(HuarongTopologyNet* net, const char* concept) {
    int i;
    for (i = 0; i < net->node_count; i++) {
        ReasoningNode* n = net->nodes[i];
        if (n && n->concept && strcmp(n->concept, concept) == 0) return n;
    }
    return NULL;
}

/* ══════════════════════════ T0 夹具自检 ══════════════════════════ */

static void t0_fixture_sanity(void) {
    int i;
    int seen[PM_LANG_COUNT] = {0};

    T_START("0. 夹具自检：每个概念串的 pm_lang_of() 与期望一致，且覆盖 PmLang 全 6 值");
    for (i = 0; i < NFIX; i++) {
        CHECK(pm_lang_of(FIX[i].concept) == FIX[i].want,
              "夹具错：pm_lang_of(\"%s\")=%s，期望 %s",
              FIX[i].human, pm_lang_name(pm_lang_of(FIX[i].concept)), pm_lang_name(FIX[i].want));
        seen[(int)FIX[i].want] = 1;
    }
    for (i = 0; i < PM_LANG_COUNT; i++) {
        CHECK(seen[i] == 1, "夹具错：PmLang 取值 %d(%s) 未被覆盖", i, pm_lang_name((PmLang)i));
    }
    T_END();
}

/* ══════════════════════════ T1 文件布局探针 ══════════════════════════ */

static void t1_file_layout(const char* path) {
    size_t n = 0;
    uint8_t* b;
    int i;

    T_START("1. 布局探针：文件头 fmt_ver==EXPECT_FMT_VER，且每节点 dist_sig_count 之后紧邻 1 字节 lang");
    b = slurp(path, &n);
    CHECK(b != NULL, "无法读回 %s: %s", path, strerror(errno));
    CHECK(n >= 4u, "文件过短（%zu 字节）", n);

    {
        int v = -1;
        memcpy(&v, b, sizeof(int));
        CHECK(v == EXPECT_FMT_VER,
              "文件头 fmt_ver=%d，期望 %d —— STATE_FORMAT_VERSION 与本单测不同步？", v, EXPECT_FMT_VER);
    }

    for (i = 0; i < NFIX; i++) {
        int sent = SENT_BASE + i;
        long p = find_sent(b, n, sent);
        CHECK(count_sent(b, n, sent) == 1,
              "哨兵 0x%08X（节点 %s）在文件里出现 %d 次，应恰好 1 次（定位不可信）",
              (unsigned)sent, FIX[i].human, count_sent(b, n, sent));
        CHECK(p >= 0, "哨兵 0x%08X（节点 %s）在文件里找不到 —— dist_sig_count 未落盘？",
              (unsigned)sent, FIX[i].human);
        CHECK((size_t)p + 5u <= n,
              "哨兵 0x%08X 之后不足 1 字节（文件被截断，[v10] lang 段缺失）", (unsigned)sent);
        CHECK(b[p + 4] == (uint8_t)FIX[i].want,
              "节点 %s：dist_sig_count 之后那 1 字节 = %d(%s)，期望 lang = %d(%s)",
              FIX[i].human, (int)b[p + 4], pm_lang_name((PmLang)(int)b[p + 4]),
              (int)FIX[i].want, pm_lang_name(FIX[i].want));
    }

    free(b);
    T_END();
}

/* ══════════════════════════ T2 往返（功能契约） ══════════════════════════ */

static void t2_roundtrip(const char* path) {
    MasterTopology* m = build_shell();
    HuarongTopologyNet* net;
    int loaded, i;
    int seen[PM_LANG_COUNT] = {0};

    T_START("2. 往返：加载后节点数不变，且每个节点的 lang == pm_lang_of(concept)");
    CHECK(m != NULL, "build_shell 失败");
    loaded = master_load_state(m, path);
    CHECK(loaded == NFIX, "master_load_state 返回 %d，期望 %d", loaded, NFIX);
    net = shell_net(m);
    CHECK(net->node_count == NFIX, "加载后节点数 %d，期望 %d", net->node_count, NFIX);

    for (i = 0; i < net->node_count; i++) {
        ReasoningNode* n = net->nodes[i];
        CHECK(n != NULL && n->concept != NULL, "第 %d 个节点为空", i);
        CHECK((int)n->lang == (int)pm_lang_of(n->concept),
              "节点 \"%s\"：lang=%d(%s)，pm_lang_of=%d(%s)",
              n->concept, (int)n->lang, pm_lang_name((PmLang)(int)n->lang),
              (int)pm_lang_of(n->concept), pm_lang_name(pm_lang_of(n->concept)));
        seen[(int)n->lang] = 1;
    }
    for (i = 0; i < NFIX; i++) {
        ReasoningNode* n = find_node(net, FIX[i].concept);
        CHECK(n != NULL, "加载后找不到节点 \"%s\"", FIX[i].human);
        CHECK((int)n->lang == (int)FIX[i].want,
              "节点 \"%s\"：lang=%s，期望 %s", FIX[i].human,
              pm_lang_name((PmLang)(int)n->lang), pm_lang_name(FIX[i].want));
    }
    for (i = 0; i < PM_LANG_COUNT; i++) {
        CHECK(seen[i] == 1, "加载后未出现语种 %d(%s) —— 语种标签被抹平了？", i, pm_lang_name((PmLang)i));
    }

    master_topology_destroy(m);
    T_END();
}

/* ══════════════════════════ T3 故意写脏 ⇒ SSOT 纠偏 + 记账 ══════════════════════════ */

static void t3_mismatch_is_corrected_and_counted(const char* path, const char* dirty_path,
                                                 const char* log_path) {
    size_t n = 0;
    uint8_t* b;
    long p;
    MasterTopology* m;
    char* log;

    T_START("3. 落盘值被写脏：加载后值以 SSOT 为准，且日志出现「lang 一致性: 1 个节点」");

    /* 复制文件：把第 0 个（中文）节点的 lang 字节改成 PM_LANG_OTHER */
    b = slurp(path, &n);
    CHECK(b != NULL, "读回 %s 失败", path);
    p = find_sent(b, n, SENT_BASE + 0);
    CHECK(p >= 0 && (size_t)p + 4u < n, "哨兵定位失败（p=%ld, n=%zu）", p, n);
    b[p + 4] = (uint8_t)PM_LANG_OTHER;
    CHECK(write_all(dirty_path, b, n) == 0, "写脏文件 %s 失败", dirty_path);
    free(b);

    m = build_shell();
    CHECK(m != NULL, "build_shell 失败");
    CHECK(err_capture_begin(log_path) == 0, "无法接管 stderr：%s", strerror(errno));
    (void)master_load_state(m, dirty_path);
    log = err_capture_end();
    CHECK(log != NULL, "回读日志失败");

    /* (a) 值仍以 SSOT 为准 */
    {
        HuarongTopologyNet* net = shell_net(m);
        ReasoningNode* n0;
        CHECK(net->node_count == NFIX, "写脏后节点数 %d，期望 %d", net->node_count, NFIX);
        n0 = find_node(net, FIX[0].concept);
        CHECK(n0 != NULL, "写脏后找不到节点 \"%s\"", FIX[0].human);
        CHECK((int)n0->lang == (int)PM_LANG_ZH,
              "节点 \"%s\" 的 lang=%s，期望按 SSOT 纠偏为 %s（落盘值只是凭证）",
              FIX[0].human, pm_lang_name((PmLang)(int)n0->lang), pm_lang_name(PM_LANG_ZH));
    }

    /* (b) 记账：只有「确实读到且不等」才会打印这一句 —— 这是读端真在读的硬证据 */
    CHECK(strstr(log, "lang 一致性: 1 个节点") != NULL,
          "日志里没有「lang 一致性: 1 个节点」—— 说明落盘字节根本没被读取比对（或计数没接线）");
    /* (c) 阴性对照：不该同时出现「全部一致」 */
    CHECK(strstr(log, "全部节点与语种 SSOT 一致") == NULL,
          "同时打印了「全部节点与语种 SSOT 一致」，与实际有 1 个不符矛盾");

    free(log);
    master_topology_destroy(m);
    T_END();
}

/* ══════════════════════════ T4 旧文件（fmt_ver=9）零迁移 ══════════════════════════ */

static void t4_legacy_v9_still_loads(const char* path, const char* v9_path, const char* log_path) {
    size_t n = 0, w = 0, out_n;
    uint8_t* b;
    uint8_t* out;
    long offs[NFIX];
    char* log;
    MasterTopology* m;
    int i, k;
    long prev;

    T_START("4. 旧文件兼容：按哨兵抽掉每节点的 lang 字节 + fmt_ver 改回 9 ⇒ 仍全部对齐、语言值正确");

    b = slurp(path, &n);
    CHECK(b != NULL, "读回 %s 失败", path);
    for (i = 0; i < NFIX; i++) {
        long p = find_sent(b, n, SENT_BASE + i);
        CHECK(p >= 0, "哨兵 0x%08X 定位失败", (unsigned)(SENT_BASE + i));
        offs[i] = p;
        CHECK(count_sent(b, n, SENT_BASE + i) == 1, "哨兵 0x%08X 重复，抽字节会错位", (unsigned)(SENT_BASE + i));
    }
    /* 按升序拷贝、在每个 offs[k]+4 处删掉 1 字节（= 去掉 [v10] 段，还原 v9 布局） */
    out = (uint8_t*)malloc(n + 1u);
    CHECK(out != NULL, "malloc 失败");
    prev = 0;
    for (k = 0; k < NFIX; k++) {
        size_t cut = (size_t)offs[k] + 4u;
        CHECK(cut >= (size_t)prev && cut <= n, "抽字节偏移越界（cut=%zu, n=%zu）", cut, n);
        memcpy(out + w, b + (size_t)prev, cut - (size_t)prev);
        w += cut - (size_t)prev;
        prev = (long)cut + 1;
    }
    CHECK((size_t)prev <= n, "尾部拷贝越界");
    memcpy(out + w, b + (size_t)prev, n - (size_t)prev);
    w += n - (size_t)prev;
    out_n = w;
    free(b);

    CHECK(out_n + (size_t)NFIX == n, "抽字节后大小 %zu，期望 %zu（原 %zu - %d）",
          out_n, n - (size_t)NFIX, n, NFIX);
    { int v = 9; memcpy(out, &v, sizeof(int)); }   /* 首字段改回 9 */

    CHECK(write_all(v9_path, out, out_n) == 0, "写 %s 失败", v9_path);
    free(out);

    m = build_shell();
    CHECK(m != NULL, "build_shell 失败");
    CHECK(err_capture_begin(log_path) == 0, "无法接管 stderr：%s", strerror(errno));
    (void)master_load_state(m, v9_path);
    log = err_capture_end();
    CHECK(log != NULL, "回读日志失败");

    {
        HuarongTopologyNet* net = shell_net(m);
        CHECK(net->node_count == NFIX, "v9 文件加载后节点数 %d，期望 %d（抽字节后布局错位？）",
              net->node_count, NFIX);
        for (i = 0; i < NFIX; i++) {
            ReasoningNode* nd = find_node(net, FIX[i].concept);
            CHECK(nd != NULL, "v9 文件加载后找不到 \"%s\"", FIX[i].human);
            CHECK((int)nd->lang == (int)FIX[i].want,
                  "\"%s\"：lang=%s，期望 %s（fmt_ver<10 应由 concept 现算）",
                  FIX[i].human, pm_lang_name((PmLang)(int)nd->lang), pm_lang_name(FIX[i].want));
        }
    }
    CHECK(strstr(log, "全部节点与语种 SSOT 一致") != NULL,
          "日志里没有「全部节点与语种 SSOT 一致」");
    CHECK(strstr(log, "lang 一致性: ") != NULL && strstr(log, "个节点") == NULL,
          "fmt_ver=9 不该产生 lang 不符记账（无该段可读）");

    free(log);
    master_topology_destroy(m);
    T_END();
}

/* ══════════════════════════ T5 两处写路径同布局（fork 子进程走 locked） ══════════════════════════ */

static void t5_locked_path_same_layout(const char* batch_path, const char* locked_path) {
    pid_t pid;
    int status = 0;
    size_t nb = 0, nl = 0;
    uint8_t* fb;
    uint8_t* fl;
    int i;

    T_START("5. 锁内路径（PIVOTMIND_SAVE=locked）与流式批路径：文件逐字节一致，lang 字节同样正确");

    pid = fork();
    CHECK(pid >= 0, "fork 失败: %s", strerror(errno));
    if (pid == 0) {
        /* 子进程：环境必须在【第一次存盘之前】设好（g_save_mode 只读一次并缓存） */
        MasterTopology* m;
        int rc = -1;
        (void)setenv("PIVOTMIND_SAVE", "locked", 1);
        m = build_fixture();
        if (m) { rc = master_save_state(m, locked_path); master_topology_destroy(m); }
        _exit(rc > 0 ? 0 : 3);
    }
    CHECK(waitpid(pid, &status, 0) == pid, "waitpid 失败");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "locked 子进程异常退出 status=0x%x（3 = 存盘失败）", status);

    fb = slurp(batch_path, &nb);
    fl = slurp(locked_path, &nl);
    CHECK(fb != NULL, "读回 %s 失败", batch_path);
    CHECK(fl != NULL, "读回 %s 失败（locked 路径未产出文件？）", locked_path);
    CHECK(nb == nl, "两路径文件大小不同：batch=%zu locked=%zu", nb, nl);

    /* lang 字节在 locked 文件里同样正确 */
    for (i = 0; i < NFIX; i++) {
        int sent = SENT_BASE + i;
        long p = find_sent(fl, nl, sent);
        CHECK(count_sent(fl, nl, sent) == 1, "locked 文件里哨兵 0x%08X 出现 %d 次，应 1 次",
              (unsigned)sent, count_sent(fl, nl, sent));
        CHECK(p >= 0 && (size_t)p + 4u < nl, "locked 文件里哨兵 0x%08X 定位失败", (unsigned)sent);
        CHECK(fl[p + 4] == (uint8_t)FIX[i].want,
              "locked 文件：节点 %s 的 lang 字节 = %d(%s)，期望 %d(%s)",
              FIX[i].human, (int)fl[p + 4], pm_lang_name((PmLang)(int)fl[p + 4]),
              (int)FIX[i].want, pm_lang_name(FIX[i].want));
    }

    /* 逐字节一致 = 「两处写路径同布局」的最强形式 */
    {
        size_t d = nb;
        for (i = 0; i < (int)(nb < nl ? nb : nl); i++) {
            if (fb[i] != fl[i]) { d = (size_t)i; break; }
        }
        CHECK(d == nb,
              "两写路径字节不一致：首个差异在偏移 %zu（batch=0x%02X locked=0x%02X）",
              d, d < nb ? (unsigned)fb[d] : 0u, d < nl ? (unsigned)fl[d] : 0u);
    }

    free(fb);
    free(fl);
    T_END();
}

/* ══════════════════════════════ main ══════════════════════════════ */

int main(void) {
    char tmproot[256];
    char f_batch[512], f_dirty[512], f_v9[512], f_locked[512], f_log[512];

    printf("\n=== PivotMind 节点语种标签落盘契约单测（lang / fmt_ver 9→10）===\n\n");

    if (!host_is_le()) {
        printf("FATAL: 本单测假定小端主机（文件字节探针按 LE 解析哨兵）\n");
        return 2;
    }

    /* 环境归一：本单测要在【流式批】默认路径上取基线，故清掉一切会改行为的环境变量
       （PIVOTMIND_SAVE 会让父进程也走锁内路径；另两个开关会触发加载期迁移/归零）。 */
    (void)unsetenv("PIVOTMIND_SAVE");
    (void)unsetenv("PIVOTMIND_SKIP_DIST_SIG_MIGRATE");
    (void)unsetenv("PIVOTMIND_SKIP_EDGE_WEIGHT_RESET");
    (void)unsetenv("PIVOTMIND_RESET_EDGE_WEIGHTS");

    t0_fixture_sanity();

    snprintf(tmproot, sizeof tmproot, "/tmp/pm_lang_persist_XXXXXX");
    if (mkdtemp(tmproot) == NULL) {
        printf("FATAL: mkdtemp(%s) 失败: %s\n", tmproot, strerror(errno));
        return 2;
    }
    snprintf(f_batch,  sizeof f_batch,  "%s/state_v10.dat", tmproot);
    snprintf(f_dirty,  sizeof f_dirty,  "%s/state_dirty.dat", tmproot);
    snprintf(f_v9,     sizeof f_v9,     "%s/state_v9.dat", tmproot);
    snprintf(f_locked, sizeof f_locked, "%s/state_locked.dat", tmproot);
    snprintf(f_log,    sizeof f_log,    "%s/load.log", tmproot);

    /* 造夹具 → 存盘（默认：流式批路径） */
    {
        MasterTopology* m = build_fixture();
        int rc;
        if (!m) { printf("FATAL: build_fixture 失败\n"); return 2; }
        rc = master_save_state(m, f_batch);
        master_topology_destroy(m);
        if (rc != NFIX) {
            printf("FATAL: master_save_state 返回 %d，期望 %d（防呆阈值拦截？）\n", rc, NFIX);
            return 2;
        }
    }

    t1_file_layout(f_batch);
    t2_roundtrip(f_batch);
    t3_mismatch_is_corrected_and_counted(f_batch, f_dirty, f_log);
    t4_legacy_v9_still_loads(f_batch, f_v9, f_log);
    t5_locked_path_same_layout(f_batch, f_locked);

    /* 清理沙箱 */
    (void)remove(f_batch);
    (void)remove(f_dirty);
    (void)remove(f_v9);
    (void)remove(f_locked);
    (void)remove(f_log);
    (void)rmdir(tmproot);

    printf("\n=== Results: %d run, %d passed, %d failed ===\n", tests_run, tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
