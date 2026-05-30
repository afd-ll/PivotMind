# 010A — 核心管线代码审查

> 日期: 2026-05-26 | 审查: 010A | 关联: 009B

## 审查范围

| 文件 | 行数 | 审查重点 |
|------|------|----------|
| `src/autonomic_learner.c` | 631 | 同时激活检测、边置信度涨落、衰减机制、刷盘 |
| `src/active_learner.c` | 952 | learn_from_dialog 拆分、feedback_correct、后台线程 |
| `src/cognitive_controller.c` | 1130 | 意图向量、causal_path_score、evaluate_draft、retry |
| `src/dialog_generate.c` | 389 | 走边循环、因果筛选、global_visited、记忆回退 |
| `src/dialog_system.c` | - | retry 循环整合、autonomic_learn 调用 |
| `src/multi_topology.c` | 2317 | topology_walk_greedy、master_generate_response |
| `src/associative_reasoning.c` | 429 | generate_from_associations、跨拓扑走边 |

---

## P0 — 逻辑缺陷

### P0-1 — feedback_correct 未操作拓扑边权重

**位置**: `src/active_learner.c:860-864`

```c
void feedback_correct(ActiveLearner* learner, const char* user_input,
                     const char* ai_response, const char* user_feedback) {
    learn_from_dialog(learner, user_input, ai_response, user_feedback);
}
```

`learn_from_dialog` (L872-952) 只在 MemorySystem 层面操作：
- `memory_store` / `memory_retrieve` / `memory_update_confidence`（记忆系统）
- 第937-949行遍历拓扑节点做 `strstr` 匹配然后调用 `update_node_from_feedback`

但 **这条路径从不触碰边的 `connection_confidences`**。ARCHITECTURE.md §二 明确要求："用户说不对 → 显式压下某条边的置信度"。当前实现仅在记忆层面调整 importance，没有修改拓扑内的边置信度。

**建议**: `feedback_correct` 应接收上一轮的 `last_path_edge_ids`（`dialog_generate.c` 已记录在 `dsys->last_path_edge_ids`），对走边路径上的边直接下调置信度。

### P0-2 — dialog_generate 中 global_visited 先使用后分配

**位置**: `src/dialog_generate.c:89-124`

```c
// L89: 先检查 global_visited
if (global_visited) {
    if (start_id < global_bm_size * 8 &&
        (global_visited[start_id / 8] & (unsigned char)(1 << (start_id % 8))))
        continue;
}

// ... 走边 ...

// L114-124: 才分配 global_visited
if (!global_visited && sub->net->node_count > 0) {
    global_bm_size = (sub->net->node_count + 7) / 8;
    global_visited = (unsigned char*)calloc(global_bm_size, 1);
    ...
}
```

第一轮循环 `global_visited == NULL`，跳过检查 → 走边 → 进入分配分支 → 分配后标记已走过的起点。第二轮循环开始 `global_visited != NULL`，但之前已经走过的起点并未在 bitmap 中标记（分配时只标记了 `start_i < current` 的起点，但走边过程中走过的节点没有标记）。

**后果**: 跨起点走边时可能重复访问同一节点。

**建议**: 将 `global_visited` 分配提前到走边循环之前（第75行之前），分配后在每次走边后更新 visited bitmap。

### P0-3 — autonomic_decay_all 是均匀衰减，非竞争衰减

**位置**: `src/autonomic_learner.c:496-517`

```c
void autonomic_decay_all(MasterTopology* master) {
    for (int t = 0; t < master->sub_topo_count; t++) {
        for (int n = 0; n < sub->net->node_count; n++) {
            for (int e = 0; e < node->connection_count; e++) {
                node->connection_confidences[e] *= AUTONOMIC_DECAY_RATE;
                ...
            }
        }
    }
}
```

所有边乘以统一的 `AUTONOMIC_DECAY_RATE`，不区分"被激活过"与"未被激活"的边。

ARCHITECTURE.md §一 & §五 设计的是：
- 被激活的边：涨置信度（已实现，`boost_connection`）
- 同源竞争边中未被激活的：**额外衰减**
- 其他边：缓慢自然衰减

当前代码做了前两步但第三步"同源竞争"缺失。`g_activated` 数组已经记录了本轮激活的边（`record_edge_activated`），但 `autonomic_decay_all` 没有使用这个信息。`g_activated` 用 `__thread` 修饰且仅在同一函数调用周期（`autonomic_learn_from_dialog`）内有效，衰减调用时已不可见。

**建议**: 
1. 在 `AutonomicState` 中持久化记录"本轮激活边集合"
2. `autonomic_decay_all` 遍历时区分三种衰减率：激活边(0%)、竞争边(×0.85)、无关边(×0.95)

---

## P1 — 重要问题

### P1-1 — evaluate_draft 的 drive_score 与当前路径无关

**位置**: `src/cognitive_controller.c:633-649`

```c
// 从情绪拓扑取效价作为 baseline
SubTopology* emotion = master_get_sub_topology_by_type(cc->master, TOPO_EMOTION);
if (emotion && emotion->net && emotion->net->node_count > 0) {
    float sum = 0.0f;
    for (int n = 0; n < emotion->net->node_count; n++) {
        ReasoningNode* node = emotion->net->nodes[n];
        if (node) sum += node->valence;
    }
    drive_score = (sum / emotion->net->node_count + 1.0f) / 2.0f;
}
```

这段代码读取整个情绪拓扑的**全局平均效价**，与当前被评估的 `draft` 路径完全无关。无论评估哪条路径，`drive_score` 都相同。函数注释说"询问路径是否呼应认知状态"，但实现只读了全局常量。

**建议**: 应计算 `draft` 路径上各节点的效价平均值，或与当前 `context_valence`（概念情绪基调）做匹配。

### P1-2 — causal_path_score 的 node_mapping 反向查找 O(n²)

**位置**: `src/cognitive_controller.c:473-477`

```c
int cg_cause = -1, cg_effect = -1;
for (int c = 0; c < cg->node_count; c++) {
    if (cg->node_mapping[c] == from_id) cg_cause = c;
    if (cg->node_mapping[c] == to_id) cg_effect = c;
}
```

对路径上每对相邻节点都线性扫描整个因果图节点表。路径长度 × 因果图节点数 = 可能数千次比较。

**建议**: 在 `CognitiveController` 中维护一个反向映射 `topo_to_cg[id] → cg_id`（数组，O(1)查找），或在 `CausalGraph` 中提供此映射。

### P1-3 — learn_from_dialog 的 strstr 全拓扑扫描

**位置**: `src/active_learner.c:937-949`

```c
for (int t = 0; t < learner->master->sub_topo_count; t++) {
    for (int n = 0; n < sub->net->node_count; n++) {
        if (node->concept && strstr(node->concept, user_input)) {
            update_node_from_feedback(node, feedback_valence, is_correct);
        }
    }
}
```

遍历全部子拓扑的全部节点，用 `strstr` 模糊匹配。当拓扑有数百万节点时（README 说 ~308 万内部边），每次纠正都做全量扫描代价极高。

**建议**: 利用 `AutonomicState` 或 `last_path_node_ids` 精确定位需要纠正的节点，而非全拓扑扫描。

### P1-4 — dialog_generate 走边后未调用 evaluate_draft

**位置**: `src/dialog_generate.c:103-143` vs `src/dialog_system.c:1661`

`dialog_generate.c` 的走边循环在第103行 `if (path_len <= 1) continue` 后做了因果筛选（L106-112），但**没有调用 `evaluate_draft` 进行内感受评估**。

对比 `dialog_system.c` 的 retry 循环，在第1661行有：
```c
float satisfaction = evaluate_draft(sys->controller, &draft, draft.length);
```

这意味着从 `dialog_generate` 生成的回复跳过了三级 retry 降级策略（候选池重排 → 缩域重搜 → 强制输出）。如果走边路径质量差，没有纠正机会。

**建议**: `dialog_generate` 在输出前调用 `evaluate_draft`，不达标时尝试下一个起点。或者统一两个生成入口，消除代码重复。

---

## P2 — 改善建议

### P2-1 — boost_connection 的 O(n²) 全连接不编码字序

**位置**: `src/autonomic_learner.c:378-404`

```c
// 输入字内部的共现边（词语内部连接）
for (int i = 0; i < input_count; i++) {
    for (int j = i + 1; j < input_count; j++) {
        boost_connection(vocab, input_nodes[i], input_nodes[j], state);
    }
}
```

输入"我爱中国"产生 {我↔爱, 我↔中, 我↔国, 爱↔中, 爱↔国, 中↔国} 六条边，输入"国中爱我"产生完全相同的边集。字序信息完全丢失。

ARCHITECTURE.md §六 标记为待改为"字序关系编码"，当前仍是共现统计。

**建议**: 对相邻字对（i, i+1）使用更高的初始权重（如 0.5），非相邻字对（i, i+k）使用递减权重（如 0.3 / k）。在走边时，adjacent pairs 天然获得更高的被选概率，使走边路径更连贯。

### P2-2 — dialog_generate / dialog_system / associative_reasoning 三处重复走边逻辑

| 位置 | 函数 | 走边方式 |
|------|------|----------|
| `dialog_generate.c:49` | `master_generate_response → generate_from_associations` | 跨拓扑走边 |
| `dialog_generate.c:98` | 直接调 `topology_walk_greedy` | 本拓扑走边 |
| `dialog_system.c:830` | `master_generate_response → generate_from_associations` | 跨拓扑走边 |
| `dialog_system.c:879` | 直接调 `topology_walk_greedy` | 本拓扑走边 |
| `associative_reasoning.c:343` | `topology_walk_cross` | 跨拓扑走边 |

五处调用，三种组合（`master_generate_response` 包装、直接 greedy、跨拓扑 cross），参数传递方式和因果筛选逻辑不一致。

**建议**: 提取 `dialog_generate` 为唯一生成入口，`associative_reasoning` 和 `dialog_system` 的 retry 循环都走同一个路径。

### P2-3 — constants.h 中魔数的组织

**位置**: `include/constants.h`

审查中发现的魔数散落：
- `src/autonomic_learner.c:119` — `flush_threshold = 50`
- `src/active_learner.c:48` — `learning_interval = 300`
- `src/dialog_generate.c:74` — `max_path = 20`
- `src/cognitive_controller.c:568` — 概念层次跳跃阈值 `gap > 3`
- `src/multi_topology.c:1205-1209` — 动态剪枝阈值（硬编码阶梯）

部分已有宏定义（如 `PM_CHARS_PER_TEXT`），部分仍是硬编码。建议将行为参数统一到 `constants.h` 或 `cognitive_params.h`。

### P2-4 — reader.c 未被标记为废弃

**位置**: `tools/reader.c`

ARCHITECTURE.md Phase 3 计划将 reader 的共现建边逻辑内化到 `autonomic_learner`，但 `tools/reader.c` 仍在工具目录且无废弃注释。Phase 3 也未开始实施。

**建议**: 如果短期不实施，在 reader.c 头注释中标注状态（"功能已由 autonomic_learner.c 部分取代，保留供离线语料导入"）。

### P2-5 — seq2seq 清理残留验证

**位置**: 全项目

changelog 008B/009B 标记 `seq2seq.c` 和 `seq2seq.h` 已删除。确认文件系统无残留（`file_search("seq2seq")` 仅返回 changelogs 引用）。`dialog_system.h` 已移除 `#include "seq2seq.h"` 和 `Seq2Seq*` 字段，`dialog_generate.c` 已移除引用。

✅ 清理完成，无残留。

---

## 架构层面评估

### 已落地 ✅

| 设计 | 实现 |
|------|------|
| 同时激活 → 涨置信度 | `autonomic_learner.c:boost_connection` |
| 新边自动创建 | `boost_connection → huarong_net_add_connection` |
| 全局衰减 | `autonomic_decay_all`（均匀衰减，非竞争） |
| 刷盘持久化 | `autonomic_request_flush` + 先备份再覆写 |
| 跨拓扑传播 | `autonomic_learn_from_dialog` 核心4-5 |
| 效价匹配（情感基调） | `topology_walk_greedy:context_valence` EMA |
| 因果筛选 | `causal_path_score` + `dialog_generate` 阈值 0.25 |
| 自矛盾检测 | `self_contradiction_check`（情绪 + 概念层次） |
| 意图向量 | `compute_intent` 三因子融合 |
| 三级 retry | `dialog_system.c` do/while 循环 |
| 非自主纠偏接口 | `feedback_correct`（仅记忆层面） |

### 未落地 ❌

| 设计 | 现状 |
|------|------|
| 竞争衰退 | 均匀衰减，无差异化 |
| 边训练改进（字序编码） | 仍是共现 O(n²) 全连接 |
| reader 内化 (Phase 3) | 未开始 |
| feedback_correct 操作拓扑边权重 | 仅操作记忆系统 |
| BPTT 集成到生成管线 | `layer_rnn_backward.c` 独立存在，未接入 |

---

## 汇总

| 等级 | 数量 | 关键项 |
|------|------|--------|
| P0 | 3 | feedback_correct 未触边权重、global_visited 先使用后分配、竞争衰减缺失 |
| P1 | 4 | drive_score 与路径无关、causal 反向查找 O(n²)、全拓扑扫描、dialog_generate 缺 evaluate |
| P2 | 5 | 字序不编码、走边逻辑重复、魔数散落、reader 未标记废弃、seq2seq 已清理 |

## 关联

- 审查: 010A
- 修复: 010B（待实施）
- 前序: 009B
