# 0.4.3 — 涌现式词类系统：种子锚点 + 特征向量聚类

> **日期**: 2026-06-25 | **类型**: 新增

## 动机

当前词性标注依赖硬编码字典（`chinese_pos_lookup` ~90行 strcmp + 后缀规则）和预制句式模板（`CN_PATTERNS[]` — SVO/SOV/SVOC 等 16 种）。这是"人类灌输给 AI 的知识"，而非 AI 自主归纳的。

核心问题：**词性是人类总结的概念，AI 应该从数据中自己发现语法的功能分类。**

## 设计理念

**种子锚点（POS Anchor）**：人类只提供每个词类 3-5 个最典型的词作为"原型"——共 ~50 个词（中英各一套）：

| 词类 | 中文种子 |
|------|---------|
| 名词 | 苹果、人、时间、桌子、思想 |
| 动词 | 吃、看、跑、想、说 |
| 形容词 | 大、好、美、快、新 |
| ... | ... |

系统用这些种子词的特征向量初始化每个词类的"锚点中心"。运行时，任何新词的 512 维 Hebbian 特征向量与所有锚点中心计算余弦相似度，自动归入最接近的词类——并微调该锚点中心（EMA，学习率 0.001）。

**三层路由**保证了从冷启动到自主演进的平滑过渡：

```
词性标注:
  1. 涌现锚点（特征向量余弦相似度 + 中心微调）  ← 优先
  2. 跨拓扑连接 vocab → TOPO_SYNTAX              ← 辅助
  3. 硬编码 chinese_pos_lookup 字典               ← 冷启动兜底

模板匹配:
  1. 涌现槽位（tpl_emergent_slot 软匹配）        ← 优先
  2. 硬编码 POS 槽位（tpl_pos_seq 精确匹配）     ← 兼容
  3. 特征向量余弦相似度兜底                        ← 最终保障
```

## 核心数据结构

### `include/emergent_pos.h`

```c
typedef struct {
    POSTag      tag;
    const char* label_cn, *label_en;          // 人类可读标签
    const char* seeds[5]; int seed_count;      // 种子词
    float  centroid[PM_NODE_FEATURE_DIM];      // 锚点中心 (512维, EMA更新)
    int    member_count; float centroid_stability; // 运行时统计
    int    is_active;
} POSAnchor;

typedef struct EmergentPOS {
    POSAnchor anchors[POS_COUNT];               // 10 个硬编码锚点
    int       extra_class_count;                // 涌现出的额外词类数
    struct { /* class_id, centroid, coherence, ... */ } extra_classes[16];

    int unclassified_count;                     // 未分类词池 (用于新类涌现)
    float unclassified_feats[256][512];

    // 配置: sim_threshold=0.50, learn_rate=0.001
    int emerge_check_counter;                   // 涌现检查计时器
} EmergentPOS;
```

### `include/huarong_topology.h` — ReasoningNode 扩展

```c
// 多义词软分配: 一个词可属于多个涌现词类
int   emergent_class_count;
int   emergent_class_ids[4];
float emergent_class_confs[4];

// 模板节点 — 涌现槽位（与 tpl_pos_seq 并行）
int   tpl_emergent_slot[4];
float tpl_emergent_conf[4];
```

### `include/cognitive_controller.h` — SoftClassResult

```c
typedef struct {
    POSTag tags[SOFT_CLASS_MAX];    // 候选词类 (按相似度降序)
    float  confs[SOFT_CLASS_MAX];   // 余弦相似度
    int    count;                   // 实际候选数
} SoftClassResult;
```

## 关键算法

### 1. 硬分类 (`emergent_pos_classify`)

```
features(512维) → cos_sim 对所有锚点中心 → 取最大 sim > 0.50
  → 成功: EMA 微调中心 + 回写节点 emergent_class_ids
  → 失败: 加入未分类池
```

### 2. 软分类 (`emergent_pos_classify_soft`) — 多义词支持

"计划"的特征向量可能在 512 维空间中同时接近"名词锚点"和"动词锚点"——返回两个候选，按相似度排序。

### 3. 新词类涌现 (`emergent_pos_try_emerge`)

```
未分类池 ≥10 词 → pairwise 余弦相似度矩阵
  → 贪婪聚类 (sim > 0.65)
  → 簇 ≥5 成员 → 创建新词类 class_id=POS_COUNT+index
  → 新词类自动参与后续分类
```

每 500 次分类触发一次检查，最多 16 个额外词类。

### 4. 锚点中心持久化

- 启动时加载 `emergent_pos.bin`（magic="PMEP"），成功则跳过懒初始化
- 每 5000 次分类自动保存，销毁时也保存
- 二进制格式：锚点中心 + 成员数 + 稳定性 + 额外词类全套

## 代码改动

### 新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `include/emergent_pos.h` | ~200 | POSAnchor/EmergentPOS 结构体、中英文种子表、API 声明 |
| `src/emergent_pos.c` | ~720 | 完整实现 |

### 修改文件

| 文件 | 改动 |
|------|------|
| `include/huarong_topology.h` | ReasoningNode +7 字段（涌现词类+模板涌现槽位） |
| `include/cognitive_controller.h` | SoftClassResult 类型、EmergentPOS 前向声明/指针、新 API |
| `src/cognitive_controller.c` | 创建/销毁/加载 EmergentPOS；`pos_tag_emergent/soft` 实现 |
| `src/multi_topology.c` | `master_get_node_pos_tag` 三层路由；`master_find_template_for_pair_nolock` 三层匹配；walk 阶段 9 处 `pos_tag_chinese` → `pos_tag_emergent` |
| `src/template_builder.c` | Pipeline A/B 模板节点同步填充 `tpl_emergent_slot` |
| `src/dialog_system.c` | 日志提示涌现系统待懒初始化 |

## 调用链

```
cognitive_controller_create()
  → emergent_pos_create("zh")        ← 创建种子锚点表
  → emergent_pos_load("emergent_pos.bin")  ← 尝试恢复持久化数据

首次调用 pos_tag_emergent():
  → emergent_pos_tag()               ← 懒初始化: 扫描词汇拓扑找种子词
    → 种子词? → 直接返回
    → 非种子? → emergent_pos_classify(features)
      → cos_sim 匹配 → 成功 → EMA 微调中心 + 回写节点
      → 失败 → 入未分类池 + 定期触发 emergent_pos_try_emerge()

walk 阶段:
  → pos_tag_emergent(cc, word)       ← 9 处替换原 pos_tag_chinese
  → scaffold_bonus / pattern_match

template_auto_build():
  → template_build_nodes() → 填充 tpl_emergent_slot[0..2]

cognitive_controller_destroy():
  → emergent_pos_destroy() → emergent_pos_save() ← 退出前持久化
```

## 跨语言支持

英文种子词使用同一 `POSTag` 枚举：

```c
// 英文种子（同一 POSTag 标签）
{POS_NOUN, {"apple","time","table","idea","person"}, 5},
{POS_VERB, {"eat","run","think","say","see"},        5},
// ...
```

Hebbian 学习让中文"苹果"和英文"apple"的 512 维向量自然趋近（分布语义假设），它们会被归入同一个锚点中心——天然跨语言。

## 向后兼容

- `pos_tag_chinese()` 和 `chinese_pos_lookup()` 完整保留，作为第三层兜底
- `CN_PATTERNS[]` 句式模板完整保留，作为第二层兜底
- 所有新增字段在序列化流程中不参与（运行时字段，已在 Memory 中注明）

## 编译验证

```
gcc -Wall -Wextra -O2 -std=gnu99 -Iinclude
新增文件: emergent_pos.h / emergent_pos.c — 零警告
修改文件: 全部通过编译
```
