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

#ifdef __cplusplus
}
#endif
#endif
