#ifndef WORD_CTX_H
#define WORD_CTX_H

/**
 * @file word_ctx.h
 * @brief 词整体左右邻分布（CX-40）—— 词级左右熵的唯一真值源
 *
 * 为什么必须有它（别再用首字/末字冒充）：
 *   charpair 表是【字级】的三条独立计数 (X,A)、(A,B)、(B,Y)，
 *   拿不出条件分布 (X | A 且后接 B)。而切片词唯一的可识别特征恰好是
 *   「组成字自己用得很广、但词整体左邻唯一」⇒ 用首字左熵冒充词左熵，
 *   等于把这个唯一信号抹掉。实测「公吵」报 6.58，真值 0.00（左邻 100% 是「老」）。
 *   词级分布只能从【语料回扫】得到 ⇒ 本模块。
 *
 * 口径：**与 tools/word_ctx.py 探针逐条对齐**（别各写一套；CX-40 表里的数字都出自那份探针）
 *   · 「段」= 一段连续汉字。**非汉字的任何东西**（空白/标点/拉丁/emoji）都算断链，
 *     等价于探针的 `HAN.findall(line)`。
 *   · 对段内每个相邻 token 对 (A,B)：
 *       左邻 = 段内前一个汉字；段首 ⇒ 记 <边界>
 *       右邻 = 段内后两个汉字；段尾 ⇒ 记 <边界>
 *   · **边界计入熵分布**（探针源码里的 BORDER 就是这么算的）：
 *     否则「只在句首出现」的词（你好/为什么）会被算成熵 0 而误杀。
 *     边界同时也单独记成 lbound/rbound，供「能否独立出现」判据使用。
 *   · 熵单位：**bit**（本模块内部）。探针报的是 **nats**，换算 ×1.4427。
 *     之所以内部用 bit：word_prob/charpair 的单 token 路径一直是 bit，
 *     同一个 wp_entropy() 不该随入参形状换单位。对外报数时按需换算即可。
 *
 * 两段生命周期（构建离线、读取在线）：
 *   构建: wctx_begin(候选, n) → wctx_scan_text(逐行) → wctx_finish()
 *         ⇒ 结果追加进结果表；多批重复（控内存）；最后 wctx_save()
 *   读取: wctx_load() → wctx_query()
 *   ⚠ 构建期状态与结果表是两套内存：finish 只把结果追加进结果表，
 *     候选/邻接/哈希在下一次 begin 时整体释放。
 */

#ifdef __cplusplus
extern "C" {
#endif

#define WCTX_KEY          32      /* 单个 token/词的字节上限（含 '\0'） */
#define WCTX_SEG_MAX      1536    /* 单段最大 token 数（4096 字节行最多 ~1365 个汉字 token） */
#define WCTX_MAX_TOKENS   65536   /* 构建期不同 token 上限 */
#define WCTX_MAX_CANDS    200000  /* 单批候选上限 */
#define WCTX_MAX_CTX      3000000 /* 单批邻接条目上限（约 48 MB） */

/** 词级统计（query 的输出） */
typedef struct {
    unsigned long long count;    /* 该词在语料中的出现次数 */
    double hl;                   /* 词整体左熵（bit，含 <边界> 这个邻类） */
    double hr;                   /* 词整体右熵 */
    unsigned lbound;             /* 出现在段首的次数（左邻是 <边界>） */
    unsigned rbound;             /* 出现在段尾的次数 */
    unsigned left_n;             /* 不同左邻个数（诊断用，= 分布的支撑集大小，含边界） */
    unsigned right_n;
} WctxStat;

/* ---------------- 构建期（离线工具） ---------------- */

/** 设定本批候选词（当前只支持 **2 个汉字** 的词）。
 *  只重置【构建期】内存；结果表跨批累积（finish 追加，begin 不清）。
 *  返回候选接受数；0 = 无有效候选。
 *  ⚠ 扫描只认汉字段 ⇒ 含拉丁/符号的「词」不会被测到（qa_retrain 只送中文双字，不受影响）。 */
int wctx_begin(const char* const* words, int n);

/** 回扫一行语料，累加本批候选的左右邻分布。返回命中的候选次数。 */
int wctx_scan_text(const char* text);

/** 结算本批：算熵并把结果追加进结果表。返回新增记录数。 */
int wctx_finish(void);

/* ---------------- 结果表（构建与读取共用） ---------------- */

/** 查询词级统计。命中返回 1 并填充 out；未测量返回 0。 */
int wctx_query(const char* word, WctxStat* out);

/** 结果表条目数。 */
int wctx_size(void);

/** 结果表是否已装载/已构建（供 word_prob 判断要不要走词级口径）。 */
int wctx_ready(void);

/** 落盘 / 装载（PMWC v1）。返回 0 成功，-1 失败。 */
int wctx_save(const char* path);
int wctx_load(const char* path);

/** 清空结果表与构建期状态（测试与重训前用）。 */
void wctx_reset(void);

/* ---------------- 诊断 ---------------- */

/** 构建期被丢弃的条目计数（超出上限时**显式记账**，绝不静默截断）：
 *  out[0]=超上限丢弃的邻接条目 · out[1]=超上限丢弃的候选 · out[2]=非 2 token 被跳过的候选 */
void wctx_drop_stats(unsigned long long out[3]);

#ifdef __cplusplus
}
#endif

#endif /* WORD_CTX_H */
