/**
 * @file word_embed.h
 * @brief 邻词分布指纹 —— 「词类」的表示层
 *
 * 为什么要有这一层（2026-10-27 实测结论）：
 *   `emergent_pos_classify_soft()` 用 `cosine(features, centroid, 512维)` 做分类，
 *   而重训时 features 是 `rand()*0.1-0.05` 的**随机噪声** ⇒ 余弦在噪声上没有语义，
 *   分类结果等于随机。`dist_sig[26]`（本该是正确表示）在重训时压根没写，
 *   线上只累积 0–18 次（语料有 1.1 亿次观测）。
 *
 * 本模块提供**一阶分布指纹**：一个词「旁边出现哪些字」的分布。
 * 方法照 Schütze (1993/1995) 的分布聚类简化版：
 *   特征维 = 频次 top-F 的单字（Zipf 定律：这批几乎全是功能字）
 *   向量 = [左邻在特征词上的计数] + [右邻在特征词上的计数] → PPMI → L2 归一化
 *
 * 已验证（tools/cluster_probe.py，纯 Python 同算法）：无监督自动分出
 *   一这每那哪两几三各某（数量/指示）· 个些种天次件段位切（量词）
 *   也都或并还只又却（副词/连词）· 让但给被与跟把（介词/连词）
 *   失低害期烦痛疲希苦勇迷愿沮紧绝累（情绪形容词）
 * 单例率 18%（阈值 0.22 / 特征 250）。
 */
#ifndef WORD_EMBED_H
#define WORD_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

/** 懒加载：优先读 word_embed.bin，失败则从 charpairs 现建。返回 1 就绪 / 0 不可用。 */
int wemb_ensure(void);

/** 显式从当前 charpairs 全表构建（重训/离线工具用）。返回词数，<0 失败。 */
int wemb_build(void);

/** 存/读 word_embed.bin。 */
int wemb_save(const char* path);
int wemb_load(const char* path);

/** 取词的指纹向量（L2 已归一化，可直接点积=余弦）。词不在表内返回 NULL。 */
const float* wemb_vector(const char* word);

/** 两词的指纹余弦。-1 = 至少一个无指纹（调用方应回退，不要当成「完全不相似」）。 */
float wemb_similar(const char* a, const char* b);

/** 直接对两个向量算余弦（同维度）。 */
float wemb_cosine(const float* a, const float* b);

/** 向量维度（供调用方分配栈缓冲）。 */
int wemb_dim(void);

/** 词的非零维数（稀疏度）。<WEMB_MIN_NNZ 时余弦不可信。 */
int wemb_nnz(const char* word);

/** 一组词的归一化原型（均值向量）。返回参与的词数，<0 表示全不可用。
 *  为什么需要：逐种子取 max 会被单个异常种子带偏；均值是「这一类的中心」。 */
int wemb_centroid(const char** words, int n, float* out);

/** 读数：词数 / 特征维数 / 是否就绪。指针可传 NULL。 */
void wemb_stats(int* nwords, int* ndim, int* ready);

/** 特征词表（前 n 个），供诊断打印。 */
int wemb_feature_words(const char** out, int max);

#ifdef __cplusplus
}
#endif

#endif /* WORD_EMBED_H */
