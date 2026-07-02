# 玄枢 PivotMind 架构文档

> 当前版本: **v0.4.13** — 13 脑区完整架构 + POS 语法映射 + 边特异性权重 + 涌现式词类 + 配置系统 + 神经网络子系统 + 编译零警告

## 整体架构

![架构总览图](docs/images/架构总览图.png)

### 架构分层

| 层级 | 职责 | 核心文件 |
|------|------|---------|
| **多拓扑网络层** | 11 层子拓扑接收输入，逐字分词后激活对应节点，跨拓扑传播 | `huarong_topology.c`, `multi_topology.c` |
| **认知调度层** | 意图向量计算、满意度评估、retry 循环、在线学习调整 | `cognitive_controller.c` |
| **对话学习层** | 联想推理、回复生成、赫布在线学习、状态持久化、POS 语法映射 | `dialog_system.c`, `autonomic_learner.c`, `dialog_generate.c` |
| **推理编排层** (v0.3) | 6 模式推理编排、任务分解、子目标调度、多候选竞争、冲突检测 | `prefrontal_executive.c`, `idea_arena.c` |
| **脑干节律层** (v0.4) | 昼夜心跳、激活衰减、自发激活、存盘调度、堆监控 | `brainstem.c` |
| **配置管理层** (v0.4.7) | 运行时 JSON 配置加载、脑区启停控制 | `json_config.c` |

---

## 脑区划分

玄枢按哺乳动物大脑皮层的功能分区建模，13 个脑区/子系统各司其职，通过丘脑信号总线通信。**全部脑区均已完整实现，无占位代码。**

| 脑区 | 文件 | 行数 | 子拓扑归属 | 功能 |
|------|------|------|----------|------|
| **前额叶 (Prefrontal)** | `prefrontal.c` | 132 | 词汇、语义、语用、概念 | 对话生成，diffusion → ACC 自适应门控 |
| **前额叶执行器 (PFE)** | `prefrontal_executive.c` | 1,502 | 跨 CC 复用 | 6 模式推理编排、任务分解、冲突检测、子目标调度 |
| **海马体 (Hippocampus)** | `hippocampus.c` | 135 | 上下文、领域 | 记忆巩固、QA 重放、感知联动 |
| **DMN** | `dmn.c` | 46 | — | 默认模式网络：梦境联想、闲暇探索 |
| **杏仁核 (Amygdala)** | `amygdala.c` | 97 | 情绪、文化 | 情绪效价采样、探索/利用平衡 |
| **感知皮层 (Perception)** | `perception.c` | 838 | — | Sogou+Bing 双 provider 联网搜索、article_reader 语义管线 |
| **布罗卡区 (Broca)** | `broca.c` | 56 | 语法、模板 | 模板自动构建与衰减调度 |
| **小脑 (Cerebellum)** | `cerebellum.c` | 80 | — (全局监控) | 硬件资源保护、CPU/内存限速 |
| **下丘脑 (Hypothalamus)** | `hypothalamus.c` | 149 | — | 四维需求驱动(好奇/获取/社交/舒适)、昼夜耦合 |
| **丘脑 (Thalamus)** | `thalamus.c` | 540 | — | 信号总线、资源门控、脑区间通信路由、脑区启停管理 |
| **脑干 (Brainstem)** | `brainstem.c` | 613 | — | 昼夜节律、激活衰减、自发激活、堆监控 |
| **扣带回 (ACC)** | `cingulate.c` | 223 | — | 四维序列评估(语义+模板+情绪+长度) |
| **想法竞技场 (IdeaArena)** | `idea_arena.c` | 722 | — | 多候选五维竞争选择、侧抑制、多巴胺调节 |
| **网状激活系统 (Reticular)** | `reticular.c` | 133 | — | 觉醒/警觉水平调节 |

## 三大核心模块

![核心模块流程图](docs/images/核心模块流程图.png)

### 1. 拓扑网络层

**核心文件**: `src/huarong_topology.c`, `src/multi_topology.c`, `include/huarong_topology.h`, `include/multi_topology.h`

**职责**: 节点/边管理、跨拓扑连接、激活传播、拓扑排序

**关键 API**:
- `huarong_net_find_or_create_node()` — 查找或创建节点
- `topology_walk_greedy()` — 贪心走边（五维评分 + 边特异性折扣）
- `topology_walk_beam()` — Beam Search（K=3）
- `master_add_cross_link()` — 添加跨拓扑连接
- `master_reevaluate_cross_links()` — 跨拓扑连接质量重评估

### 2. 认知调度层

**核心文件**: `src/cognitive_controller.c`, `include/cognitive_controller.h`

**职责**: 意图向量计算、满意度评估、retry 循环、涌现式词类、BPTT 置信度接入

**关键 API**:
- `compute_intent()` — 计算意图向量（含 NN 置信度因子）
- `evaluate_draft()` — 评估草案质量
- `emergent_pos_classify()` — 涌现式词类分类

### 3. 对话学习层

**核心文件**: `src/dialog_system.c`, `src/autonomic_learner.c`, `src/dialog_generate.c`, `src/diffusion.c`

**职责**: 分词解析、联想推理、回复生成、赫布学习、多层扩散

**关键 API**:
- `dialog_input_create()` — 创建对话输入
- `autonomic_learn_from_dialog()` — 赫布在线学习
- `diffusion_generate()` — 多层扩散回复生成

---

## 核心数据结构

![数据结构关系图](docs/images/数据结构关系图.png)

### MasterTopology（认知主控）

位于 `include/multi_topology.h`，全局唯一顶层结构：

```c
typedef struct MasterTopology {
    StringPool* string_pool;                // 共享字符串池
    SubTopology** sub_topologies;           // 子拓扑指针数组
    int sub_topo_count;                     // 当前子拓扑数量
    CrossTopologyLink** cross_links;        // 跨拓扑边数组
    int cross_link_count;                   // 跨拓扑边总数
    CrossTopoAdjEntry** cross_adj;          // 跨拓扑邻接表（O(1) 索引）
    int cross_adj_count;
    pthread_rwlock_t rwlock;                // 读写锁
    ThreadPool* thread_pool;                // 并行推理线程池
    CrossTopoHitRecord cross_hit_records[CROSS_HIT_TABLE_SIZE]; // 动态跨拓扑建边跟踪
    int cross_hit_round;                    // 当前推理轮次
} MasterTopology;
```

### SubTopology（子拓扑）

```c
typedef struct SubTopology {
    int topo_id;                    // 拓扑唯一 ID
    TopologyType type;              // 拓扑类型枚举
    const char* name;               // 拓扑名称
    HuarongTopologyNet* net;        // 底层拓扑网络
    NodeHashTable* node_hash;       // 节点哈希表（加速按名查找）
    int priority;                   // 推理优先级 (1-10)
    float weight;                   // 在主拓扑中的权重
    float recent_activation;        // leaky integrator 衰减
    time_t last_used;
    bool is_active;                 // 是否激活
    pthread_rwlock_t rwlock;        // 子拓扑级读写锁
} SubTopology;
```

### ReasoningNode（推理节点）

```c
typedef struct ReasoningNode {
    int node_id;                            // 节点唯一标识
    char* concept;                          // 概念名称/字符
    float* features;                        // 512 维语义向量 (NODE_FEATURE_DIM=512)
    int feature_dim;

    // 连接边（动态数组，预分配 DEFAULT_CONNECTION_CAPACITY=10）
    ReasoningNode** connections;            // 指向目标节点的指针数组
    float* connection_weights;              // 逻辑强度
    float* connection_motivational_bias;    // 动机倾向
    float* connection_confidences;          // 边置信度
    int connection_count;
    int connection_capacity;

    // 节点状态
    float activation;                       // 当前激活值（0.0-1.0）
    float confidence;                       // 节点置信度
    CognitiveConfidence* cognitive_confidence;  // 三维置信度
    float valence;                          // 效价 (-1.0 ~ +1.0)
    float heat;                             // 热度（路径多样性）
    int selection_count;                    // 被贪心走边选中次数
    NodeType node_type;                     // 功能词/普通词/专有名词

    // 涌现式词类（v0.4.3）
    int   emergent_class_count;
    int   emergent_class_ids[4];
    float emergent_class_confs[4];
} ReasoningNode;
```

### CrossTopologyLink（跨拓扑连接）

```c
typedef struct CrossTopologyLink {
    int link_id;
    int from_topo_id, from_node_id;         // 源拓扑+节点
    int to_topo_id, to_node_id;             // 目标拓扑+节点
    float weight;                           // 动态权重（use_count 提升）
    const char* relation;                   // 关系类型
    int bidirectional;                      // 是否双向
    float transfer_rate;                    // 跨拓扑激活传递率（重评估动态更新）
    time_t created_time;
    int use_count;                          // 使用次数（动态权重学习）
} CrossTopologyLink;
```

---

## 多拓扑网络

### 各拓扑职能

| 枚举值 | 拓扑名称 | 职能描述 |
|--------|---------|---------|
| TOPO_VOCABULARY (0) | 词汇拓扑 | 单字节点 + 边，承载最基础的字间联想 |
| TOPO_SEMANTIC (1) | 语义拓扑 | 概念级推理，抽象程度高于词汇 |
| TOPO_EMOTION (2) | 情绪拓扑 | 情感极性（正/负效价），影响回复情感倾向 |
| TOPO_SYNTAX (3) | 语法拓扑 | 字序/搭配约束，维持输出语法合理性 |
| TOPO_CONTEXT (4) | 上下文拓扑 | 对话历史节点，支持多轮上下文 |
| TOPO_DOMAIN (5) | 领域拓扑 | 专业术语关联，如"量子"→"计算" |
| TOPO_PRAGMA (6) | 语用拓扑 | 对话策略，如问答/闲聊/解释模式切换 |
| TOPO_CULTURE (7) | 文化拓扑 | 文化背景关联，影响特定文化语境下的联想 |
| TOPO_CONCEPT (8) | 概念拓扑 | 数值、规则、实体等高抽象概念 |
| TOPO_MASTER (9) | 主拓扑 | 全局调度与优先级管理 |
| TOPO_TEMPLATE (10) | 模板拓扑 | 句式模板，路径编码递归抽象 |

### 拓扑间关系

```
词汇拓扑 ←跨拓扑→ 语义拓扑
↕                     ↕
情绪拓扑 ←跨拓扑→ 领域拓扑
↕                     ↕
语法拓扑 ←跨拓扑→ 语用拓扑
↕                     ↕
上下文拓扑 ←跨拓扑→ 文化拓扑
         ↕
       概念拓扑
```

跨拓扑连接在训练后通过 `rebuild_cross_connections()` 批量重建，基于节点特征（512 维语义向量）的余弦相似度、精确名称匹配和最左子串匹配三种策略。动态新建跨拓扑连接通过 `CrossTopoHitRecord` 跟踪。

### 跨拓扑连接质量重评估 (v0.4.7)

每 600 tick（约 10 分钟）通过 `master_reevaluate_cross_links()` 重算 `transfer_rate`：

```c
transfer_rate = 0.4 + 0.6 × min(2.0, use_count / expected_use) × weight
```

高频使用的连接提升传导效率，低频连接降低但保留最低传导能力（0.4）。

### 跨拓扑连接机制

使用扁平化邻接表索引实现 O(1) 查找：

```c
// 编码索引 = topo_id × MAX_NODES_PER_TOPO + node_id
int adj_idx = topo_id * MAX_NODES_PER_TOPO + node_id;
CrossTopoAdjEntry* entry = master->cross_adj[adj_idx];
while (entry) {
    CrossTopologyLink* link = master->cross_links[entry->link_index];
    // 激活传递
    entry = entry->next;
}
```

激活传递公式：`new_activation = src_activation × link->weight × link->transfer_rate × motivation × valence`

---

## 联想推理引擎

### 激活扩散机制

位于 `src/dialog_system.c` 的 `dialog_topo_worker()` 实现并行拓扑传播：

```c
for (int n = 0; n < sub->net->node_count; n++) {
    ReasoningNode* node = sub->net->nodes[n];
    if (node->activation < 0.15f) continue;

    for (int c = 0; c < node->edge_count; c++) {
        Edge* edge = &node->edges[c];
        float new_activation =
            edge->weight                    // 边权重
            × node->activation              // 源激活
            × confidence_factor             // 置信度因子
            × activation_multiplier         // 低置信放大(1.3x) / 高置信抑制(0.7x)
            × embed_factor                  // 512 维特征向量余弦相似度
            × DECAY_RATE;                   // 0.7 衰减系数

        if (new_activation > ACTIVATION_THRESHOLD) {
            connected->activation = max(connected->activation, new_activation);
            dialog_add_association(reasoning, ...);
        }
    }
}
```

并行策略：每跳内按拓扑级并行（`DialogTopoTask`），活跃拓扑数 ≤ CPU 核数时效率最高。

### 贪心走边 `topology_walk_greedy()`

**签名**:
```c
int topology_walk_greedy(SubTopology* sub, int start_node_id,
                         int* path_out, float* scores_out,
                         int max_len, unsigned char* visited,
                         float intent_weight,
                         MasterTopology* master,
                         const float* query_anchor,
                         void* cc_ptr);
```

**评分公式（五维加法混合）**:
```
score = 0.28 × edge_weight
      + 0.22 × edge_confidence
      + 0.11 × edge_motivational_bias
      + 0.28 × target_activation
      + 0.11 × target_confidence
最终 × (1.0 + 0.6 × target_valence) × edge_spec
```

**边特异性权重 (v0.4.13)**:

当节点出边 > 4 条时，计算边权重集中度，区分语义专一边与均匀 hub 边：

```c
concentration = max_w / sum_w_all;           // 最强边占比
expected = 1.0 / edge_count;                 // 均匀分布期望
edge_spec = 0.55 + 0.45 × (concentration / (concentration + expected));
```

- 高集中度（少数强边支配，如"苹果"→"吃"） → edge_spec → 1.0（不打折）
- 低集中度（hub 词均匀分布，如"你"→8000条弱边） → edge_spec → 0.55（55折）
- 效果：强语义关联边权重不受影响，弱随机共现边被压制。

**热度衰减机制（路径多样性）**:
- 节点被选中后 `selection_count++`，`heat = 1.0 / (1.0 + 0.1 × selection_count)`
- 功能词衰减慢（0.95^count），专有名词衰减快（0.85^count）
- 前 3 步"低保期"几乎不剪枝，后续逐步收紧

### Beam Search `topology_walk_beam()`

签名与 greedy 完全兼容（K=3 并行路径），避免贪心局部最优。

### 跨拓扑走边 `topology_walk_cross()`

每步同时评估本拓扑内连接和跨拓扑连接，允许路径在拓扑间自然跳转：
- 跨拓扑跳跃的"边权重" = `link->weight × transfer_rate`
- 其余四维从目标节点获取

### 与传统图搜索的本质区别

| 维度 | 传统图搜索（BFS/DFS/A*） | PivotMind 走边 |
|------|------------------------|---------------|
| 搜索目标 | 最短路径/最小代价 | 最"像话"的路径（多维评分） |
| 评分函数 | 单一边权重/启发式 | 5维加法混合 × 效价乘法调节 |
| 多样性 | 无内在多样性机制 | 热度衰减 + 节点类型差异化 |
| 激活上下文 | 仅路径节点参与 | 所有激活节点共同影响评分 |
| 跨层穿越 | 需显式跨层边 | 跨拓扑连接天然支持多拓扑行走 |

---

## 多层扩散引擎

### 虚词过滤 (v0.4.7)

`diffusion.c` 内置 `is_function_word()` 检查约 130 个中英文虚词，在三层过滤：

1. **活跃集更新**: 扩散每轮的 top-K 跳过虚词
2. **加权评分**: 虚词不进入候选表
3. **输出阶段**: 兜底再次过滤

防止 "the be not to have are..." 或 "的了是在……" 虚词串污染输出。

### 连接词 POS 语法关系映射 (v0.4.13)

扩散输出连接词由 POS 词类对动态映射，替代硬编码轮换。三级回退机制：

```
Level1: 模板匹配（按需）
Level2: pos_connector_map(prev_pos, curr_pos) — 按词类对返回语法连接词
Level3: 直接拼接（无连接词）
```

`pos_connector_map` 核心映射：

| prev_pos | curr_pos | 连接词 | 说明 |
|----------|----------|--------|------|
| N | N | `的` | 名词修饰：人类的语言 |
| Adj | N | `的` | 形容词修饰：美丽的风景 |
| Adv | V | `地` | 状语修饰：快速地运行 |
| N | Adj | `是` | 谓语句：天空是蓝色 |
| N | V | `""` | 主谓结构：太阳升起 |
| V | N | `""` | 动宾结构：吃苹果 |

**效果**：输出从无语法关系的词序列升级为符合语法规则的句子片段。

### 扩散流程

```
输入分词 → vocab 节点激活 → 3 跳扩散 (vocab↔semantic, vocab→template)
         → 加权排序: total = vocab×0.45 + sem×0.25 + tpl×0.20 + emo×0.10
         → 模板导向 → 侧抑制去重 → 输出
```

---

## 对话系统

### 两层意图识别

位于 `src/dialog_system.c`：

**第一层：关键词分类**
```c
INTENT_EXPLAIN_WORDS = {"为什么","原因","怎么回事","为什么呢","怎么会","导致","造成"}
INTENT_HOWTO_WORDS   = {"怎么","如何","怎样","方法","步骤","操作"}
INTENT_QUERY_WORDS   = {"是什么","什么是","哪个","多少","谁","什么时候"}
INTENT_COMPARE_WORDS = {"比较","区别","不同","vs","对比","差异"}
INTENT_LEARN_WORDS   = {"学习","记住","了解","知道","认识"}
INTENT_CHAT_WORDS    = {"你好","嗨","在吗","嘿","喂"}
```

**第二层：认知调度向量**

意图向量作为调度权重影响激活传播：
```c
new_activation *= intent_weights[sub->type];  // 乘性调节
```

### 回复生成流程

1. **精确匹配检查**：`memory_retrieve(memory, "response:{完整输入}")` → 命中则直接返回
2. **拓扑驱动生成**：`master_generate_response()` → 跨拓扑走边 → POS 语法映射连接 → 生成概念序列
3. **联想兜底**：从 top-5 激活节点出发，`topology_walk_greedy()` 生成回复
4. **自动学习**：`autonomic_learn_from_dialog(input, output)`

---

## 涌现式词类系统 (Emergent POS) **v0.4.3**

### 设计理念

不硬编码词性字典。人类只提供每词类 3-5 个"种子锚点"词（中英各 ~50 个）。
系统用种子词的 512 维 Hebbian 特征向量初始化锚点中心。

### 运行机制

1. **分类**: 新词通过余弦相似度自动归入最接近的词类（阈值 0.50）
2. **微调**: 归类成功后以 EMA（学习率 0.001）微调锚点中心
3. **涌现**: 未分类词 ≥ 10 个 → 贪婪聚类（sim > 0.65，簇 ≥ 5 成员）→ 涌现新词类

### 三层路由

```
词性标注:
  1. 涌现锚点（特征向量余弦相似度 + 中心微调）  ← 优先
  2. 跨拓扑连接 vocab → TOPO_SYNTAX              ← 辅助
  3. 硬编码 chinese_pos_lookup 字典               ← 冷启动兜底
```

### 跨语言

Hebbian 学习让中文"苹果"和英文"apple"的 512 维向量自然趋近，天然跨语言。

### 持久化

锚点中心保存到 `emergent_pos.bin`（magic="PMEP"），重启不丢失。

---

## 学习系统

### 预训练 (Skip-gram/CBOW)

**核心文件**: `src/pretrain.c` (1,624 行)

| 参数 | 默认值 | 说明 |
|------|--------|------|
| embedding_dim | 64 | 嵌入维度 |
| window_size | 5 | 窗口大小（最大 10） |
| negative_samples | 5 | 负采样数 |
| learning_rate | 0.025 → 0.0001 | 线性衰减 |
| momentum | 0.9 | 动量加速 |
| grad_clip | 5.0 | 梯度裁剪 |

### 学习器矩阵

| 学习器 | 文件 | 方式 | 说明 |
|--------|------|------|------|
| **自主学习者** | `autonomic_learner.c` | 赫布在线 | 共现即强化，16 分片并发更新 |
| **主动学习者** | `active_learner.c` | 7×24 后台 | 自动获取新知识，分析概念关系 |
| **自我学习者** | `self_learner.c` | 好奇驱动 | 好奇心采样 → 深度游走 → 知识审查 → 自纠错 |
| **BPTT 学习者** | `bptt_learner.c` | 时序反向传播 | RNN + Linear，Adam 优化器（lr=0.001） |

### BPTT ↔ 拓扑桥接

`bptt_learner.c` 实现神经网络与拓扑网络的双向桥接：

- **训练方向**: 拓扑节点特征向量 → RNN(512→256) → Linear(256→512) → MSE 损失
- **反馈方向**: RNN 预测输出 → 余弦相似度匹配词汇节点 → 预激活 top-8 节点（偏置 0.25）
- **调度接入** (v0.4.7): `bptt_get_confidence()` → `cognitive_controller.nn_confidence` → 意图权重因子

### 灾难性遗忘防护

**核心文件**: `src/catastrophic_forgetting.c` (1,385 行)

基于 EWC（弹性权重巩固）：Fisher 信息矩阵标记参数重要性，新学习时选择性保护已有知识。
默认 λ=1000.0，支持在线 EWC 衰减（gamma=0.9）。

---

## 神经网络子系统

玄枢内置完整的轻量级神经网络引擎，可配合拓扑系统使用：

| 模块 | 文件 | 说明 |
|------|------|------|
| 张量运算 | `tensor.c` | 多维张量 create/destroy/broadcast/clone/view |
| 矩阵运算 | `matrix_ops.c` | 矩阵乘/转置/加/缩放 |
| 层级层 | `layer.c` | 8 种层类型 (LINEAR/RELU/SIGMOID/TANH/SOFTMAX/DROPOUT/EMBEDDING/SIMPLE_RNN) |
| LSTM | `layer_lstm.c` | 完整 LSTM：W/R 矩阵、bias、双向、层归一化 |
| GRU | `layer_gru.c` | 完整 GRU：更新/重置门、双向、层归一化 |
| RNN | `layer_rnn.c` | Simple RNN 前向/反向 + Embedding 层 |
| 模型 | `model.c` | 多层堆叠、前向传播、MSE 损失、序列化 |
| 生成模型 | `generative_model.c` | 词汇表 (PAD/SOS/EOS/UNK) + 文本生成 |
| 训练器 | `trainer.c` | Mini-batch 训练、学习率调度 |
| 优化器 | `optimizer.c` | SGD / Adam (β1=0.9, β2=0.999) / RMSprop |
| 量化 | `quantization.c` | FP16/INT8/INT4/INT2 |
| 剪枝 | `pruning.c` | MAGNITUDE/RANDOM/GRADIENT/STRUCTURED |
| 注意力 | `attention.c` | Bahdanau/Luong/Self-Attention/Multi-Head |

---

## 运行时配置系统 (v0.4.7)

### 概述

`pivotmind_config.json`（可选，缺失时全部回退 constants.h 默认值）。采用最小化 JSON 解析器，零外部依赖。

配置五大类：拓扑、学习、推理、时钟、脑区开关。

### 可调参数全表

#### 拓扑参数 (topology)

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `feature_dim` | int | 512 | 节点特征向量维度（影响跨拓扑余弦相似度精度） |
| `max_nodes_per_topo` | int | 10000 | 单子拓扑最大节点数 |
| `cross_hit_table_size` | int | 2048 | 跨拓扑动态建边跟踪表大小 |

#### 学习参数 (learning)

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `decay_rate` | float | 0.7 | 激活传播每跳衰减系数（降低=联想更远，升高=更聚焦） |
| `learn_rate` | float | 0.005 | 在线学习 EMA 基准速率 |
| `autonomic_shard_count` | int | 16 | 赫布学习边更新分片数（影响并行度） |
| `active_learner_interval` | int | 300 | 主动学习器后台扫描间隔（秒） |
| `max_connections` | int | 8000 | 自主学习刷盘触发连接数上限 |
| `flush_threshold` | int | 50 | 自主学习刷盘触发更新次数 |
| `idle_flush_seconds` | int | 30 | 自主学习空闲刷盘间隔（秒） |

#### 推理参数 (inference)

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `max_response_len` | int | 2048 | 回复文本最大长度（字节） |
| `default_hop_count` | int | 3 | 走边默认跳数 |
| `max_associations` | int | 100 | 单次推理最大联想记录数 |
| `max_hops_reasoning` | int | 200 | 递归激活传播硬上限（防栈溢出） |

#### 后台时钟参数 (clock)

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `tick_interval_ms` | int | 1000 | 脑干心跳间隔（毫秒） |
| `decay_per_tick` | float | 0.97 | 每 tick 激活值衰减系数 |
| `spontaneous_prob` | float | 0.0001 | 自发激活概率（模拟'走神'） |
| `spontaneous_strength` | float | 0.15 | 自发激活强度 |
| `consolidate_interval` | int | 10 | 记忆巩固间隔（tick 数） |

#### 脑区开关 (brain_regions)

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `prefrontal` | bool | true | 前额叶：对话与决策 |
| `hippocampus` | bool | true | 海马体：记忆巩固与感知联动 |
| `dmn` | bool | true | 默认模式网络：梦境联想 |
| `perception` | bool | true | 感知皮层：联网搜索（网络断开时建议禁用） |
| `broca` | bool | true | 布罗卡区：模板构建 |
| `cerebellum` | bool | true | 小脑：CPU/内存硬件保护 |
| `amygdala` | bool | true | 杏仁核：情绪调控 |
| `hypothalamus` | bool | true | 下丘脑：四维需求驱动 |

### 完整配置示例

```json
{
    "topology": {
        "feature_dim": 512,
        "max_nodes_per_topo": 10000,
        "cross_hit_table_size": 2048
    },
    "learning": {
        "decay_rate": 0.7,
        "learn_rate": 0.005,
        "autonomic_shard_count": 16,
        "active_learner_interval": 300,
        "max_connections": 8000,
        "flush_threshold": 50,
        "idle_flush_seconds": 30
    },
    "inference": {
        "max_response_len": 2048,
        "default_hop_count": 3,
        "max_associations": 100,
        "max_hops_reasoning": 200
    },
    "clock": {
        "tick_interval_ms": 1000,
        "decay_per_tick": 0.97,
        "spontaneous_prob": 0.0001,
        "spontaneous_strength": 0.15,
        "consolidate_interval": 10
    },
    "brain_regions": {
        "prefrontal": true,
        "hippocampus": true,
        "dmn": true,
        "perception": true,
        "broca": true,
        "cerebellum": true,
        "amygdala": true,
        "hypothalamus": true
    }
}
```

### 低配 ARM 调优建议

```json
{
    "topology": { "max_nodes_per_topo": 3000 },
    "learning": { "active_learner_interval": 600, "max_connections": 3000 },
    "clock": { "tick_interval_ms": 2000, "spontaneous_prob": 0.00005 },
    "brain_regions": { "perception": false, "dmn": false }
}
```

### 脑区生命周期管理

Thalamus 新增 `enabled[THAL_SUBSYSTEM_COUNT]` 标志和 `thalamus_enable_region()` / `thalamus_is_region_enabled()` API。
Brainstem 各 tick 函数检查 enabled 状态，禁用脑区的 tick 完全跳过——不初始化、不调度、不消耗 CPU。
禁用感知皮层同时避免无网络环境下的连接超时等待。

---

## 认知调度中心

### 意图向量计算

完整的因子融合公式（v0.4.7 增加 NN 置信度）：

```c
float w = intent_base[i] * (1.0 + tanh(learned_base[i] - 1.0) * 0.3)  // 在线学习
        × (1.0 + context_bias × ctx_activations[i])                     // 上下文关联
        × (1.0 + novelty_bias × (novelty[i] - 1.0))                     // 新颖性
        × (1.0 + valence_bias × (valence_p[i] - 1.0))                   // 效价
        × (1.0 + coherence_scale × (coherence[i] - 1.0))               // 连贯性
        × (1.0 + 0.1 × nn_confidence);                                  // NN 置信度 (v0.4.7)
```

`nn_confidence = 1.0 / (1.0 + avg_loss)`，未训练时 = 0，乘法因子 = 1.0，无影响。

### 上下文关联度

- 对输入逐字分词
- 在各子拓扑中精确匹配（`strcmp`）节点概念
- `ctx_activations[t]` = 匹配节点数 / 子拓扑总节点数

### 新颖性因子

`leaky integrator`：`recent_activation` 每节点激活+0.2，每轮×0.8 衰减
- 公式：`novelty = 1.0 / (1.0 + 10.0 × recent_activation)`
- 最近未用（≈0）→ novelty≈1.0；频繁用（≈1.0）→ novelty≈0.09

### Retry 循环

```c
typedef enum {
    RETRY_OK           = 0,   // 通过，无需修正
    RETRY_FROM_POOL    = 1,   // 从候选池重排（不重搜）
    RETRY_WITH_SEARCH  = 2,   // 缩域重搜（需重建 dialog_reasoning）
    RETRY_FAILED       = -1   // 已达上限，强制输出
} RetryStatus;
```

---

## 训练流程

### batch_learn 主循环

![训练工作流](docs/images/训练工作流.png)

位于 `src/tools/batch_learn.c`，OpenMP 并行训练：

```
[1/4] 加载拓扑
      master_load_state() / master_topology_create()

[2/4] 解析 QA 数据 (hermes_knowledge_base.json, 25MB)
      JSON 格式 → questions[] / answers[]

[3/4] OpenMP 并行学习（N 轮）
      for each epoch:
          #pragma omp parallel for
          for each QA pair:
              autonomic_learn_from_dialog(master, Q, A, &state)
          rebuild_cross_connections(master)  // epoch 结束后单线程执行

[4/4] 保存状态
      master_save_state(master, "pivotmind_state.dat")
      save_features(master, "features.bin")
      save_cross_edges(master, "cross_edges.bin")
```

**并行安全机制**：
- `huarong_net_add_connection()` 内部有 `net->mutex`（递归锁）保护
- 赫布学习用 thread-local buffer 收集激活对，barrier 后批量更新
- 边更新按哈希分片（`AUTONOMIC_SHARD_COUNT=16`），各 shard 独立 mutex
- 刷盘操作有 `flush_lock`（double-check 模式）

### 在线赫布学习

位于 `src/autonomic_learner.c`：

```c
void autonomic_learn_from_dialog(MasterTopology* master,
                                  const char* input, const char* output,
                                  AutonomicState* state,
                                  void* causal_graph,
                                  MemorySystem* memory) {
    // 1. 提取输入/输出中的不重复单字
    // 2. 查找或创建对应节点
    // 3. 同时激活 → 涨边置信度 / 建新边
    for each (input_node, output_node) pair:
        if edge exists:
            confidence += 0.05;         // 涨置信
        else:
            huarong_net_add_connection(... , 0.3);  // 新边初始0.3
}
```

### 并行训练合并（`merge_states.py`）

1. 把 QA 数据分成 N 份
2. 各机器从空拓扑（或种子拓扑）独立训练
3. 训练结束后用 `merge_states.py` 合并

合并策略：
- 按 `(topo_type, concept)` 合并节点（无视 node_id 不一致）
- 连接去重取均值，特征向量取均值
- 跨拓扑连接通过 concept 名重映射 node_id

---

## 状态持久化

### 文件格式演进

| 版本 | 格式变化 |
|------|---------|
| v1 | 基础节点+连接二进制 |
| v2 | 增加 topo_type 字段 |
| v3 | 增加 activation 字段 |
| v4 | 增加 features 特征向量（512 维） |
| v5 | 增加 sentinel(-1) + 魔数(0xDEADBEEF) 分隔节点区和跨连接区 |

### 当前格式（v5）

```
[int fmt_ver] [ver_len] [version_string]
[节点流]:
  [topo_type(int)] [node_id(int)] [concept_len(int)] [concept(char*)]
  [activation(float)] [feat_dim(int)] [features(float×512)]
  [conn_count(int)]
  [连接...]:
    [tgt_concept_len(int)] [tgt_concept(char*)] [weight(float)] [bias(float)] [confidence(float)]
  ...
  [sentinel(-1)]  ← 节点区结束标记
  [magic(0xDEADBEEF)] [link_count(int)]
[跨拓扑流]:
  [from_topo(int)] [from_node(int)] [to_topo(int)] [to_node(int)] [weight(float)] [use_count(int)]
  ...
```

### save/load 机制

- `master_save_state()`：先写临时文件 → rename 覆盖（原子写入，单文件无 .bak）
- `master_load_state()`：读取全部节点和连接，sentinel 检测 + 魔数验证 + 旧格式回退
- `save_features()`：独立存储 512 维节点特征向量到 `features.bin`
- `save_cross_edges()`：独立存储跨拓扑边到 `cross_edges.bin`
- 涌现式词类锚点中心保存到 `emergent_pos.bin`

---

## 关键算法

### 拓扑排序（Kahn 队列算法）

位于 `src/huarong_topology.c`：

```
1. 预建节点指针→索引查找表（二分查找 O(log N)）
2. 计算入度 O(N+E)
3. Kahn 算法 O(N+E)
   - 零入度节点入队
   - 出队→加入排序结果→邻居入度减1→入度=0则入队
```

性能提升：原实现 O(N²) → 优化后 O(N log N + E)

### 节点哈希（DJB2）

位于 `src/node_hash.c`：
```c
unsigned int hash_string(const char* str, int bucket_count) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % bucket_count;
}
```

默认桶数 1009（素数），链地址法处理冲突，平均 O(1) 查找。

### A* 因果路径搜索

位于 `src/causal_reasoning.c`：
```c
float causal_astar_search(CausalGraph* graph, int start, int goal) {
    // open_set 使用最小堆（priority_queue）
    // closed_set 防止重复访问
    // f_score = g_score + h_score
}
```

因果置信度计算（四因子加权）：
```c
confidence = base_score × 0.4
           + validation × 0.2
           + diversity × 0.2
           + stability × 0.2
```

---

## 代码组织

```
PivotMind/
├── src/                    # 核心源文件（86个 .c, ~48,600 行，编译零警告）
│   ├── huarong_topology.c             # 底层拓扑网络：节点/边/哈希/拓扑排序
│   ├── multi_topology.c               # 多拓扑管理：SubTopology/MasterTopology/走边
│   ├── cognitive_controller.c         # 认知调度中心：意图向量/retry/满意度
│   ├── dialog_system.c                # 对话管线：激活传播/联想记录
│   ├── dialog_generate.c              # 回复生成：走边→文本/记忆缓存
│   ├── dialog_system.h → intent 部分    # 意图识别：关键词→意图枚举
│   ├── diffusion.c                    # 多层扩散引擎（v0.4.7 虚词过滤）
│   ├── associative_reasoning.c        # 联想推理：激活扩散/递归传播
│   ├── autonomic_learner.c            # 自主学习：赫布/刷盘/边分片更新
│   ├── memory_system.c                # 三级记忆：STM/LTM/工作记忆
│   ├── causal_reasoning.c             # 因果推理：A*搜索/因果置信度
│   ├── feature_io.c                   # 特征向量持久化
│   ├── cross_edge_io.c                # 跨拓扑边持久化+重建
│   ├── node_hash.c                    # 节点哈希表（DJB2，链地址法）
│   ├── topology_growth.c              # 拓扑增长：密度检测/冷却触发
│   ├── catastrophic_forgetting.c      # 灾难性遗忘防护（EWC）
│   ├── memory_consolidation.c         # 记忆巩固：STM→LTM
│   ├── concept_abstraction.c          # 概念抽象（7层抽象层级）
│   ├── attention.c                    # 注意力机制（4种）
│   ├── thread_pool.c                  # 线程池
│   ├── metrics.c                      # 指标统计
│   ├── string_pool.c                  # 共享字符串池
│   ├── json_config.c                  # 运行时 JSON 配置加载
│   ├── chinese.c / vocab.c / utf8_tokenizer.c        # 中文+英文处理
│   │
│   ├── 脑区模块:
│   │   ├── prefrontal.c                  # 前额叶：对话策略 + ACC 自适应门控
│   │   ├── prefrontal_executive.c        # 前额叶执行器：PFE 6 模式推理
│   │   ├── hippocampus.c                 # 海马体：记忆巩固/重放
│   │   ├── dmn.c                         # 默认模式网络：梦境/闲暇
│   │   ├── amygdala.c                    # 杏仁核：情绪效价
│   │   ├── perception.c                  # 感知皮层：Sogou+Bing 联网搜索
│   │   ├── broca.c                       # 布罗卡区：模板生成
│   │   ├── cerebellum.c                  # 小脑：资源保护
│   │   ├── hypothalamus.c                # 下丘脑：需求驱动
│   │   ├── thalamus.c                    # 丘脑：信号总线 + 脑区启停
│   │   ├── brainstem.c                   # 脑干：昼夜节律 + 心跳循环
│   │   ├── cingulate.c                   # 扣带回：ACC 评估
│   │   ├── idea_arena.c                  # 想法竞技场：候选竞争
│   │   └── reticular.c                   # 网状激活系统：觉醒调节
│   │
│   ├── 涌现式词类:
│   │   └── emergent_pos.c                # 种子锚点 + 512 维特征聚类
│   │
│   ├── 学习器:
│   │   ├── active_learner.c              # 主动学习器（7×24 后台）
│   │   ├── self_learner.c                # 自我学习器（好奇驱动）
│   │   └── bptt_learner.c               # BPTT 学习器（RNN 反向传播）
│   │
│   ├── 神经网络子系统:
│   │   ├── tensor.c                      # 张量运算
│   │   ├── matrix_ops.c                  # 矩阵运算
│   │   ├── gradient_ops.c                # 梯度计算
│   │   ├── layer.c / layer_lstm.c / layer_gru.c / layer_rnn*.c
│   │   ├── model.c / model_io.c / generative_model.c
│   │   ├── trainer.c / optimizer.c / scheduler.c
│   │   ├── pretrain.c / feature_learn.c / feature_pretrain.c
│   │   ├── quantization.c / pruning.c
│   │   └── memory_arena.c / tensor_pool.c
│   │
│   └── ...                              # 其他辅助模块
│
├── include/                # 头文件（89个 .h, ~12,600 行）
│   ├── multi_topology.h              # MasterTopology / SubTopology / CrossTopologyLink
│   ├── huarong_topology.h            # ReasoningNode / HuarongTopologyNet
│   ├── cognitive_controller.h        # CognitiveController / intent_weights / retry
│   ├── dialog_system.h               # DialogSystem / DialogReasoning / IntentType
│   ├── memory_system.h               # MemoryEntry / STM / LTM
│   ├── causal_reasoning.h            # CausalGraph / 因果置信度
│   ├── emergent_pos.h                # POSAnchor / EmergentPOS
│   ├── json_config.h                 # ConfigContext 运行时配置
│   ├── bptt_learner.h                # BPTT 学习器
│   └── ...
│
├── tools/                  # 命令行工具（57 文件）
│   ├── batch_learn.c                 # 批量训练（OpenMP 并行，核心工具）
│   ├── seed_builder.c                # 种子拓扑构建（共现建边）
│   ├── corpus_train.c                # 语料训练
│   ├── template_build.c              # 模板构建工具
│   ├── convert_state.py              # 跨架构状态转换 (二进制 ↔ JSON)
│   └── merge_states.py               # 多机训练结果合并脚本
│
├── demos/                  # 演示程序
│   ├── pivotmind_gateway.c           # HTTP 网关（推荐入口）
│   └── digital_life.c                # 命令行交互演示
│
├── tests/                  # 测试
│   ├── unit/                         # 单元测试（17 项）
│   ├── regression/                   # 回归测试套件（32 项）+ 训练追踪
│   ├── integration/                  # 集成测试
│   └── test_pfe_unit.c               # PFE 专项测试（23 项）
│
├── scripts/                # 自动化脚本（12 文件）
├── changelogs/             # 改动记录（55 编号）
├── reports/                # 审查报告
├── docs/                   # 补充文档
├── data/                   # 运行时数据（hermes 知识库 25MB）
├── Makefile                # 构建系统（12 二进制 + 15 测试目标）
└── ARCHITECTURE.md         # 本文档
```

---

## 已修复的问题

| Bug | 修复方式 | 相关文件 |
|-----|---------|---------|
| 走边 O(n²) | 预建节点指针→索引二分查找表，拓扑排序 O(N²)→O(N log N+E) | huarong_topology.c |
| strstr 误匹配 | 改用 strcmp 精确匹配 | cognitive_controller.c |
| 跨拓扑冷启动 | 实现动态跨拓扑建边（CrossTopoHitRecord 跟踪） | multi_topology.c |
| 并发建边竞态 | net->mutex 递归锁保护 | huarong_topology.c |
| 刷盘数据丢失 | 原子写入（写临时文件→rename），撤销 .bak 双副本 | autonomic_learner.c |
| 循环激活栈溢出 | 递归传播加 recursion_depth 硬上限（MAX_RECURSION_DEPTH=1000） | associative_reasoning.c |
| 跨连接读写断裂 | sentinel(-1) + 魔数(0xDEADBEEF) 分隔节点区和跨连接区 | multi_topology.c |
| realloc 悬空指针 | 索引替代裸指针 + strdup 接管 + malloc+memcpy+free 替代链式 realloc | article_reader.c, huarong_topology.c 等 |
| double-free 退役竞态 | swap-before-retire + NULL-before-free 模式 | huarong_topology.c, node_cache.c |
| 扩散引擎虚词污染 | is_function_word() ~130 词三层过滤 | diffusion.c |
| strchr 多字符常量 bug | strchr → strstr（单字符→子串匹配） | json_config.c |
| 编译警告全项目 | 10 文件 14 处警告清零 (`-Wall -Wextra`) | 见 changelogs/054 |
| Windows localtime_r | `#ifdef _WIN32` → localtime_s | brainstem.c, error.c, perception.c |
| strncpy 截断 | → snprintf 安全替代 | web_fetch.c |

## 设计决策记录

| 决策 | 原因 |
|------|------|
| 逐字分词，不做词语拓扑 | 无词表边界，新组合自动适应；"学习"=「学」+「习」同时激活 |
| 拓扑联想为主，NN 层为辅 | BPTT 桥接提供 NN 反馈，拓扑走边仍是生成主路径 |
| 512 维特征向量（NODE_FEATURE_DIM） | 余弦相似度区分度好，128/256 维下锚点聚类效果不理想 |
| 二进制状态格式 | 加载/保存毫秒级，250MB 文件可秒读 |
| 边数不设上限 | 让拓扑自由生长，剪枝策略留给后续 |
| 并行用 OpenMP 而非 pthread | 代码侵入性低，适合 for 循环并行模式 |
| 锁策略：net->mutex 粗粒度 | 简单可靠，竞态比死锁更难调试 |
| 涌现而非编码 | POS 从种子涌现、跨拓扑从使用模式中自适应 |
| 语法连接词 POS 映射 | 按(prev_pos, curr_pos)词类对决定连接词，比硬编码轮换更准确 |

## 待优化方向

| 方向 | 目标 | 关键文件 |
|------|------|---------|
| 参数自动调优 | 用强化学习或贝叶斯优化调参 intent_weights | cognitive_controller.c |
| 训练效果追踪 | 固定测试集的量化回复质量指标 | tests/regression/train_track.py ✅ |
| 分布式多节点 | 丘脑信号总线延伸到跨机路由 | thalamus.c, multi_topology.c |
| 语音/图像拓扑 | 扩展到多模态（预留 thread_pool 框架） | multi_topology.c |
| FPGA 部署 | 硬件级神经形态计算 | roadmap |
| ~~跨架构状态格式~~ | ~~JSON/MessagePack~~ → **已实现**: `tools/convert_state.py` | feature_io.c, multi_topology.c |
