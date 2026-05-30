# 022 — 打通因果图+记忆系统回路

> 日期: 2026-05-30 | 修复人: CodeWhale

## 改动

### 签名变更
`autonomic_learn_from_dialog` 新增两个可选参数：
```c
void autonomic_learn_from_dialog(MasterTopology* master,
                                 const char* user_input,
                                 const char* ai_response,
                                 AutonomicState* state,
                                 void* causal_graph,   // 新增：CausalGraph*，可为NULL
                                 MemorySystem* memory); // 新增：MemorySystem*，可为NULL
```

所有调用方已更新：`dialog_system.c` 传入 controller 中的因果图和记忆；
`batch_learn.c` 和内部调用传入 `NULL, NULL`。

### 回路 7：因果图 → 赫布 boost

- **文件**: `src/autonomic_learner.c` → 核心3和核心4之间
- **机制**: 对每对 (input_node, response_node)，
  在因果图中查找是否存在因果边 → 按因果强度增强赫布权重
  - `extra_boost = 1.0 + causal_strength * 0.5`
- **效果**: 有因果关联的概念对在赫布学习中获得更高权重

### 回路 9：记忆系统 → 赫布 boost

- **文件**: `src/autonomic_learner.c` → 同上位置
- **机制**: 对每个节点的概念名在 LTM 中检索
  - 重要概念 (importance > 0.5) → 赫布权重增强
  - `extra_boost = 1.0 + importance * 0.3`
- **效果**: 系统记忆中重要的概念在赫布学习时获得更高权重

## 全部 10 条回路状态

| # | 回路 | 状态 |
|---|------|:---:|
| 1 | satisfaction → valence | ✅ 020 |
| 2 | activation → 学习门控 | ✅ 020 |
| 3 | satisfaction → cognitive_confidence | ✅ 020 |
| 3b | cognitive_confidence → 走边 | ✅ 021 |
| 4 | 模板增量生长 | ✅ 020 |
| 5 | selection → 衰减保护 | ✅ 021 |
| 6 | selection → 赫布 boost | ✅ 021 |
| 7 | 因果图 → 赫布 | ✅ 022 |
| 8 | 节点valence → 全局情绪 | ✅ 021 |
| 9 | LTM → 赫布 | ✅ 022 |
