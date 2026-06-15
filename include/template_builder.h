#ifndef TEMPLATE_BUILDER_H
#define TEMPLATE_BUILDER_H

#include "path_encoding.h"
#include "multi_topology.h"
#include "huarong_topology.h"
#include "cognitive_controller.h"

/* ================================================================
 *  模板构建器 — 从路径频率表生成模板节点
 *
 *  分三步：
 *    1. 前缀分组: 将不可分解三元组按 (node_a, node_b, topo_id) 分组
 *    2. 软聚类:   组内按 node_c 的特征向量余弦相似度聚类
 *    3. 创建节点: 每簇生成模板节点 + 跨拓扑连接
 * ================================================================ */

/** 默认参数 */
#define TEMPLATE_MIN_RATIO            2.0f   /* 不可分解性最低比值 */
#define TEMPLATE_SIMILARITY_THRESHOLD 0.7f   /* 余弦相似度聚类阈值 */
#define TEMPLATE_MIN_CLUSTER_SIZE     2      /* 最小簇大小（低于此值不生成模板） */

/**
 * 模板构建配置
 */
typedef struct {
    int    max_templates;          /* 最多生成模板数 */
    float  min_ratio;              /* 不可分解性最低比值 */
    float  similarity_threshold;   /* 余弦相似度阈值 */
    int    min_cluster_size;       /* 最小簇成员数 */
} TemplateBuildConfig;

/**
 * 前缀分组（按 (node_a, node_b, topo_id) 分桶）
 */
typedef struct {
    int    node_a;          /* 三元组第1个节点 */
    int    node_b;          /* 三元组第2个节点 */
    int    topo_id;         /* 所属拓扑ID */
    int*   node_c_list;     /* 所有 node_c 节点ID列表 */
    float* ir_ratios;       /* 对应的不可分解性比值 */
    int*   counts;          /* 对应的出现次数 */
    int    size;            /* 当前成员数量 */
    int    capacity;        /* 分配容量 */
} TripletPrefixGroup;

/**
 * 模板簇（聚类结果）
 */
typedef struct {
    int    node_a;           /* 模板前缀第一个节点 */
    int    node_b;           /* 模板前缀第二个节点 */
    int*   member_ids;       /* 簇内 node_c 成员ID */
    int    member_count;     /* 成员数量 */
    int    representative_c; /* 代表 node_c (count 最大者) */
    int    total_count;      /* 簇内总出现次数 */
    int    template_node_id; /* 生成后的模板拓扑节点ID (-1=未生成) */
} TemplateCluster;

/* ================================================================
 *  公共工具函数
 * ================================================================ */

/**
 * 余弦相似度
 * @param a  向量 a
 * @param b  向量 b
 * @param dim 维度
 * @return 余弦相似度 [-1, 1]
 */
float template_cosine_sim(const float* a, const float* b, int dim);

/* ================================================================
 *  API
 * ================================================================ */

/**
 * 获取默认构建配置
 */
TemplateBuildConfig template_config_default(void);

/**
 * 前缀分组
 *
 * 将不可分解性分析结果按相同 (node_a, node_b, topo_id) 前缀合并。
 * 每个前缀组收集所有可能的 node_c 候选。
 *
 * @param results       不可分解性分析结果
 * @param result_count  结果数量
 * @param nodes         节点数组
 * @param node_count    节点总数
 * @param cfg           构建配置
 * @param group_count   输出: 分组数量
 * @return 分组数组 (需调用 template_free_groups 释放)
 */
TripletPrefixGroup* template_group_triplets(
    IrreducibilityResult* results, int result_count,
    ReasoningNode* const* nodes, int node_count,
    TemplateBuildConfig* cfg, int* group_count);

/**
 * 软聚类
 *
 * 对每个前缀组内的 node_c 节点进行余弦相似度聚类。
 * 相似度 > cfg->similarity_threshold 的节点归入同一簇。
 *
 * @param groups         前缀分组
 * @param group_count    分组数量
 * @param nodes          节点数组
 * @param node_count     节点总数
 * @param cfg            构建配置
 * @param cluster_count  输出: 聚类结果数量
 * @return 簇数组 (需调用 template_free_clusters 释放)
 */
TemplateCluster* template_cluster_groups(
    TripletPrefixGroup* groups, int group_count,
    ReasoningNode* const* nodes, int node_count,
    TemplateBuildConfig* cfg, int* cluster_count);

/**
 * 创建模板节点
 *
 * 为每个簇在 TOPO_TEMPLATE 拓扑中创建节点，
 * 并与词汇拓扑建立跨拓扑连接以支持投票。
 *
 * @param master         主拓扑
 * @param clusters       簇数组
 * @param cluster_count  簇数量
 * @param vocab          词汇子拓扑
 * @param max_templates  最大模板数
 * @return 创建的模板节点数量
 */
int template_build_nodes(
    MasterTopology* master,
    TemplateCluster* clusters, int cluster_count,
    SubTopology* vocab, int max_templates);

/**
 * 释放前缀分组数组
 */
void template_free_groups(TripletPrefixGroup* groups, int count);

/**
 * 释放簇数组
 */
void template_free_clusters(TemplateCluster* clusters, int count);

/* ================================================================
 *  P2: 多粒度共存
 * ================================================================ */

/**
 * 模板的模板: 从高频模板节点构建高层概念节点
 *
 * 扫描 TOPO_TEMPLATE 中的模板节点，按特征余弦相似度聚类，
 * 为每组创建概念节点加入 TOPO_CONCEPT，
 * 并建立跨拓扑连接 (概念 ↔ 成员模板)。
 *
 * @param master        主拓扑
 * @param max_concepts  最多创建概念节点数
 * @return 创建的概念节点数量
 */
int template_build_concepts(MasterTopology* master, int max_concepts);

/**
 * 冷路径稀释: 衰减不活跃模板链接的 transfer_rate
 *
 * 遍历所有涉及 TOPO_TEMPLATE 的跨拓扑连接，
 * 对 use_count < min_use 的链接衰减 transfer_rate，
 * 长期不活跃的标记但保留不删除。
 *
 * @param master         主拓扑
 * @param max_idle_rounds 超过此轮次未使用视为不活跃
 * @param decay_rate      衰减系数 (0.0-1.0, 建议 0.85f)
 * @return 衰减的链接数
 */
int template_decay_inactive_links(MasterTopology* master,
                                  int max_idle_rounds, float decay_rate);

/**
 * 运行时自构建: 从频率表 → 模板节点 一站式管线
 *
 * 检查 freq_table 是否有足够数据(entry_count >= min_entries)，
 * 如有则执行完整管线: 不可分解性分析 → 前缀分组 → 软聚类 → 模板节点创建。
 * 如模板拓扑已有节点则跳过(幂等)。
 *
 * @param master      主拓扑
 * @param min_entries 触发构建的最低频率表条目数 (建议 500)
 * @param max_templates 最多模板节点数
 * @return 创建的模板节点数, 0 表示数据不足或已构建
 */
int template_auto_build(MasterTopology* master, int min_entries, int max_templates);

/* ================================================================
 *  语法句式模板 — 基于 POS 序列的主谓宾/定中/状中等句式
 *
 *  从 CognitiveController 收集的 POS 模式自动构建模板节点。
 *  每个模板节点编码一个 POS 序列 + 槽位间连接词。
 *
 *  示例：
 *    [POS_NOUN, POS_VERB, POS_NOUN] + {"", ""}  → 主谓宾 SVO
 *    [POS_ADJ,  POS_NOUN]              + {"的"}   → 定中 Adj+的+N
 *    [POS_ADV,  POS_VERB]              + {"地"}   → 状中 Adv+地+V
 * ================================================================ */

/**
 * 从 POS 模式构建语法句式模板
 *
 * @param master    主拓扑
 * @param cc        认知调度中心（含 pos_patterns）
 * @param min_count 最低观测次数才创建模板 (建议 3)
 * @return 创建的模板节点数
 */
int template_build_from_pos_patterns(MasterTopology* master,
                                      CognitiveController* cc,
                                      int min_count);

#endif /* TEMPLATE_BUILDER_H */
