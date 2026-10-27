/* qa_memory.h — 轻量 QA 记忆检索
 *
 * 存储大量 Q|A 对，按输入 token 的交集分数检索最佳匹配 Q，返回对应 A。
 * 作为扩散引擎的 fallback：扩散产出太短/无回应时补上。
 */
#ifndef QA_MEMORY_H
#define QA_MEMORY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QAMemory QAMemory;

/* 创建 QA 记忆，从 pipe 文件加载（每行 Q|A） */
QAMemory* qa_memory_create(const char* pipe_path, int max_entries);

/* 检索：对 input 分词后，计算与所有 Q 的 token 交集分数，返回最高分 A */
const char* qa_memory_query(QAMemory* m, const char* input);

/* 释放 */
void qa_memory_destroy(QAMemory* m);

/* 获取条目数 */
int qa_memory_count(QAMemory* m);

/* 运行时添加单条 QA 对（分词后存入）。
 * CX-27 起：问/答任一无实义字符（纯标点/空白）即拒收，返回 -1。 */
int qa_memory_add(QAMemory* m, const char* question, const char* answer);

/* 被写侧闸门拒收的条目数（累计，只读；供 /debug 读数） */
unsigned long long qa_memory_rejected(void);

#ifdef __cplusplus
}
#endif
#endif
