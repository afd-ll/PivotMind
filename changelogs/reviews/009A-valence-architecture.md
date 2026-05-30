# 009A — 效价-因果-内感受三层分离审查

> 日期: 2026-05-26 | 审查: 009A | 关联: 008B

## 问题

008 尝试用 Seq2Seq 外部模型做输出约束，与玄枢的拓扑内生哲学冲突。
008B 用语义-因果双约束替代，但架构层次错位：

- **效价**被当作五个评分维度之一，与因果、语义并列投票
- **因果**被放在 evaluate 阶段，属于"事后打分"，而非"事前筛选"
- **内感受**变成了加权算分机，丧失了"内驱力检验"的本质

## 架构原则

认知科学中：
- **效价 = 内驱力** — 模仿人体的好奇、反思、探索欲望，决定"想要什么"
- **因果 = 逻辑约束** — 探索阶段的筛选器，确保"想得合理"
- **内感受 = 效价检验** — 判断结果是否满足内驱力，是单一的整体判断

三者不在同一个层面，不应并列加权。

## 设计

```
         ┌─────────────────────────────────────┐
  上游   │  效价(内驱力)                         │
         │  CognitiveState.drive → 意图权重       │
         │  决定"往哪个方向走"                    │
         └──────────────┬──────────────────────┘
                        ↓
         ┌─────────────────────────────────────┐
  中游   │  走边约束                            │
         │  causal_path_score: 因果合理→通过     │
         │  context_valence: 情感基调→效价匹配   │
         │  语义动态权重: 上下文→防主题漂移       │
         │  决定"每一步选哪个节点"                │
         └──────────────┬──────────────────────┘
                        ↓
         ┌─────────────────────────────────────┐
  下游   │  内感受(evaluate_draft)               │
         │  质量(40%) + 连贯(30%) + 效价(30%)    │
         │  - 矛盾扣分(最多25%)                  │
         │  判断"结果是否满足内驱力"              │
         └─────────────────────────────────────┘
```

### 改动清单

| # | 文件 | 内容 |
|---|------|------|
| 1 | `multi_topology.c` | topology_walk_greedy 加入 context_valence 情感基调约束 |
| 2 | `cognitive_controller.c` | causal_consistency_check → causal_path_score（移到中游，签名改为 (cc, sub, node_ids, path_len)） |
| 3 | `cognitive_controller.c` | evaluate_draft 五维→三维：质量+连贯+效价-矛盾 |
| 4 | `include/cognitive_controller.h` | 新增 causal_path_score 声明，CognitiveController 加 causal_graph/concept_hierarchy 字段 |
| 5 | `dialog_generate.c` | walk 循环中加入 causal_path_score 筛选（causal < 0.25 跳过） |
| 6 | `dialog_system.c` | dialog_system_create 注入 causal_graph/concept_hierarchy |

### 撤回

| # | 内容 |
|---|------|
| — | `seq2seq.h` / `seq2seq.c` 删除 |
| — | dialog_system.h/c, dialog_generate.c 移除所有 seq2seq 引用 |

## 验证

- 情感基调约束：正效价路径中负效价候选节点得分降低
- 因果筛选：causal < 0.25 的路径被跳过
- 内感受回归：evaluate 不再打印因果维度
