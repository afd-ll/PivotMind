# 0.2.7 — 走边推理增强里程碑

> **日期**: 2026-05-31 | **类型**: 新增

## 动机

v0.2.5 完成了跨拓扑锁安全、竞争队列热修复、认知调度中心等底层加固。但走边算法仍停留在"纯局部联想"阶段——每一步只看当前节点的出边邻居，缺乏全局目标引导。这导致路径"局部连贯但全局跑题"的结构性问题。

v0.2.6 的核心主题是：**让走边从"联想机器"向"推理机器"迈出关键一步**，同时加固并发安全和特征学习。

## 改动总览

| 类别 | 改动 | 关键文件 |
|------|------|----------|
| 🟢 推理增强 | walk_base_score 增加第7维目标引力（goal_gravity） | `src/multi_topology.c` |
| 🟢 学习增强 | 上下文敏感 Hebbian 更新（hebbian_update_contextual） | `include/common.h` |
| 🔵 并发安全 | master_add_cross_link 内部 wrlock 下重复检查（TOCTOU 防护） | `src/multi_topology.c` |
| 🔵 并发优化 | brainstem 衰减分段释放读锁（BATCH_SIZE=200） | `src/brainstem.c` |
| 🔵 代码清理 | topology_walk_beam 启用 query_anchor（移除 dead code） | `src/multi_topology.c` |

---

## 详细改动

### 1. walk_base_score 增加目标引力维（P0—核心改动）

**问题**：`walk_base_score` 是一个六维局部评分函数（边权重 + 边置信度 + 边偏置 + 节点激活 + 节点置信度 + 语义得分）。六个维度全部来自局部信息，没有任何一项回答"往哪里走更接近目标"。

**修复**：新增第七维 `goal_similarity`——候选节点特征向量与输入锚点（`query_anchor`）的余弦相似度。权重 `EDGE_WALK_W_GOAL_GRAVITY=0.12`。

```c
// 修改前（六维）
static inline float walk_base_score(
    float edge_weight, float edge_conf, float edge_bias,
    float node_act, float node_conf, float semantic_score,
    float semantic_weight)
{
    return EDGE_WALK_W_WEIGHT     * clamp(edge_weight, 0.0f, 1.0f) +
           EDGE_WALK_W_CONF       * clamp(edge_conf,   0.0f, 1.0f) +
           EDGE_WALK_W_BIAS       * clamp(edge_bias,   0.0f, 1.0f) +
           EDGE_WALK_W_ACTIVATION * clamp(node_act,    0.0f, 1.0f) +
           EDGE_WALK_W_NODE_CONF  * clamp(node_conf,   0.0f, 1.0f) +
           semantic_weight * (semantic_score + 1.0f) * 0.5f;
}

// 修改后（七维）
static inline float walk_base_score(
    float edge_weight, float edge_conf, float edge_bias,
    float node_act, float node_conf, float semantic_score,
    float semantic_weight,
    float goal_similarity)   // ← 新增
{
    return EDGE_WALK_W_WEIGHT     * clamp(edge_weight, 0.0f, 1.0f) +
           // ... 六维同上 ...
           EDGE_WALK_W_GOAL_GRAVITY * clamp(goal_similarity, -1.0f, 1.0f);
}
```

**影响范围**：所有走边算法——`topology_walk_greedy`、`topology_walk_cross`（同拓扑/跨拓扑）、`topology_walk_beam`——共 5 个调用点全部更新。

**效果**：走边路径会被引力牵引向输入主题语义区。对于解释性问题（如"为什么天是蓝的"），路径不再仅仅围绕"天"和"蓝"做词汇联想，而是偏向因果、物理相关节点。

---

### 2. 上下文敏感 Hebbian 更新（P2）

**问题**：`hebbian_update` 是 pair-wise 操作——两个共激活节点互相拉近特征向量。但"苹果"在"我吃了一个苹果"和"苹果发布了新手机"中应该是不同的语义倾向，静态向量无法编码一词多义。

**修复**：新增 `hebbian_update_contextual`，在双向拉近的同时引入上下文引力——两个节点都向路径上下文均值微移。

```c
static inline void hebbian_update_contextual(float* a, float* b, int dim,
                                              float lr,
                                              const float* context_mean,
                                              float context_strength) {
    if (!a || !b || dim <= 0) return;
    for (int i = 0; i < dim; i++) {
        float diff = b[i] - a[i];
        a[i] += lr * diff;
        b[i] -= lr * diff;
        if (context_mean) {
            a[i] += lr * context_strength * (context_mean[i] - a[i]);
            b[i] += lr * context_strength * (context_mean[i] - b[i]);
        }
    }
}
```

**设计决策**：不改特征维度（保持 512 维），不改变 `hebbian_update` 接口（向后兼容），新增独立函数供调用方选择性使用。`context_strength` 建议 0.1~0.2，`context_mean=NULL` 时退化为普通 Hebbian。

---

### 3. master_add_cross_link TOCTOU 防护（P3）

**问题**：`master_process_cross_hits` 在无锁状态下调用 `cross_link_exists` 检查，然后调用 `master_add_cross_link`（内部获取 wrlock）。在检查与获取锁之间存在时间窗口，另一个线程可能在此期间创建了同一条跨拓扑边。

**修复**：`master_add_cross_link` 内部在获取 wrlock 后再次检查 `cross_link_exists`，若已存在则返回已有连接的 link_id，不再重复创建。

```c
pthread_rwlock_wrlock(&master->rwlock);
// TOCTOU 防护：wrlock 下再次检查是否已存在
if (cross_link_exists(master, from_topo_id, from_node_id,
                      to_topo_id, to_node_id)) {
    // 返回已有连接的 link_id
    for (int i = 0; i < master->cross_link_count; i++) {
        // ... 查找并返回
    }
}
```

**影响**：消除了并发场景下跨拓扑边重复创建的可能性，但生产环境中该窗口极窄（hit_count 阈值通常 >= 3），属于预防性加固。

---

### 4. brainstem 衰减分段释放读锁（P4）

**问题**：`brainstem_tick_decay_spontaneous` 在处理衰减时一次性持有 `master->rwlock` 读锁。当节点总数达到 10 万+ 时，5% 采样 = 5000 个节点，持锁时间可能达到毫秒级。在 `pthread_rwlock` 实现中，持续读锁会饿死写锁——gateway 学习请求、跨拓扑建边等写入操作会被无限期阻塞。

**修复**：衰减操作拆分为每批 200 个节点（`DECAY_BATCH_SIZE=200`），每批之间释放读锁，给写锁申请者留出窗口。

```c
#define DECAY_BATCH_SIZE 200
while (decayed_total < sample_count) {
    int batch = min(sample_count - decayed_total, DECAY_BATCH_SIZE);
    pthread_rwlock_rdlock(&master->rwlock);
    // ... 处理 batch 个节点 ...
    pthread_rwlock_unlock(&master->rwlock);
    // 隐式 yield：下一轮 rdlock 前写锁有机会进入
}
```

自发激活（数量极少，通常 < 10 个节点/次）仍一次性执行，不拆分。

**注意**：此改动仅在节点数极大（> 5 万）且 gateway 并发写入频繁时才有效果提升。在此之前属于"幸福烦恼"预防性优化。

---

### 5. 代码清理：beam_walk 启用 query_anchor

移除 `topology_walk_beam` 中的 `(void)query_anchor` dead code，启用目标引力计算。beam search 现在与 greedy walk 和 cross walk 一致，在候选评分中纳入输入锚点对齐。

---

## 设计讨论记录

本次迭代中讨论但**未实施**的方案：

| 方向 | 决定 | 理由 |
|------|------|------|
| 模板系统 POS 槽位化（P1） | 暂缓 | 涉及模板拓扑结构变更，需独立版本 |
| 情感拓扑参与走边（P5） | 暂缓 | 需先验证情绪拓扑连接质量（同效价 vs 跨效价边权比），存在噪声注入风险 |
| 节点级锁替代为原子操作 | 暂缓 | 当前 mutex 方案简单可调试，等待实际瓶颈出现 |

---

## 影响评估

| 维度 | 评估 |
|------|------|
| 生成质量 | **提升** — 走边路径向输入主题收敛，减少"跑题" |
| 特征学习 | **增强** — 上下文敏感 Hebbian 为多义消歧提供基础 |
| 并发安全 | **加固** — TOCTOU 防护 + 读锁分段释放 |
| 兼容性 | **完全向后兼容** — 无 API 变更，无序列化格式变更 |
| 性能 | 走边每步多 1 次 cosine_similarity（512 维），在 RK3399 上约 +5% 开销，可接受 |
