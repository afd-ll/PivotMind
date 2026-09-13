/**
 * @file pivotmind_paths.c
 * @brief 路径 SSOT 实现 —— 契约见 include/pivotmind_paths.h。
 *
 * 三条不可让步的性质（与作者定稿）：
 *   ① pm_home()/pm_dir() 纯查询：只做字符串运算，不碰文件系统，永不失败，总非空。
 *   ② pm_ensure_dirs() 无状态：不设缓存、不加锁（共享可变状态 = 0），EEXIST 幂等。
 *   ③ pm_path() 零分配：只往调用者的缓冲区写，长度不够返回 -1 且不越界。
 */

#define _GNU_SOURCE 1   /* faccessat + AT_EACCESS + strerror_r 的 GNU 变体 */

#include "pivotmind_paths.h"
#include "error.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* 编译期缺省 PM_HOME_DEFAULT 的定义已上移到 include/pivotmind_paths.h：
 * 所有消费者（含测试 TU）必须看到同一个字符串宏，否则第 3 级回退无从验证。 */

/* 编译期事实：PM_HOME_DEFAULT 必须是绝对路径（不许相对、不许运行期兜底）。
   ⛔ C 语言没有编译期字符串自省：字符串字面量的下标**不是**整型常量表达式（那是 C++ 的规则），
      实测 gcc 15 -std=gnu99 下「数组维度为负 / enum 值 / _Static_assert / __builtin_choose_expr」
      四种写法**全部**被拒（原始输出见 hermes-workspace/pivotmind-review/fix-plans/paths-ssot-raw.md §4「断言写法探针」）。
      硬门因此落在两处**真正能拦死**的地方：
        ① 主门 —— 构建系统：Makefile 对 $(PM_HOME_DEFAULT) 的绝对路径 $(error) 闸门；
        ② 次门 —— 本文件被【单独编译】（不经 Makefile）时：下面的 GCC __attribute__((error)) 静态门，
           常量折叠后若首字符不是 '/'，就在**编译期**硬报错。
   运行期一行兜底都不加（相对路径在这里按原样使用，由上面两道门在上游拦死）。 */
#if defined(__GNUC__)
extern void pm_home_default_must_be_absolute(void)
    __attribute__((error("PM_HOME_DEFAULT must be an absolute path (first char must be '/')")));
#endif

#define PM_DIR_COUNT 6

/* 位号 = 数组下标；PM_DIR_HOME(1u<<0)..PM_DIR_CORPUS(1u<<5) */
static const char *const g_subdir[PM_DIR_COUNT] = {
    NULL,       /* PM_DIR_HOME    —— 就是 $PM_HOME 本身 */
    "data",     /* PM_DIR_DATA */
    "log",      /* PM_DIR_LOG */
    "session",  /* PM_DIR_SESSION */
    "run",      /* PM_DIR_RUN */
    "corpus"    /* PM_DIR_CORPUS  —— 书库（默认自动建） */
};

#define PM_FILE_COUNT 12

/* 位号 = 数组下标；PM_FILE_STATE(1u<<0)..PM_FILE_TOKEN(1u<<11)。
   数据文件名只在本表出现一次（SSOT）；调用点一律取 pm_file()。 */
static const char *const g_filename[PM_FILE_COUNT] = {
    "pivotmind_state.dat",      /* PM_FILE_STATE         —— 主状态（脑干/健康监控/TrainMode/网关） */
    "brain_state.dat",          /* PM_FILE_BRAIN_CACHE   —— 冻结节点缓存（退出即删） */
    "features.bin",             /* PM_FILE_FEATURES      —— 语义特征 */
    "cross_edges.bin",          /* PM_FILE_CROSS_EDGES   —— 跨拓扑边备份 */
    "memory_seed.dat",          /* PM_FILE_MEMORY_SEED   —— 记忆种子（D1 防覆盖门卫） */
    "emergent_pos.bin",         /* PM_FILE_EMERGENT_POS  —— 涌现词性锚点（维度头 v2） */
    "pivotmind_config.json",    /* PM_FILE_CONFIG        —— 运行配置 */
    "intent_base.bin",          /* PM_FILE_INTENT_BASE   —— 意图基座 */
    "pfe_strategy.bin",         /* PM_FILE_PFE_STRATEGY  —— PFE 策略 */
    "pfe_workspace.bin",        /* PM_FILE_PFE_WORKSPACE —— PFE 工作区 */
    "pretrain_embeddings.bin",  /* PM_FILE_PRETRAIN_EMB  —— 预训练嵌入 */
    "gw_token"                  /* PM_FILE_TOKEN         —— 网关凭据（0600，跨重启不变） */
};

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static char g_home[PM_PATH_MAX];                  /* 解析结果（pthread_once 内写一次） */
static char g_dirs[PM_DIR_COUNT][PM_PATH_MAX];    /* 六个目录（同上，纯只读查询） */
static char g_files[PM_FILE_COUNT][PM_PATH_MAX];  /* 十二个数据文件（同上） */
static char g_log_path[PM_PATH_MAX];              /* 日志文件（同上；$PIVOTMIND_LOG_FILE 优先） */

/* 单 bit 掩码 ⇒ 位号；非法（0 / 多 bit / 越界 limit）⇒ -1。
   limit 由调用方给出（PM_DIR_COUNT / PM_FILE_COUNT），两族共用同一套位号规则。 */
static int pm_bit_index(unsigned which, int limit) {
    unsigned v;
    int i;
    if (which == 0u) return -1;
    if ((which & (which - 1u)) != 0u) return -1;   /* 多于一位 */
    for (v = which, i = 0; (v & 1u) == 0u; v >>= 1, i++) { /* 找最低置位 */ }
    return (i < limit) ? i : -1;
}

/* 去尾部 '/'（全是 '/' ⇒ 保留一个）；src 为空串/为 NULL ⇒ -1。
   g_home 里绝不出现 "//"，因为后续拼接一律 "%s/%s"。 */
static int pm_trim_trailing_slash(char *dst, size_t n, const char *src) {
    size_t len;
    if (dst == NULL || n == 0u) return -1;
    dst[0] = '\0';
    if (src == NULL) return -1;
    len = strlen(src);
    while (len > 1u && src[len - 1u] == '/') len--;
    if (len == 0u || len + 1u > n) return -1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

/* 拼接 base/sub（base == "/" 时不留 "//"）；成功 0，失败 -1 且 dst 置空 */
static int pm_join(char *dst, size_t n, const char *base, const char *sub) {
    int need;
    if (dst == NULL || n == 0u || base == NULL || sub == NULL) return -1;
    dst[0] = '\0';
    need = (strcmp(base, "/") == 0) ? snprintf(dst, n, "/%s", sub)
                                    : snprintf(dst, n, "%s/%s", base, sub);
    if (need < 0 || (size_t)need >= n) {
        dst[0] = '\0';
        return -1;
    }
    return 0;
}

/* 候选 home 值是否合法：非空、非全空白、绝对路径、不含 '~'、不是 "/"、长度留足拼接余量。
   v == NULL 表示「未设置」——不算非法（由调用方区分「未设置」与「设置了但非法」）。 */
static int pm_home_value_ok(const char *v, const char **why) {
    size_t i;
    if (v == NULL) { *why = "未设置"; return 0; }
    if (v[0] == '\0') { *why = "空串"; return 0; }
    for (i = 0u; v[i] != '\0'; i++) {
        char c = v[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' && c != '\f') break;
    }
    if (v[i] == '\0') { *why = "全空白"; return 0; }
    if (v[0] != '/') { *why = "不是绝对路径"; return 0; }
    if (strchr(v, '~') != NULL) { *why = "含 '~'"; return 0; }
    if (strcmp(v, "/") == 0) { *why = "是文件系统根 \"/\""; return 0; }
    if (strlen(v) + 16u >= PM_PATH_MAX) { *why = "过长（> PM_PATH_MAX-16）"; return 0; }
    *why = NULL;
    return 1;
}

/* 解析一次并缓存：$PIVOTMIND_HOME → $HOME/pivotmind → PM_HOME_DEFAULT。 */
static void pm_resolve_once(void) {
    const char *why = NULL;
    const char *env;
    char buf[PM_PATH_MAX];
    int i;

    /* ---- 第 1 级：$PIVOTMIND_HOME ---- */
    env = getenv("PIVOTMIND_HOME");
    if (env != NULL) {
        if (pm_trim_trailing_slash(buf, sizeof buf, env) != 0) {
            why = "空串";
        } else if (pm_home_value_ok(buf, &why)) {
            memcpy(g_home, buf, strlen(buf) + 1u);
            goto build_dirs;
        }
        LOG_WARNING("pivotmind_paths: 忽略 $PIVOTMIND_HOME=\"%s\"（%s），按解析顺序回退到下一级",
                    env, (why != NULL) ? why : "非法值");
    }

    /* ---- 第 2 级：$HOME/pivotmind ---- */
    env = getenv("HOME");
    if (env != NULL) {
        if (pm_trim_trailing_slash(buf, sizeof buf, env) != 0) {
            why = "空串";
        } else if (pm_home_value_ok(buf, &why)) {
            if (pm_join(g_home, sizeof g_home, buf, "pivotmind") == 0) goto build_dirs;
            why = "拼接失败（长度超限）";
        }
        LOG_WARNING("pivotmind_paths: 忽略 $HOME=\"%s\"（%s），回退到编译期缺省 PM_HOME_DEFAULT",
                    env, (why != NULL) ? why : "非法值");
    }

    /* ---- 第 3 级：PM_HOME_DEFAULT（编译期绝对路径；由文件头的静态门 + Makefile 的 $(error) 闸门保证）---- */
    why = NULL;
    if (pm_trim_trailing_slash(buf, sizeof buf, PM_HOME_DEFAULT) == 0 &&
        pm_home_value_ok(buf, &why)) {
        memcpy(g_home, buf, strlen(buf) + 1u);
        LOG_WARNING("pivotmind_paths: 无可用的 $PIVOTMIND_HOME/$HOME，采用编译期缺省 PM_HOME_DEFAULT=%s",
                    g_home);
    } else {
        LOG_WARNING("pivotmind_paths: PM_HOME_DEFAULT=\"%s\" 不可用（%s），兜底 /pivotmind",
                    PM_HOME_DEFAULT, (why != NULL) ? why : "非法值");
        memcpy(g_home, "/pivotmind", sizeof "/pivotmind");
    }

build_dirs:
    for (i = 0; i < PM_DIR_COUNT; i++) {
        if (i == 0) {
            memcpy(g_dirs[0], g_home, strlen(g_home) + 1u);
        } else if (pm_join(g_dirs[i], sizeof g_dirs[i], g_home, g_subdir[i]) != 0) {
            /* 校验已保证长度余量 ⇒ 理论不可达；仍不静默 */
            LOG_WARNING("pivotmind_paths: 目录 \"%s\" 拼接失败，退化为 home 本身（不应发生）",
                        g_subdir[i]);
            memcpy(g_dirs[i], g_home, strlen(g_home) + 1u);
        }
    }

    /* 数据文件 = <home>/data/<登记文件名>（第 2 步：调用点替换后的唯一落点来源） */
    for (i = 0; i < PM_FILE_COUNT; i++) {
        if (pm_join(g_files[i], sizeof g_files[i], g_dirs[1], g_filename[i]) != 0) {
            /* 校验已保证长度余量 ⇒ 理论不可达；仍不静默 */
            LOG_WARNING("pivotmind_paths: 数据文件 \"%s\" 拼接失败，退化为 data 目录本身（不应发生）",
                        g_filename[i]);
            memcpy(g_files[i], g_dirs[1], strlen(g_dirs[1]) + 1u);
        }
    }

    /* 日志文件：$PIVOTMIND_LOG_FILE（非空、绝对路径）优先 —— 收编既有运行时开关，
       不再由调用点各自 getenv；空串/相对路径/超长 ⇒ 视为未设置 + WARN。 */
    env = getenv("PIVOTMIND_LOG_FILE");
    if (env != NULL && env[0] != '\0') {
        if (env[0] != '/' || pm_trim_trailing_slash(g_log_path, sizeof g_log_path, env) != 0) {
            LOG_WARNING("pivotmind_paths: 忽略 $PIVOTMIND_LOG_FILE=\"%s\""
                        "（要求绝对路径且长度 < PM_PATH_MAX），改用 <home>/log/pivotmind.log", env);
            env = NULL;
        }
    }
    if (env == NULL || env[0] == '\0') {
        if (pm_join(g_log_path, sizeof g_log_path, g_dirs[2], "pivotmind.log") != 0) {
            LOG_WARNING("pivotmind_paths: 日志路径拼接失败，退化为 home 本身（不应发生）");
            memcpy(g_log_path, g_home, strlen(g_home) + 1u);
        }
    }
}

/* errno → 文本（strerror_r，线程安全；buf 为本线程栈上缓冲） */
static const char *pm_errno_str(int err, char *buf, size_t n) {
    char *p;
    buf[0] = '\0';
    p = strerror_r(err, buf, n);
    if (p != NULL && p[0] != '\0') return p;
    if (buf[0] != '\0') return buf;
    snprintf(buf, n, "errno=%d", err);
    return buf;
}

/* 就绪判定 + 按需 mkdir(0700)。0 = 就绪，-1 = 未就绪（并已 WARN）。 */
static int pm_ensure_one(const char *path) {
    int attempt;

    for (attempt = 0; attempt < 2; attempt++) {
        struct stat st;
        char eb[128];

        if (lstat(path, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                LOG_WARNING("pivotmind_paths: %s 已存在但【不是目录】（文件/坏符号链接），视为未就绪；"
                            "本函数不删除、不覆盖", path);
                return -1;
            }
            if ((st.st_mode & 0077u) != 0u) {
                LOG_WARNING("pivotmind_paths: %s 权限过宽（对组/其他开放）：mode=0%03o，期望 0700；"
                            "本函数【不改既有目录权限】，请自行 chmod",
                            path, (unsigned)(st.st_mode & 07777u));
            }
            if (faccessat(AT_FDCWD, path, W_OK, AT_EACCESS) != 0) {
                int err = errno;
                LOG_WARNING("pivotmind_paths: %s 不可写（faccessat(W_OK, AT_EACCESS) 失败：%s），视为未就绪",
                            path, pm_errno_str(err, eb, sizeof eb));
                return -1;
            }
            return 0;
        }

        if (errno != ENOENT) {
            int err = errno;
            LOG_WARNING("pivotmind_paths: lstat(%s) 失败：%s，视为未就绪",
                        path, pm_errno_str(err, eb, sizeof eb));
            return -1;
        }

        if (mkdir(path, 0700) == 0) return 0;
        if (errno != EEXIST) {
            int err = errno;
            LOG_WARNING("pivotmind_paths: mkdir(%s, 0700) 失败：%s，视为未就绪",
                        path, pm_errno_str(err, eb, sizeof eb));
            return -1;
        }
        /* EEXIST：并发对手刚建好（或存在同名文件）⇒ 复检一次（有界，不递归） */
    }

    LOG_WARNING("pivotmind_paths: %s 反复无法就绪（mkdir 报 EEXIST 后复检仍不就绪）", path);
    return -1;
}

const char *pm_home(void) {
#if defined(__GNUC__) && defined(__OPTIMIZE__)
    /* 编译期硬门（常量折叠，不带运行期开销）：相对路径 ⇒ 编译不过。见文件头那段说明。
     * ⚠ 必须带 __OPTIMIZE__ 守卫：__attribute__((error)) 的检查依赖优化器把 `if (0)` 消掉；
     *   -O0 下该调用点残留 ⇒ 即便 PM_HOME_DEFAULT 合法也会误报（构建直接失败）。
     *   -O0 的兜底由 Makefile 主门承担：PM_HOME_DEFAULT 不以 '/' 开头 ⇒ $(error)。 */
    if (PM_HOME_DEFAULT[0] != '/') pm_home_default_must_be_absolute();
#endif
    pthread_once(&g_once, pm_resolve_once);
    return g_home;
}

const char *pm_dir(unsigned which) {
    int idx = pm_bit_index(which, PM_DIR_COUNT);
    if (idx < 0) return NULL;   /* 非法 which：绝不「悄悄返回 home」 */
    pthread_once(&g_once, pm_resolve_once);
    return g_dirs[idx];
}

unsigned pm_ensure_dirs(unsigned which) {
    unsigned bad = 0u;
    int i;

    if ((which & ~PM_DIR_ALL) != 0u) {
        LOG_WARNING("pivotmind_paths: pm_ensure_dirs 收到未知位 0x%x（已知位 = PM_DIR_ALL 0x%x），未知位按未就绪返回",
                    which & ~PM_DIR_ALL, PM_DIR_ALL);
        bad |= (which & ~PM_DIR_ALL);
    }

    for (i = 0; i < PM_DIR_COUNT; i++) {
        unsigned bit = 1u << i;
        const char *d;
        if ((which & bit) == 0u) continue;
        d = pm_dir(bit);
        if (d == NULL || d[0] == '\0') {
            LOG_WARNING("pivotmind_paths: 目录位 0x%x 取不到路径，视为未就绪", bit);
            bad |= bit;
            continue;
        }
        if (pm_ensure_one(d) != 0) bad |= bit;
    }
    return bad;
}

/* name 是否含 ".." 段（"." 段不算；"..." 不算） */
static int pm_has_dotdot_segment(const char *name) {
    const char *p;
    if (name[0] == '.' && name[1] == '.' && (name[2] == '\0' || name[2] == '/')) return 1;
    for (p = name; *p != '\0'; p++) {
        if (*p == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '\0' || p[3] == '/')) return 1;
    }
    return 0;
}

int pm_path(char *buf, size_t n, unsigned which, const char *name) {
    const char *dir = pm_dir(which);
    size_t dl, nl;

    if (dir == NULL || buf == NULL || n == 0u) return -1;

    dl = strlen(dir);

    /* name == NULL / "" ⇒ 目录本身（不带尾斜杠） */
    if (name == NULL || name[0] == '\0') {
        if (dl + 1u > n) { buf[0] = '\0'; return -1; }
        memcpy(buf, dir, dl + 1u);
        return (int)dl;
    }

    if (name[0] == '/' || pm_has_dotdot_segment(name)) {
        buf[0] = '\0';
        return -1;   /* 绝对路径 / 含 ".." 段：拒绝 */
    }

    nl = strlen(name);
    if (dl + 1u + nl + 1u > n) {
        buf[0] = '\0';   /* 容量不足：只在界内写一个 '\0'，其余字节一律不动 */
        return -1;
    }
    memcpy(buf, dir, dl);
    buf[dl] = '/';
    memcpy(buf + dl + 1u, name, nl + 1u);
    return (int)(dl + 1u + nl);
}

int pm_data_path(char *buf, size_t n, const char *name) {
    return pm_path(buf, n, PM_DIR_DATA, name);
}

const char *pm_file(unsigned which) {
    int idx = pm_bit_index(which, PM_FILE_COUNT);
    if (idx < 0) return NULL;   /* 非法 which：绝不「悄悄返回 home」 */
    pthread_once(&g_once, pm_resolve_once);
    return g_files[idx];
}

const char *pm_log_path(void) {
    pthread_once(&g_once, pm_resolve_once);
    return g_log_path;
}

/* ============================================================================
 * 旧扁平布局审计（v0.5.33 新增；契约见 include/pivotmind_paths.h）
 *
 * 背景：v0.5.30 把数据落点从「<home>/xxx」改成「<home>/data/xxx」。线上旧部署的
 *   数据文件仍在 <home> 根目录 —— 换上新二进制后引擎找不到 data/<name>，会
 *   【新建空状态并正常启动】（不报错），表现为「升级后玄枢失忆」。
 *   这里只做审计与 fail-loud，不自动搬文件（搬数据是部署动作，交给
 *   deploy/migrate-home-layout.sh，需人工确认）。
 * ============================================================================ */

int pm_legacy_layout_report(char *report, size_t cap) {
    int i, hits = 0;
    size_t used = 0;
    char legacy[PM_PATH_MAX];

    if (report != NULL && cap > 0u) report[0] = '\0';
    pthread_once(&g_once, pm_resolve_once);

    for (i = 0; i < PM_FILE_COUNT; i++) {
        struct stat st;
        size_t nl;

        /* SSOT 落点已存在 ⇒ 该文件不在旧布局里（含「两边都有」的情况） */
        if (stat(g_files[i], &st) == 0) continue;
        if (pm_join(legacy, sizeof legacy, g_home, g_filename[i]) != 0) continue;
        if (stat(legacy, &st) != 0) continue;   /* 旧落点也没有 ⇒ 干净 */

        hits++;
        if (report == NULL || cap == 0u) continue;
        nl = strlen(g_filename[i]);
        /* "  · "(5B) + name + '\n'(1) + '\0'(1) = nl + 7；留 1B 余量。整行放不下则不写。 */
        if (used + nl + 8u <= cap) {
            used += (size_t)snprintf(report + used, cap - used,
                                     "  \xc2\xb7 %s\n", g_filename[i]);
        }
    }
    return hits;
}

int pm_legacy_layout_guard(const char *who, int refuse) {
    char list[512];
    struct stat st;
    const char *tag = (who != NULL && who[0] != '\0') ? who : "pivotmind";
    const char *env;
    int hits, state_missing;

    hits = pm_legacy_layout_report(list, sizeof list);
    if (hits <= 0) return 0;   /* 干净 ⇒ 静默 */

    pthread_once(&g_once, pm_resolve_once);
    /* g_files[0] 即 PM_FILE_STATE：主状态是否已在 SSOT 落点就位 */
    state_missing = (stat(g_files[0], &st) != 0);
    env = getenv("PIVOTMIND_ALLOW_LEGACY_LAYOUT");

    if (!state_missing) {
        /* 主状态已就位：旧文件只是残留（当前被忽略），提示即可 */
        LOG_WARNING("[%s] 检测到 %d 个数据文件仍留在【旧扁平布局】%s/ 下，而路径 SSOT 只读 %s/data/"
                    " —— 当前以 SSOT 为准（旧文件被忽略）：\n%s"
                    "            迁移/清理：deploy/migrate-home-layout.sh --home \"%s\"",
                    tag, hits, g_home, g_home, list, g_home);
        return 0;
    }

    if (env != NULL && env[0] == '1') {
        LOG_WARNING("[%s] 旧扁平布局命中 %d 个文件，但 PIVOTMIND_ALLOW_LEGACY_LAYOUT=1"
                    " ⇒ 允许按【空状态】启动（已显式放行）：\n%s",
                    tag, hits, list);
        return 0;
    }

    LOG_ERROR("[%s] 拒绝启动：数据文件仍在【旧扁平布局】%s/，而 SSOT 主状态 %s 不存在。\n"
              "            继续启动会【静默从空脑开始】（不报错）= 玄枢失忆，故此处 fail-loud。\n%s"
              "            ① 迁移（推荐）：deploy/migrate-home-layout.sh --home \"%s\" --yes\n"
              "            ② 确要用空状态启动：PIVOTMIND_ALLOW_LEGACY_LAYOUT=1",
              tag, g_home, g_files[0], list, g_home);
    return refuse ? 1 : 0;
}