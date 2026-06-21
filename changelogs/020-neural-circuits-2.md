# 0.1.4 — 打通剩余神经回路

> **日期**: 2026-05-30 | **类型**: 新增

## 回路 3b：cognitive_confidence → 走边评分

- **文件**: `src/multi_topology.c`（4 处 node_conf 计算）
- **机制**: 走边评分中的 `node_conf` 由原来的纯 `target->confidence` 改为：
  `node_conf = confidence * 0.6 + cognitive_confidence->combined * 0.4`
  - 每次走边前调用 `cognitive_confidence_compute()` 刷新综合置信度
  - 三个维度（预测准确度/满意度/新颖性）通过 EMA 更新后自然流入评分
- **效果**: 回路 3（020 已通）的输出此刻真正被走边读取

## 回路 5：Fisher 信息代理 → 衰减保护

- **文件**: `src/autonomic_learner.c` → `autonomic_decay_all`
- **机制**: `node->selection_count` 作为重要性代理
  - `node_importance = 1.0 / (1.0 + 0.05 * selection_count)`
  - 有效衰减率 = `1.0 - (1.0 - decay_rate) * node_importance`
  - 被走边频繁选中的节点（selection_count 高），其所有出边衰减更慢
- **效果**: 重要的边不再被均匀衰减——系统学会了保护高频使用路径

## 回路 6：selection_count → 赫布 boost

- **文件**: `src/autonomic_learner.c` → `boost_connection_weighted`
- **机制**: 走边频繁选中的节点对，赫布学习时权重乘 `sel_boost`
  - `sel_boost = 1.0 + 0.1 * avg_selection_count / 10.0`（上限 1.5x）
- **效果**: 走边和赫布学习两个信号源形成正反馈——走得多的边，学得更快

## 回路 8：节点级 valence → 全局情绪漂移

- **文件**: `src/background_clock.c` → `drift_cognitive_state`
- **机制**: 每次时钟 tick 从情绪拓扑采样 20 个节点，
  平均 valence 的 10% 混入 `state->valence` 的 EMA
- **效果**: 全局情绪不再只向基线回归——情绪拓扑中
  积累的正/负效价会缓慢渗透到系统的整体情绪状态

## 未实现（需改函数签名）

- **回路 7（因果图 ↔ 赫布）**: `boost_connection_weighted` 无因果图访问权限
- **回路 9（LTM ↔ 拓扑）**: `autonomic_learn_from_dialog` 不接收 MemorySystem 参数
