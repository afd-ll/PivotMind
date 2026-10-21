#ifndef CHARPAIR_H
#define CHARPAIR_H

/**
 * @file charpair.h
 * @brief 字对计数表 —— 键是【概念字符串对】，不是 node_id
 *
 * 为什么不用 node_id（v0.6.11 修正）：
 *   实测字对表 286 条键里 **199 条（69.6%）指向 ≥ 当前词汇节点数的 id**
 *   （最大 3045 vs 词汇 2984）⇒ 表重启/重载后无法解释，也无法倒查
 *   "这个候选词凭哪句话长出来的"。按字符串存键则**跨重启稳定、可离线审计**。
 *
 * 另：计数不再要求两个字都已存在于词汇拓扑 —— 只要文本里相邻且都是
 *   中文单字就计。否则新字（模型第一次见到的字）的字对会被整体丢弃。
 *
 * 文件格式（charpairs.bin）：
 *   magic "PMCP" · u32 version=2 · u32 count
 *   逐条：u8 len_a · a 字节 · u8 len_b · b 字节 · u32 count
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CharPairTable CharPairTable;

CharPairTable* cpt_create(void);
void cpt_destroy(CharPairTable* t);

/** 计数 +1。a/b 为 UTF-8 字符串（预期是中文单字）。返回该对当前计数。 */
uint32_t cpt_observe(CharPairTable* t, const char* a, const char* b);

/** 取计数（不存在返回 0）。 */
uint32_t cpt_get(const CharPairTable* t, const char* a, const char* b);

int cpt_pair_count(const CharPairTable* t);

/** 遍历（只读）。用于读数器/涌现判据，不参与决策。 */
void cpt_foreach(const CharPairTable* t,
                 void (*fn)(const char* a, const char* b, uint32_t c, void* ud),
                 void* ud);

int cpt_save(const CharPairTable* t, const char* path);
int cpt_load(CharPairTable* t, const char* path);

/* ==================== 全局单例（落点 charpairs.bin） ==================== */

CharPairTable* cpt_global(void);
void cpt_global_shutdown_save(void);

/** 把一段文本按【字】切分并累加相邻字对计数（只计 3 字节 UTF-8 中文单字）。
 *  返回本次参与计数的字数（0/1 表示没计）。vocab 参数不使用（保留签名兼容）。 */
struct SubTopology;
int cp_observe_text(const char* text);

#ifdef __cplusplus
}
#endif

#endif /* CHARPAIR_H */
