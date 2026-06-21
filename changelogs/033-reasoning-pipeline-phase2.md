# 0.3.3 — 推理管线 Phase 2 真实接入

> **日期**: 2026-06-20 | **类型**: 新增

---

## 概述

Phase 1 搭建了 PrefrontalExecutive + IdeaArena 的骨架，但 `pfe_solve_subgoal()` 只是一个扫描 activation 的空壳，`pfe_reason()` 从未被 Gateway 调用。Phase 2 补全了这些关键缺口，使 PivotMind 首次拥有端到端的推理能力。

---

## 改动文件

| 文件 | 变更 |
|------|------|
| `src/prefrontal_executive.c` | 核心重写：真实 diffusion 管线的 `pfe_solve_subgoal()`、特征向量跨子目标注入、可解释推理链输出 |
| `src/idea_arena.c` | `arena_feedback_to_master()` 新增胜者/淘汰路径边权重的 motivational_bias 调节 |
| `demos/pivotmind_gateway.c` | `handle_chat()` 复杂问题走 PFE 推理路径，简单问题回退旧路径；PFE 响应标注 `"reasoning":"pfe"` |

---

## 详细变更

### 1. `pfe_solve_subgoal()` — 空壳 → 真实管线

**之前**（Phase 1 空壳）:
- 只扫描语义拓扑前 200 个节点的 `activation` 最大值
- 用 `0.3 + best_act * 0.7` 模拟满意度
- 答案文本是 `[分解答案] X (满意度=Y)` 占位符

**现在**（Phase 2 真实推理）:
```
pfe_solve_subgoal()
  ├─ cognitive_controller_set_context()  — 设置子问题上下文
  ├─ calc_context_activations()         — 计算上下文激活
  ├─ compute_intent()                    — 意图推断
  ├─ for retry in 0..MAX_RETRIES:
  │   ├─ diffusion_init()               — 初始化扩散上下文
  │   ├─ diffusion_generate()           — 真实走边生成序列
  │   ├─ cingulate_evaluate()           — ACC 四维评估
  │   ├─ 提取词汇拓扑节点 ID
  │   └─ 递增温度重试（0.15 + retry*0.12）
  └─ 存储最佳结果
```

### 2. 跨拓扑特征向量注入

新增两个内部函数：

- **`pfe_store_dependency_features()`**: 将已解子目标的答案节点特征向量 EMA 写入 `working_activation[][]`
- **`pfe_apply_working_bias()`**: 对语义拓扑节点做余弦相似度偏置（相似度 > 0.3 的节点激活提升），为后续子目标的扩散走边提供引导

这解决了"子目标 A 的产出如何影响子目标 B 的搜索空间"的问题。

### 3. 可解释推理链输出

`pfe_synthesize_answer()` 不再简单拼接，而是输出完整的推理轨迹：

```
为了回答这个问题，我分步进行了思考：

1. 什么是X？ → [答案1] (置信度: 72%)
2. X的关键特征和属性 → [答案2] (置信度: 68%)
3. X与Y的因果链 → [答案3] (置信度: 81%)

综合以上分析：[结论]

（推理模式: 解释分解，共3子问题，已解决3个）
```

单子目标模式直接输出答案，低置信度时附带提示。

### 4. Gateway 推理路由

`handle_chat()` 新增逻辑：

```
if (pfe_assess_complexity(msg) > 0) → pfe_reason() [PFE路径]
else → prefrontal_chat() [旧联想路径]

PFE 失败 → 自动回退到旧路径
```

PFE 响应在 JSON 中额外标注 `"reasoning":"pfe"`，方便前端区分推理模式。

PFE 调用后自动执行 `arena_feedback_to_master()` 做胜者回流。

### 5. IdeaArena 边权重反馈

`arena_feedback_to_master()` 增强：

- **胜者路径**: 路径节点间边的 `weight` EMA 提升（α=0.08）
- **淘汰路径**: 路径节点间边的 `motivational_bias` 衰减（×0.85，下限 0.01）

实现了"竞争中胜出的路径被强化，失败的路径被抑制"的闭环学习。

---

## 编译验证

```
$ gcc -Wall -Wextra -std=gnu99 -I include -c src/prefrontal_executive.c → OK (0 warnings)
$ gcc -Wall -Wextra -std=gnu99 -I include -c src/idea_arena.c → OK (0 warnings)
$ gcc -Wall -Wextra -std=gnu99 -I include -c src/brainstem.c → OK (0 warnings)
```

---

## Phase 2 完成项清单

| 任务 | 状态 |
|------|------|
| `pfe_solve_subgoal()` 接入真实 diffusion+cingulate 管道 | ✅ |
| `pfe_reason()` 接入 Gateway 请求处理管线 | ✅ |
| 子目标答案的跨拓扑特征向量注入 | ✅ |
| IdeaArena 胜者反馈回流到边权重 (motivational_bias) | ✅ |
| 推理链的可解释输出 | ✅ |
| 多候选 Beam 收集（通过 retry 温度扫描实现） | ✅ |

---

## Phase 3 待办

- [ ] PFE 策略权重自学习（哪种分解策略对哪类问题更有效）
- [ ] 递归分解（复杂子目标进一步拆解）
- [ ] 推理成功率统计与自适应参数调优
- [ ] 推理过程持久化（可复现的推理链）
- [ ] 单元测试
