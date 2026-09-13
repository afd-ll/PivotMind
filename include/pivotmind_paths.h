/**
 * @file pivotmind_paths.h
 * @brief 路径 SSOT（唯一真值源）—— 数据/日志/会话/运行/书库的落点只此一处。
 *
 * 这是「路径可移植化」的**第一步**：只立真值源，**不动任何现有调用点**（替换是第 2 步）。
 * 契约（已逐条定稿，勿改）：
 *   1. 纯查询：pm_home() / pm_dir() 不碰文件系统、永不失败、总返回非空字符串；
 *      pm_dir(非法 which) ⇒ 返回 NULL（调用方必须判），绝不「悄悄返回 home」。
 *   2. 无状态 ensure：pm_ensure_dirs() 是**唯一写文件系统**的入口；不设缓存、不加锁、
 *      可重复调用；共享可变状态 = 0（并发安全，EEXIST 幂等）。
 *   3. 零分配 pm_path()：缓冲区由调用者提供，本模块不 malloc。
 *
 * pm_home() 解析顺序（命中即止；内部 pthread_once 解析一次并缓存）：
 *   1. $PIVOTMIND_HOME —— 必须【非空、非全空白、绝对路径、不含 "~"、不是 "/"】
 *   2. $HOME/pivotmind —— 运行期展开
 *   3. PM_HOME_DEFAULT —— 编译期固定绝对路径（语义 = 系统级缺省位置，由 Makefile 平台化提供）
 *   任一非法值 ⇒ 视为未设置 + 一条 WARN（说明原因，**不许静默**）。
 *
 * 目录布局（which → 路径）：
 *   PM_DIR_HOME    <home>
 *   PM_DIR_DATA    <home>/data
 *   PM_DIR_LOG     <home>/log
 *   PM_DIR_SESSION <home>/session
 *   PM_DIR_RUN     <home>/run
 *   PM_DIR_CORPUS  <home>/corpus      （书库：默认自动建 —— 作者红线）
 * 注：$PIVOTMIND_HOME / $HOME 的**尾部斜杠**会被规范掉（避免出现 "//"），除此之外逐字使用。
 *
 * 「就绪」判定（pm_ensure_dirs）：lstat 是【目录】（非文件/坏符号链接）
 *   + faccessat(W_OK, AT_EACCESS) 通过。新建目录一律 mkdir(path, 0700)；
 *   【已存在的目录绝不 chmod】；只读父目录 ⇒ 该位置位（未就绪）。
 * 返回值为**未能就绪**的位掩码（0 = 全部就绪）；明细逐条 WARN。
 */

#ifndef PIVOTMIND_PATHS_H
#define PIVOTMIND_PATHS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单个路径缓冲上限（容量由调用方按 size_t n 给出，含 '\0'） */
#define PM_PATH_MAX 4096

/* 编译期缺省数据根（第 3 级回退的目标）。
 * Makefile 传的是【裸 token】（-DPM_HOME_DEFAULT_PATH=/var/lib/pivotmind），
 * 引号在这里由 C 补齐 ⇒ 无论经过几层 shell，PM_HOME_DEFAULT 都是字符串字面量。
 * 定义放在【头文件】而不是 .c：测试 TU 等所有消费者都要看到同一个值（见契约单测第 3 条）。 */
#ifndef PM_HOME_DEFAULT
#  ifdef PM_HOME_DEFAULT_PATH
#    define PM_PATHS_STR_(x) #x
#    define PM_PATHS_STR(x) PM_PATHS_STR_(x)
#    define PM_HOME_DEFAULT PM_PATHS_STR(PM_HOME_DEFAULT_PATH)
#  else
#    define PM_HOME_DEFAULT "/var/lib/pivotmind"
#  endif
#endif

/* 目录位掩码（pm_ensure_dirs 返回值 = 未能就绪的位；0 = 全部就绪） */
#define PM_DIR_HOME    (1u << 0)
#define PM_DIR_DATA    (1u << 1)   /* $PM_HOME/data */
#define PM_DIR_LOG     (1u << 2)
#define PM_DIR_SESSION (1u << 3)
#define PM_DIR_RUN     (1u << 4)
#define PM_DIR_CORPUS  (1u << 5)   /* 书库：默认自动建（作者红线） */
#define PM_DIR_ALL     (PM_DIR_HOME | PM_DIR_DATA | PM_DIR_LOG | PM_DIR_SESSION | PM_DIR_RUN | PM_DIR_CORPUS)

/* 数据文件登记表（文件名只在此处出现一次）。
 * 落点 = <home>/data/<name>；调用点一律取 pm_file(PM_FILE_xxx)，
 * 全仓不得再出现 "pivotmind_state.dat" 这类数据文件名字面量。
 * 位号 = 数组下标；与 PM_DIR_* 同构：单 bit、非法值 ⇒ NULL。 */
#define PM_FILE_STATE         (1u << 0)   /* 主状态 pivotmind_state.dat（脑干/健康监控/TrainMode/网关共同落点） */
#define PM_FILE_BRAIN_CACHE   (1u << 1)   /* 冻结节点缓存 brain_state.dat（脑干运行缓存，退出即删） */
#define PM_FILE_FEATURES      (1u << 2)   /* 语义特征 features.bin */
#define PM_FILE_CROSS_EDGES   (1u << 3)   /* 跨拓扑边备份 cross_edges.bin */
#define PM_FILE_MEMORY_SEED   (1u << 4)   /* 记忆种子 memory_seed.dat（有 D1 防覆盖门卫） */
#define PM_FILE_EMERGENT_POS  (1u << 5)   /* 涌现词性锚点 emergent_pos.bin（带维度头 v2） */
#define PM_FILE_CONFIG        (1u << 6)   /* 运行配置 pivotmind_config.json */
#define PM_FILE_INTENT_BASE   (1u << 7)   /* 意图基座 intent_base.bin */
#define PM_FILE_PFE_STRATEGY  (1u << 8)   /* PFE 策略 pfe_strategy.bin */
#define PM_FILE_PFE_WORKSPACE (1u << 9)   /* PFE 工作区 pfe_workspace.bin */
#define PM_FILE_PRETRAIN_EMB  (1u << 10)  /* 预训练嵌入 pretrain_embeddings.bin */
#define PM_FILE_TOKEN         (1u << 11)  /* 网关凭据 gw_token（0600；原为编译期宏 GW_TOKEN_FILE） */
#define PM_FILE_ALL           (0x00000FFFu)  /* 12 个位全置 */

/* ---- 纯查询 ---- */

/** 唯一数据根。永不失败、永不为 NULL/空；解析一次并缓存。 */
const char *pm_home(void);

/** 目录路径（which 必须是单个 PM_DIR_* 位）。非法 which ⇒ NULL（调用方必须判）。 */
const char *pm_dir(unsigned which);

/* ---- 唯一写文件系统入口 ---- */

/** 确保 which 里的目录就绪（mkdir 0700 / 幂等）；返回**未能就绪**的位掩码，0 = 全部就绪。 */
unsigned pm_ensure_dirs(unsigned which);

/* ---- 零分配拼路径 ---- */

/**
 * 拼 <pm_dir(which)>[/name]：
 *   name == NULL 或 ""  ⇒ 返回【目录本身】（不带尾斜杠）
 *   name 含 ".." 段    ⇒ -1      name 是绝对路径 ⇒ -1      含 "/" 的相对子路径 ⇒ 允许
 *   buffer 不足/越界    ⇒ -1，且**不得写坏 buffer**
 * 返回写入长度（不含 '\0'）。
 */
int pm_path(char *buf, size_t n, unsigned which, const char *name);

/** == pm_path(buf, n, PM_DIR_DATA, name) */
int pm_data_path(char *buf, size_t n, const char *name);

/* ---- 数据文件登记表查询（第 2 步：调用点替换）---- */

/**
 * 数据文件路径（which 必须是单个 PM_FILE_* 位）：<home>/data/<登记文件名>。
 * 纯查询：解析一次并缓存，不碰文件系统、永不失败、永不为 NULL/空（与 pm_dir 同构）。
 * 非法 which（0 / 多 bit / 越界）⇒ NULL —— 调用方必须判，绝不「悄悄返回 home」。
 */
const char *pm_file(unsigned which);

/**
 * 日志文件路径：$PIVOTMIND_LOG_FILE（非空）优先，否则 <home>/log/pivotmind.log。
 * 纯查询：解析一次并缓存、永不失败、永不为 NULL/空。
 * 注：$PIVOTMIND_LOG_FILE 是既有的运行时开关，此处收编进 SSOT，不再由调用点各自 getenv。
 */
const char *pm_log_path(void);

#ifdef __cplusplus
}
#endif

#endif /* PIVOTMIND_PATHS_H */
