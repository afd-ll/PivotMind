# 009B — 效价-因果-内感受三层分离实施

> 日期: 2026-05-26 | 审查: 009A | 修复: 009B | 关联: 008B

## 改动

### 1. multi_topology.c — 情感基调约束（效价匹配）

`topology_walk_greedy` 新增 `context_valence` 变量，EMA 累积路径情感色彩。

**初始化**（路径起点）:
```c
float context_valence = start_node_ptr ? start_node_ptr->valence : 0.0f;
```

**候选评分替换**（原：纯节点效价 → 现：情境匹配）:
```c
// 原: valence_mod = 1.0f + 0.6f * raw_val;  // "越正向越好"
// 新:
float valence_match = 1.0f - fabsf(context_valence - raw_val) * 0.5f;
float valence_mod   = 0.5f + 0.5f * valence_match;  // "跟基调一致就好"
```

**每步更新**（EMA α=0.3）:
```c
context_valence = context_valence * 0.7f + stepped_node->valence * 0.3f;
```

效果：路径情感基调自然形成，正效价路径抑制负效价候选，反之亦然。

---

### 2. cognitive_controller.c — 因果从评价移到筛选

`causal_consistency_check(cc, draft)` → `causal_path_score(cc, sub, node_ids, path_len)`

| 变化 | 原因 |
|------|------|
| 参数从 PathResult 改为裸数组 | 中游调用者只有 node_ids/path_len，没有 PathResult |
| 从 evaluate 内部调用移除 | 因果属于"探索约束"非"效价检验" |
| 改为 public API | dialog_generate 需要直接调用 |

---

### 3. cognitive_controller.c — evaluate_draft 精简

五维加权 → 三维+惩罚：
```
原: 效价20% + 连贯25% + 激活20% + 因果25% + 语义10%
新: 质量40% + 连贯30% + 效价30% - 矛盾扣分(最多25%)
```

注释更新为"内驱力检验"语义。

---

### 4. cognitive_controller.h — 新增字段 + API

```c
// 新字段
void* causal_graph;       // CausalGraph*
void* concept_hierarchy;  // ConceptHierarchy*

// 新 API
float causal_path_score(CognitiveController* cc,
                        SubTopology* sub,
                        const int* node_ids,
                        int path_len);
```

---

### 5. dialog_generate.c — walk 循环因果筛选

在 `topology_walk_greedy` 返回后、路径输出前插入：

```c
if (path_len <= 1) continue;

// 因果筛选
if (dsys && dsys->controller) {
    float causal = causal_path_score(dsys->controller, sub, path_nodes, path_len);
    if (causal < 0.25f) continue;  // 因果不合理，试下一个起点
}
```

---

### 6. dialog_system.c — 字段注入

```c
sys->controller->causal_graph = sys->causal_graph;
sys->controller->concept_hierarchy = sys->concept_hierarchy;
```

---

## 撤回

| 文件 | 改动 |
|------|------|
| `include/seq2seq.h` | 删除 |
| `src/seq2seq.c` | 删除 |
| `include/dialog_system.h` | 移除 seq2seq include 和字段 |
| `src/dialog_system.c` | 移除创建/销毁/训练代码 |
| `src/dialog_generate.c` | 移除 seq2seq include 和 fallback |

## 影响范围

- `multi_topology.c`: +5 行（context_valence）
- `cognitive_controller.c`: ~30 行修改（三函数重排）
- `cognitive_controller.h`: +2 字段 + 1 API
- `dialog_generate.c`: +8 行（因果筛选）
- `dialog_system.c`: +3 行（字段注入）
- 无破坏性变更
