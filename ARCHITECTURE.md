# 玄枢 PivotMind 架构文档

## 整体架构

![架构总览图](docs/images/架构总览图.png)

### 架构分层

| 层级 | 职责 | 核心文件 |
|------|------|---------|
| **多拓扑网络层** | 9层子拓扑接收输入，逐字分词后激活对应节点，跨拓扑传播 | `huarong_topology.c`, `multi_topology.c` |
| **认知调度层** | 意图向量计算、满意度评估、retry 循环、在线学习调整 | `cognitive_controller.c` |
| **对话学习层** | 联想推理、回复生成、赫布在线学习、状态持久化 | `dialog_system.c`, `autonomic_learner.c` |

## 三大核心模块

![核心模块流程图](docs/images/核心模块流程图.png)

### 1. 拓扑网络层

**核心文件**: `src/huarong_topology.c`, `src/multi_topology.c`, `include/huarong_topology.h`, `include/multi_topology.h`

**职责**: 节点/边管理、跨拓扑连接、激活传播、拓扑排序

**关键 API**:
- `huarong_net_find_or_create_node()` — 查找或创建节点
- `topology_walk_greedy()` — 贪心走边
- `master_add_cross_link()` — 添加跨拓扑连接

### 2. 认知调度层

**核心文件**: `src/cognitive_controller.c`, `include/cognitive_controller.h`

**职责**: 意图向量计算、满意度评估、retry 循环、在线学习

**关键 API**:
- `cognitive_controller_compute_intent()` — 计算意图向量
- `cognitive_controller_satisfy()` — 评估满意度

### 3. 对话学习层

**核心文件**: `src/dialog_system.c`, `src/autonomic_learner.c`, `src/dialog_generate.c`

**职责**: 分词解析、联想推理、回复生成、赫布学习

**关键 API**:
- `dialog_input_create()` — 创建对话输入
- `autonomic_learn_from_dialog()` — 在线学习

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
} SubTopology;
```

### ReasoningNode（推理节点）

```c
typedef struct ReasoningNode {
    int node_id;                            // 节点唯一标识
    char* concept;                          // 概念名称/字符
    float* features;                        // 24维语义向量 (NODE_FEATURE_DIM=24)
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
} ReasoningNode;
```

### CrossTopologyLink（跨拓扑连接）

```c
typedef struct CrossTopologyLink {
    int link_id;
    int from_topo_id, from_node_id;         // 源拓扑+节点
    int to_topo_id, to_node_id;             // 目标拓扑+节点
    float weight;
    const char* relation;                   // 关系类型
    int bidirectional;                      // 是否双向
    float transfer_rate;                    // 跨拓扑激活传递率
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

跨拓扑连接在训练后通过 `rebuild_cross_connections()` 批量重建，基于节点特征（24维语义向量）的余弦相似度。动态新建跨拓扑连接通过 `CrossTopoHitRecord` 跟踪。

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

激活传递公式：`new_activation = activation × link->weight × link->transfer_rate × DECAY_RATE`

---

## 联想推理引擎

### 激活扩散机制

位于 `src/dialog_system.c` 的 `dialog_topo_worker()` 实现并行拓扑传播：

```c
for (int n = 0; n < sub->net->node_count; n++) {
    ReasoningNode* node = sub->net->nodes[n];
    if (node->activation < 0.15f) continue;

    for (int c = 0; c < node->connection_count; c++) {
        ReasoningNode* connected = node->connections[c];
        float new_activation =
            node->connection_weights[c]     // 边权重
            × node->activation              // 源激活
            × confidence_factor             // 置信度因子
            × activation_multiplier         // 低置信放大(1.3x) / 高置信抑制(0.7x)
            × embed_factor                  // 特征向量余弦相似度
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
                         float intent_weight);
```

**评分公式（五维加法混合）**:
```
score = 0.28 × edge_weight
      + 0.22 × edge_confidence
      + 0.11 × edge_motivational_bias
      + 0.28 × target_activation
      + 0.11 × target_confidence
最终 × (1.0 + 0.6 × target_valence)
```

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

## 对话系统

### 两层意图识别

位于 `src/dialog_intent.c`：

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

### 实体提取

位于 `src/dialog_entities.c`：
```c
typedef struct {
    char* text;             // 原文
    char* normalized;       // 归一化
    EntityType type;        // OBJECT/ACTION/ATTRIBUTE/CONCEPT/CAUSAL
    float confidence;
    int start_pos, end_pos;
} DialogEntity;
```

### 回复生成流程（`src/dialog_generate.c`）

1. **精确匹配检查**：`memory_retrieve(memory, "response:{完整输入}")` → 命中则直接返回
2. **拓扑驱动生成**：`master_generate_response()` → 跨拓扑走边 → 生成概念序列
3. **联想兜底**：从 top-5 激活节点出发，`topology_walk_greedy()` 生成回复
4. **自动学习**：`autonomic_learn_from_dialog(input, output)`

---

## 认知调度中心

### 意图向量计算

三因子融合公式：
```c
final_weight[t] = base_weight
    × (1.0 + context_bias × ctx_activations[t])     // 上下文关联度
    × (1.0 + novelty_bias × novelty_factors[t])      // 新颖性（刚用过的拓扑降权）
    × learned_base[t];                                // 在线学习调整因子
```

### 上下文关联度

- 对输入逐字分词
- 在各子拓扑中精确匹配（`strcmp`）节点概念
- `ctx_activations[t]` = 匹配节点数 / 子拓扑总节点数

### 新颖性因子

`leaky integrator`：`recent_activation` 每节点激活+0.2，每轮×0.8衰减
- 公式：`novelty = 1.0 / (1.0 + 10.0 × recent_activation)`
- 最近未用（≈0）→ novelty≈1.0；频繁用（≈1.0）→ novelty≈0.09

### 在线学习调整

- 正反馈（满意）→ 对应拓扑的 learned_base 上升
- 负反馈（不满意）→ 对应拓扑的 learned_base 下降

### Retry 循环

```c
typedef enum {
    RETRY_OK           = 0,   // 通过，无需修正
    RETRY_FROM_POOL    = 1,   // 从候选池重排（不重搜）
    RETRY_WITH_SEARCH  = 2,   // 缩域重搜（需重建 dialog_reasoning）
    RETRY_FAILED       = -1   // 已达上限，强制输出
} RetryStatus;

// satisfaction < threshold → retry_count++
// retry_count ≥ max_retry → RETRY_FAILED
```

---

## 训练流程

### batch_learn 主循环

![训练工作流](docs/images/训练工作流.png)

位于 `src/tools/batch_learn.c`，OpenMP 并行训练：

```
[1/4] 加载拓扑
      master_load_state() / master_create()

[2/4] 解析 QA 数据
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
                                  AutonomicState* state) {
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
| v4 | 增加 features 特征向量（24维） |
| v5 | 增加 sentinel(-1) + 魔数(0xDEADBEEF) 分隔节点区和跨连接区 |

### 当前格式（v5）

```
[int fmt_ver] [ver_len] [version_string]
[节点流]:
  [topo_type(int)] [node_id(int)] [concept_len(int)] [concept(char*)]
  [activation(float)] [feat_dim(int)] [features(float×24)]
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

- `master_save_state()`：先写临时文件 → rename 覆盖（原子写入）
- `master_load_state()`：读取全部节点和连接，sentinel 检测 + 魔数验证 + 旧格式回退
- `save_features()`：独立存储节点特征向量到 `features.bin`
- `save_cross_edges()`：独立存储跨拓扑边到 `cross_edges.bin`
- 备份机制：写盘前先 rename 原文件为 `.bak`

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

位于 `src/causal_reasoning.c`，用于因果推理：
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
├── src/                    # 核心源文件（57个 .c）
│   ├── huarong_topology.c           # 底层拓扑网络：节点/边/哈希/拓扑排序
│   ├── multi_topology.c             # 多拓扑管理：SubTopology/MasterTopology/走边
│   ├── cognitive_controller.c       # 认知调度中心：意图向量/retry/满意度
│   ├── dialog_system.c              # 对话管线：激活传播/联想记录
│   ├── dialog_intent.c              # 意图识别：关键词→意图枚举
│   ├── dialog_generate.c            # 回复生成：走边→文本/记忆缓存
│   ├── dialog_semantic.c            # 语义引擎
│   ├── dialog_verify.c              # 回复验证
│   ├── associative_reasoning.c      # 联想推理：激活扩散/递归传播
│   ├── autonomic_learner.c          # 自主学习：赫布/刷盘/边分片更新
│   ├── memory_system.c              # 三级记忆：STM/LTM/工作记忆
│   ├── causal_reasoning.c           # 因果推理：A*搜索/因果置信度
│   ├── feature_io.c                 # 特征向量持久化
│   ├── cross_edge_io.c              # 跨拓扑边持久化
│   ├── node_hash.c                  # 节点哈希表（DJB2，链地址法）
│   ├── topology_growth.c            # 拓扑增长：密度检测/冷却触发
│   ├── catastrophic_forgetting.c    # 灾难性遗忘防护
│   ├── memory_consolidation.c       # 记忆巩固：STM→LTM
│   ├── concept_abstraction.c        # 概念抽象
│   ├── concept_processor.c          # 概念处理
│   ├── enhanced_generator.c         # 增强生成
│   ├── attention.c                  # 注意力机制
│   ├── tensor.c / tensor_pool.c     # 张量计算
│   ├── layer.c / layer_gru.c / layer_lstm.c / layer_rnn.c  # 神经网络层
│   ├── model.c / model_io.c         # 模型管理
│   ├── pretrain.c / trainer.c       # 预训练
│   ├── optimizer.c / scheduler.c    # 优化器
│   ├── pruning.c / quantization.c   # 裁剪/量化
│   ├── network_tool.c               # 网络工具
│   ├── thread_pool.c                # 线程池
│   ├── metrics.c                    # 指标统计
│   ├── gradient_ops.c / matrix_ops.c / string_pool.c  # 基础运算
│   ├── chinese.c / vocab.c / utf8_tokenizer.c         # 中文处理
│   └── ...                          # 其他辅助模块
│
├── include/                # 头文件（29个 .h）
│   ├── multi_topology.h          # MasterTopology / SubTopology / CrossTopologyLink
│   ├── huarong_topology.h        # ReasoningNode / HuarongTopologyNet
│   ├── cognitive_controller.h    # CognitiveController / intent_weights / retry
│   ├── dialog_system.h           # DialogSystem / DialogReasoning / IntentType
│   ├── memory_system.h           # MemoryEntry / STM / LTM
│   ├── causal_reasoning.h        # CausalGraph / 因果置信度
│   ├── cognitive_params.h        # CognitiveConfidence / EdgeWeightDual
│   ├── common.h                  # 全局常量/宏
│   └── ...
│
├── tools/                  # 命令行工具
│   ├── batch_learn.c              # 批量训练（OpenMP 并行，核心工具）
│   ├── batch_learn_lowmem.c       # 低内存版（禁用跨拓扑重建）
│   ├── seed_builder.c             # 种子拓扑构建（共现建边）
│   ├── corpus_train.c             # 语料训练
│   ├── test_dialog.c              # 对话测试工具
│   └── merge_states.py            # 多机训练结果合并脚本
│
├── demos/                  # 演示程序
│   └── digital_life.c             # 交互对话演示
│
├── data/                   # 语料和 QA 数据
├── tests/                  # 测试用例
├── docs/                   # 补充文档
├── changelogs/             # 改动记录
├── Makefile                # 构建系统
└── ARCHITECTURE.md         # 本文档
```

---

## 已修复的问题

| Bug | 修复方式 | 相关文件 |
|-----|---------|---------|
| 走边 O(n²) | 预建节点指针→索引二分查找表，拓扑排序 O(N²)→O(N log N+E) | huarong_topology.c |
| strstr 误匹配 | 循环变量未使用 + 包含匹配导致"人"匹配"人民"→改用 strcmp 精确匹配 | cognitive_controller.c |
| 跨拓扑冷启动 | 实现动态跨拓扑建边（CrossTopoHitRecord 跟踪） | multi_topology.c |
| 并发建边竞态 | huarong_net_add_connection 内部加 net->mutex 递归锁 | huarong_topology.c |
| 刷盘数据丢失 | 先备份 .bak 再 rename 覆盖（原子写入） | autonomic_learner.c |
| 循环激活栈溢出 | 递归传播加 recursion_depth 硬上限（MAX_RECURSION_DEPTH=1000） | associative_reasoning.c |
| 跨连接读写断裂 | sentinel(-1) + 魔数(0xDEADBEEF) 分隔节点区和跨连接区 | multi_topology.c |

## 设计决策记录

| 决策 | 原因 |
|------|------|
| 逐字分词，不做词语拓扑 | 无词表边界，新组合自动适应；"学习"=「学」+「习」同时激活 |
| 不引入神经网络层 | 验证拓扑联想路线，NN 层源码保留在磁盘 |
| 二进制状态格式 | 加载/保存毫秒级，250MB 文件可秒读 |
| 边数不设上限 | 让拓扑自由生长，剪枝策略留给后续 |
| 并行用 OpenMP 而非 pthread | 代码侵入性低，适合 for 循环并行模式 |
| 锁策略：net->mutex 粗粒度 | 简单可靠，竞态比死锁更难调试 |

## 待优化方向

| 方向 | 目标 | 关键文件 |
|------|------|---------|
| 拓扑剪枝 | 置信度低于阈值或热度耗尽的边自动删除 | catastrophic_forgetting.c |
| 动态图索引 | 用邻接矩阵替代邻接表，加速高度数节点走边 | multi_topology.c |
| 参数自动调优 | 用强化学习或贝叶斯优化调参 intent_weights | cognitive_controller.c |
| 文本格式持久化 | 迁移到 JSON/MessagePack，支持跨架构互换 | feature_io.c |
| 语音/图像拓扑 | 扩展到多模态（预留 thread_pool 框架） | multi_topology.c |
