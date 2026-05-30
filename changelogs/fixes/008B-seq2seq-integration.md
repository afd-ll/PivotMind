# 008B — 语义-因果双重约束输出（008A 实施）

> 日期: 2026-05-26 | 审查: 008A | 修复: 008B | 关联: 006B, 007

## 决策

摒弃 Seq2Seq 外部模型方案，改用**语义拓扑 + 因果拓扑内生约束**，全在拓扑体系内闭环。

## 改动清单

### 撤回（与 008 临时方案对比）

| 文件 | 改动 |
|------|------|
| `include/seq2seq.h` | 删除（需手动 `del`） |
| `src/seq2seq.c` | 删除（需手动 `del`） |
| `include/dialog_system.h` | 移除 `#include "seq2seq.h"` 和 `Seq2Seq* seq2seq` 字段 |
| `src/dialog_system.c` | 移除 Seq2Seq 创建/销毁/训练代码 |
| `src/dialog_generate.c` | 移除 `#include "seq2seq.h"` 和 Seq2Seq fallback |

### 新增

| 文件 | 改动 |
|------|------|
| `src/cognitive_controller.c` | +`causal_consistency_check()` — 路径相邻概念对→因果图查询→一致性评分 |
| | +`self_contradiction_check()` — 情绪效价互斥检测 + 概念层次跳跃检测 |
| | `evaluate_draft` 升级 v2：三维→五维评分 |

### 增强

| 文件 | 改动 |
|------|------|
| `include/cognitive_controller.h` | CognitiveController 新增 `causal_graph` 和 `concept_hierarchy` 字段 |
| `src/dialog_system.c` | `dialog_system_create` 注入因果图和概念层次到认知调度中心 |
| `src/multi_topology.c` | `topology_walk_greedy` 语义权重动态增强（context_count > 5 → +0.10） |

## evaluate_draft v2 五维评分

| 维度 | 权重 | 说明 |
|------|------|------|
| 效价 | 20% | 情绪拓扑平均效价 |
| 拓扑连贯 | 25% | 路径内相邻节点连接强度 |
| 激活充足 | 20% | 平均激活值 |
| **因果一致性** | **25%** | 概念对在因果图中的边强度（新增） |
| **语义约束** | **10%** | 自矛盾检测 + 概念跳跃惩罚（新增） |

## 架构

```
用户输入 → 拓扑激活 → 走边路径生成 → 回复文本
              ↑            ↓
         意图权重      ┌─ 语义约束：上下文动态权重→影响 walk 评分
                      ├─ 因果回读：causal_consistency_check→evaluate_draft
                      └─ 自矛盾检测：self_contradiction_check→evaluate_draft
```

无外部模型。语义拓扑管"往哪走"，因果拓扑管"对不对"。

## 影响范围

- `cognitive_controller.c`（+~150 行，两个新函数 + evaluate_draft 重写）
- `multi_topology.c`（+2 行，语义权重动态增强）
- `include/cognitive_controller.h`（+2 字段）
- `dialog_system.c`（+3 行，字段注入）
- dialog_system.h / dialog_generate.c（恢复原状）
