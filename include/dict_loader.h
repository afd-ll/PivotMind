/**
 * dict_loader.h — 词典加载与词性标注
 *
 * 加载结巴分词 dict.txt 格式: word freq POS（空格分隔）
 * 提供最大化匹配分词 + 词性查询
 */
#ifndef PM_DICT_LOADER_H
#define PM_DICT_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

/* 词典条目 */
typedef struct {
    char*  word;       /* 词 */
    int    freq;       /* 词频 */
    char   pos[8];     /* 词性标签 (n/v/a/d/r...) */
} DictEntry;

/* 词典哈希表 */
typedef struct DictTable DictTable;

/* 创建词典表 (capacity: 初始容量) */
DictTable* dict_table_create(int capacity);

/* 销毁 */
void dict_table_destroy(DictTable* dt);

/* 插入条目 (word/freq/pos) */
int dict_table_insert(DictTable* dt, const char* word, int freq, const char* pos);

/* 查询词条，返回 NULL 表示未收录 */
const DictEntry* dict_table_lookup(DictTable* dt, const char* word);

/* 从 jieba dict.txt 加载词典，返回导入的条目数 */
int dict_load_jieba(DictTable* dt, const char* file_path);

/* 获取词典条目数 */
int dict_table_size(DictTable* dt);

/* 正向最大匹配分词
 * 结果写入 out_words[out_cap][...] 和 out_pos[out_cap][8]
 * 返回切分出的词数；未收录的字单独作为单字词 (pos="x")
 */
int dict_segment_text(DictTable* dt, const char* text,
                      char out_words[][64], char out_pos[][8],
                      int out_cap);

/* 词性标签 → 语法拓扑节点名的映射
 * 将 jieba 标签转为内部语法节点名 (NOUN/VERB/ADJ/...)
 * 返回 NULL 表示未知词性
 */
const char* pos_to_syntax_node(const char* pos);

#ifdef __cplusplus
}
#endif
#endif /* PM_DICT_LOADER_H */
