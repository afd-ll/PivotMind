#ifndef WORD_PROB_H
#define WORD_PROB_H

/**
 * @file word_prob.h
 * @brief 词概率 / 左右熵 / PMI —— 全部由【token 对计数表】现算（派生量，不落盘）
 *
 * 为什么是派生量：六个 SSOT 里「格式」由 state 的 fmt_ver 管，
 * 概率只是计数的函数 ⇒ 落盘等于制造第二真值源、且换口径要迁移。
 * 因此本模块只做「从 charpairs.bin 重建缓存」，重启后按需重建（秒级）。
 *
 * 三个量，同一套统计（中英通用，切分单位由 charpair 决定）：
 *   P(w)    = 出现次数 / 总对数        （对数压缩成 [0,1] 的 pnorm 供打分用）
 *   左熵 HL = H(w 左边的 token)  右熵 HR = H(w 右边的 token)
 *   PMI(a,b)= log2( P(ab) / (P(a)·P(b)) )
 *
 * 判据（左右熵挡跨词碎片，与 word_emerge 的既有结论一致）：
 *   · 真词 min(HL,HR) 普遍 ≥3.7（编译 3.71 / 问题 3.70 / 玄枢 4.11）
 *   · 跨词碎片 min(HL,HR) 极低（叉编 0.00 / 枢的 0.00 / 有什 0.00 / 的项 0.28）
 *   ⇒ 本模块把这条判据开放给【生成】与【存量词体检】，不再只用于新词晋升。
 *
 * 线程安全：内部一把读写锁；构建在写锁内，查询在读锁内。
 * 重建触发：token 对总数变化，或距上次构建 ≥ WP_REBUILD_SEC（默认 30s）。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 确保缓存可用（必要时重建）。返回 1 = 可用，0 = 表空/构建失败。 */
int wp_ensure(void);

/** 统计口径快照：tokens=不同 token 数, pairs=不同 token 对 数, total=对计数总和。 */
void wp_stats(int* tokens, int* pairs, unsigned long long* total);

/** P(w) 的原始值（次数/总对数）。未知返回 0。 */
double wp_p(const char* tok);

/** P(w) 的对数压缩值：log10(1+count)/log10(1+maxcount)，未知返回 0。 */
float wp_pnorm(const char* tok);

/** 次数（token 作为左成员与右成员的计数和的一半，取整）。未知返回 0。 */
unsigned long long wp_count(const char* tok);

/** 左右熵：side=0 取左熵（左边 token 的分布），side=1 取右熵。未知返回 0。
 *
 * 两套口径（**别混**，CX-40）：
 *   · 单 token（CJK 单字 / 拉丁词）⇒ charpair 的 token 级分布，精确。
 *   · 纯 CJK 多字词 ⇒ 只认 word_ctx 里的【语料实测】词级分布；
 *     没测过就是 0（未知），**绝不拿首字/末字的熵冒充**。
 *     冒充的代价：切片词「公吵」报 6.58，真值 0.00 —— 唯一可识别特征被抹掉。 */
double wp_entropy(const char* tok, int side);

/** min(左熵,右熵)。 */
double wp_min_entropy(const char* tok);

/** PMI(tok_a, tok_b)；任一侧未知返回 0。 */
double wp_pmi(const char* tok_a, const char* tok_b);

/** 生成侧增益 ∈ [WP_GAIN_MIN, WP_GAIN_MAX]（默认 0.45 ~ 1.25）。
 *  规则：概率高 ⇒ 增；左右熵高 ⇒ 增（搭配自由，更像真词）；
 *       概率低且熵低 ⇒ 压（碎片/脏词）；单字与未知 token 恒 1.0（不奖不罚）。 */
float wp_gain(const char* tok);

/** 词形合法性（供晋升闸门 / 存量体检）：
 *  CJK 单字 → 1（单字豁免）；多字与英文 → 需 min(HL,HR) ≥ min_ent 且次数 ≥ min_count。
 *  ⚠ 多字词的熵依赖 word_ctx（语料实测）。**没跑过 word_ctx 时多字词恒不合法**
 *  —— 这是有意的：宁可「不知道就不晋级」，也不要「用错的判据乱晋级」。 */
int wp_is_wordlike(const char* tok, double min_ent, unsigned long long min_count);

/* ==================== 写侧结构闸门（CX-27） ====================
 * 与 wp_is_wordlike 的分工：
 *   · 本闸门只看【结构】——非法 UTF-8 / 纯符号 / 超长。**不依赖统计**，
 *     所以冷启动（新词第一次出现、charpair 还没有它）也能判。
 *   · wp_is_wordlike 依赖统计，用于**存量体检与晋升**，不适合当写侧闸门
 *     （新词必然 0 次，会被误杀）。
 * reason 取值（wp_word_gate 回填）：
 *   0 通过 · 1 非法UTF-8/控制字节/替换符 · 2 无实义字符（纯符号/空白）
 *   3 汉字数 > 6（跨词过合并） · 4 超长（≥31 字节，charpair 键上限）
 */
int wp_word_gate(const char* w, int* reason);

/** 文本是否含实义字符（汉字/字母/数字）——供 QA 答案这类【句子级】写入判据：
 *  只拒「纯标点/纯空白」，不套用词的 6 字上限。 */
int wp_text_has_content(const char* text);

/** 闸门计数快照：out[0..4] 对应上面 5 个 reason 的累计次数（只读）。 */
void wp_gate_stats(unsigned long long out[5]);

/** 丢缓存（表被重置/大改后调用；下次 wp_ensure 重建）。 */
void wp_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* WORD_PROB_H */
