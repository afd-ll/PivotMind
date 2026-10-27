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
 *   magic "PMCP" · u32 version=3 · u32 count
 *   逐条：u8 len_a · a 字节 · u8 len_b · b 字节 · u32 count
 *   （v3 起单 token ≤31 字节、键含英文词；v2 旧表按版本不符自动作废重建）
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

/** 把一段文本切 token 并累加【相邻 token 对】计数，返回 token 数。
 *  切分（v3 起中英双轨）：
 *    · CJK 单字      → 一个 token
 *    · 拉丁字母/数字 → 一个 token（小写化，≤31 字节）⇒ 英文按空格天然成词
 *    · 空白/标点/emoji/其它文字 → 断链
 *  中文行为与旧版一致（逐字成对）；英文从此也进同一张表，
 *  因此 P(w) / 左右熵 / PMI 对英文同样成立（同一公式，换切分单位）。 */
int cp_observe_tokens(const char* text);

/** 旧名保留（article_reader 在用），等价于 cp_observe_tokens。 */
int cp_observe_text(const char* text);

#ifdef __cplusplus
}
#endif

#endif /* CHARPAIR_H */
