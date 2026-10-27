#ifndef CORPUS_FILTER_H
#define CORPUS_FILTER_H

/**
 * @file corpus_filter.h
 * @brief 语料入口过滤 —— 繁体字 / 文言 / 无实义字符
 *
 * 为什么要单独一层：
 *   · QA 记忆的加载路径（qa_memory_create 读 pipe 文件）**不经过 qa_memory_add**，
 *     所以写入闸门管不到它；线上实测「天气」命中的答案就是 "= ="。
 *   · 重训（只喂 QA 对）同样要在入口过滤，否则繁体/古文会被当成正常语料学进去。
 *
 * 判据口径（都是"拒收"条件，命中任一即拒）：
 *   ① 含常用繁体字（TERM 表；只收「繁体形态与简体不同且常用」的字，避免误杀）
 *   ② 文言标记过密：之/乎/者/也/矣/焉/哉/兮/曰 在实义字里占比 ≥ 8%（且至少 3 个）
 *   ③ 无实义字符（纯标点/空白 —— 复用 wp_text_has_content）
 *   ④ 单行超长（> 512 字节，防截断产生的假词）
 *
 * ⚠ 这是「守卫」，不是完整的繁简分类器：目标是别把明显不对的东西喂进去，
 *   不追求 100% 召回。拒收计数与样本会打出来，便于核对是否误杀。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 拒收原因（供日志/读数） */
#define CF_OK          0
#define CF_TRAD        1   /* 含繁体字 */
#define CF_CLASSICAL   2   /* 文言标记过密 */
#define CF_NO_CONTENT  3   /* 无实义字符 */
#define CF_TOO_LONG    4   /* 单行过长 */

/** 判定一条 QA 文本是否可接受。why 回填原因（可传 NULL）。返回 1 = 接受。 */
int cf_accept_qa(const char* question, const char* answer, int* why);

/** 只判单段文本（不配对）。 */
int cf_accept_text(const char* text, int* why);

/** 该字符是否为常用繁体字（3 字节 UTF-8 单字；非 3 字节一律返回 0）。 */
int cf_is_trad_char(const char* ch3);

/** 拒收计数快照：out[0..4] 对应 CF_OK..CF_TOO_LONG 的累计。 */
void cf_stats(unsigned long long out[5]);

/** 打印最近 N 条被拒样本（诊断用；每条最多 60 字节）。 */
void cf_dump_samples(int n);

#ifdef __cplusplus
}
#endif

#endif /* CORPUS_FILTER_H */
