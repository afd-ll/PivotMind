/**
 * @file emergent_pos.h
 * @brief 涌现式词类系统 — 种子锚点 + 特征向量聚类实现自主词性学习
 *
 * 核心理念：
 *   人类只提供每个词类 3-5 个最典型种子词（~50 词总量），
 *   作为"锚点"。AI 通过特征向量余弦相似度，自动将新词归入
 *   最接近的锚点词类，并在运行时微调锚点中心。
 *
 * 优点：
 *   - 跨语言：中英文共享相同的锚点中心（Hebbian 学习让同功能词向量趋近）
 *   - 多义词：软分配（soft assignment）允许一个词属于多个词类
 *   - 新词类涌现：当大量词对所有已知锚点相似度都低，可自然发现新类别
 *   - 冷启动：种子词保证最小可用，无需大量训练数据
 */

#ifndef EMERGENT_POS_H
#define EMERGENT_POS_H

#include "constants.h"
#include "cognitive_controller.h"

/* ================================================================
 *  常量
 * ================================================================ */

/** 每个词类种子词数上限（中英文各一个表）
 *  v2.1 阶段0-A：5 → 32 —— 名词需 10~20 个单字种子（喂料路径按单字切分，
 *  多字种子永不命中，名词是配价宾语最关键类却只有"人"1 个单字种子）。 */
#define POS_ANCHOR_MAX_SEEDS    32

/** 涌现词类最大数（预留扩展，当前只用 10 个硬编码锚点） */
#define MAX_EMERGENT_CLASSES    32

/** 涌现池触发阈值 — 未分类词积攒到此数触发新类发现 */
#define EMERGE_POOL_TRIGGER     10

/** 涌现新类最小成员数 — 至少这么多词才能形成新词类 */
#define EMERGE_MIN_CLUSTER_SIZE 5

/** 涌现新类内部紧密度阈值 — 组内平均 pairwise cosine sim 需超过此值 */
#define EMERGE_COHERENCE_THRESH 0.65f

/** 涌现触发间隔 — 每 N 次分类触发一次涌现检查 */
#define EMERGE_CHECK_INTERVAL   500

/** 默认相似度阈值 — 低于此值不归入任何已知词类 */
#define POS_ANCHOR_DEFAULT_THRESHOLD 0.50f

/** 锚点中心微调学习率（小步慢移，保证稳定） */
#define POS_ANCHOR_LEARN_RATE  0.001f

/** 中心稳定性阈值 — 达到此值后学习率减半 */
#define POS_ANCHOR_STABILITY_THRESHOLD 0.95f

/* ================================================================
 *  数据结构
 * ================================================================ */

/**
 * POS 锚点 — 一个词类的原型表示
 *
 * 每个锚点以人类提供的 3-5 个种子词初始化其中心向量，
 * 运行时通过特征向量相似度将新词归入该类，并微调中心。
 */
typedef struct {
    POSTag      tag;                              /* 人类词类标签 */
    const char* label_cn;                         /* 中文名（调试用） */
    const char* label_en;                         /* 英文名（调试用） */
    const char* seeds[POS_ANCHOR_MAX_SEEDS];      /* 种子词 */
    int         seed_count;                       /* 实际种子数 */

    /* === 运行时统计（动态更新，不序列化）=== */
    float  centroid[PM_NODE_FEATURE_DIM];         /* 特征向量中心 */
    int    member_count;                          /* 涌现成员数（含种子词外新归入的词） */
    float  centroid_stability;                    /* 中心稳定性 0-1, 1=完全稳定 */
    int    is_active;                             /* 是否已激活（种子词已找到特征向量） */
} POSAnchor;

/**
 * 涌现词类系统 — 挂载在 CognitiveController 上
 */
typedef struct EmergentPOS {
    POSAnchor anchors[POS_COUNT];           /* 10 个硬编码锚点（排除 POS_UNKNOWN） */
    int       anchor_count;                 /* 实际激活的锚点数 */

    /* 配置 */
    float     sim_threshold;                /* 相似度阈值 */
    float     learn_rate;                   /* 中心微调学习率 */
    int       classify_count;               /* 累计分类次数（用于学习率衰减） */

    /* 新词类涌现池 */
    int       unclassified_pool_nodes[64]; /* 未分类节点 ID 池 (v0.5.9: 256→64, 聚类快16倍) */
    float     unclassified_feats[64][PM_NODE_FEATURE_DIM];
    int       unclassified_count;

    /* 涌现出的新词类（超出 POS_COUNT 的词类） */
    struct {
        int   class_id;                     /* POS_COUNT + index */
        float centroid[PM_NODE_FEATURE_DIM];
        int   member_count;
        float coherence;                    /* 组内平均 pairwise cosine sim */
        int   is_active;
        char  label_hint[32];              /* 人类可读标签提示（可选，事后命名） */
    } extra_classes[16];
    int extra_class_count;                  /* 当前涌现出的额外词类数 */
    int emerge_check_counter;               /* 涌现检查计时器 */

    /* 统计 */
    int       total_classifications;        /* 总分类次数 */
    int       soft_classifications;         /* 软分配次数（多义词） */

    /* v0.5.10: 涌现池/额外词类/计数器并发写保护。
     * tag_soft 锁外调用（article_flush 为避 O(n²) 移出 ar->mutex），
     * 多个 learn worker 并发写 unclassified/extra_classes 会堆损坏
     * （08-08 15:28 double free 实锤）。锁只保护写 ep 内部状态的
     * 汇聚点（classify / classify_soft / try_emerge / adjust_centroid）。 */
    pthread_mutex_t lock;
} EmergentPOS;

/* ================================================================
 *  API
 * ================================================================ */

/**
 * 创建涌现词类系统
 *
 * @param lang  语言标识 "zh" 或 "en"
 * @return EmergentPOS* (需 emergent_pos_destroy 释放)
 */
EmergentPOS* emergent_pos_create(const char* lang);

/**
 * 销毁涌现词类系统
 */
void emergent_pos_destroy(EmergentPOS* ep);

/**
 * 初始化锚点中心向量
 *
 * 在词汇拓扑中搜索种子词节点，取其特征向量的均值作为锚点中心。
 * 必须在词汇拓扑有足够节点后调用（启动阶段，至少 500 词）。
 *
 * @param ep     涌现词类系统
 * @param master 主拓扑（需含 TOPO_VOCABULARY）
 * @return 成功初始化的锚点数
 */
int emergent_pos_init_centroids(EmergentPOS* ep, struct MasterTopology* master);

/**
 * 硬分类 — 找最接近的锚点词类
 *
 * 取词的 512 维特征向量，与所有锚点中心计算余弦相似度，
 * 返回相似度最高且超过阈值的最接近词类。
 *
 * @param ep         涌现词类系统
 * @param features   词节点的 512 维特征向量
 * @return POSTag 标签，POS_UNKNOWN 表示未能分类
 */
POSTag emergent_pos_classify(EmergentPOS* ep, const float* features);

/**
 * 软分类 — 返回所有相似度超过阈值的词类候选
 *
 * 多义词支持：一个词可以属于多个词类。
 * tags[] 按相似度降序排列。
 *
 * @param ep       涌现词类系统
 * @param features 词节点的 512 维特征向量
 * @param result   输出：软分类结果
 */
void emergent_pos_classify_soft(EmergentPOS* ep, const float* features,
                                SoftClassResult* result);

/**
 * 按词名标注词性（双层路由）
 *
 * 第一层：检查是否是种子词（O(1) 哈希查找）
 * 第二层：特征向量余弦相似度匹配
 *
 * @param ep      涌现词类系统
 * @param master  主拓扑
 * @param word    词名
 * @return POSTag 标签
 */
POSTag emergent_pos_tag(EmergentPOS* ep, struct MasterTopology* master,
                        const char* word);

/**
 * 种子词可信标签（只走第一层，绕开 512 维语义分类器）。
 *
 * v2.1 阶段0-A：喂料路径 dist_sig 累积只用此函数的标签——它是人标先验
 * （ep->anchors[tag].seeds[] 线性命中），不是 emergent_pos_classify 学出来的
 * "错尺子"标签。命中返回 POS，否则 POS_UNKNOWN。绝不碰第二层。
 */
POSTag emergent_pos_seed_tag(EmergentPOS* ep, const char* word);

/**
 * 按词名软标注词性（双层路由，多义词支持）
 */
void emergent_pos_tag_soft(EmergentPOS* ep, struct MasterTopology* master,
                           const char* word, SoftClassResult* result);

/**
 * 手动触发锚点微调 — 在新词分类后自动调用
 *
 * 用新词的向量微调目标锚点的中心（EMA）。
 *
 * @param ep       涌现词类系统
 * @param tag      目标词类
 * @param features 词的 512 维特征向量
 */
void emergent_pos_adjust_centroid(EmergentPOS* ep, POSTag tag,
                                  const float* features);

/**
 * 获取锚点信息（调试用）
 */
const POSAnchor* emergent_pos_get_anchor(EmergentPOS* ep, POSTag tag);

/**
 * 获取统计信息
 */
int emergent_pos_anchor_count(EmergentPOS* ep);

/**
 * 尝试从池中发现新的涌现词类
 *
 * 当未分类池积攒 >= EMERGE_POOL_TRIGGER 个词时，
 * 对这些词的特征向量做双聚类尝试，
 * 若某个簇>= EMERGE_MIN_CLUSTER_SIZE 且内部紧密度>= EMERGE_COHERENCE_THRESH，
 * 则创建新的涌现词类。
 *
 * @return 新发现的词类数量（0=无新类）
 */
int emergent_pos_try_emerge(EmergentPOS* ep);
/* v0.5.19: 分布签名累积（左右邻POS + 位置标志）——语法词类的涌现尺子 */
void emergent_pos_update_dist_sig(ReasoningNode* node, int left_pos, int right_pos, int pos_flags);
/* v0.5.19: 分布聚类诊断——打印分布签名聚类 vs 语义聚类对比（验证方向） */
void emergent_pos_diag_dist_clusters(EmergentPOS* ep, MasterTopology* master);

/**
 * 获取词类的可读名称
 */
const char* emergent_pos_class_name(EmergentPOS* ep, int class_id);

/**
 * 持久化: 保存所有锚点中心 + 额外词类到磁盘
 *
 * 文件格式: 二进制, 先写 10 个硬编码锚点中心, 再写额外词类数+中心。
 * 每次 classify 调用中自动触发（每 EMERGE_CHECK_INTERVAL 次）。
 *
 * @param ep       涌现词类系统
 * @param filepath 文件路径（如 "emergent_pos.bin"）
 * @return 0=成功, -1=失败
 */
int emergent_pos_save(EmergentPOS* ep, const char* filepath);

/**
 * 持久化: 从磁盘加载锚点中心
 *
 * 若加载成功, 所有已激活锚点直接恢复, 无需懒初始化。
 * 若文件不存在或损坏, 返回 0 让调用者走懒初始化路径。
 *
 * @param ep       涌现词类系统
 * @param filepath 文件路径
 * @return 成功加载的锚点数, 0=未找到/无效
 */
int emergent_pos_load(EmergentPOS* ep, const char* filepath);

#endif /* EMERGENT_POS_H */
