# 017-competitive-queue

## 概要

实现竞争队列生成机制，替代贪心走边的局部最优局限。

## 改动

### 新增
- `competitive_queue_generate()`: 基于全局工作空间理论的路径生成
  - 全图激活场视角（非局部边邻居）
  - 竞争评分 = 激活 × 意图调制 × 锚点对齐 × 效价 × 热度 × 置信度
  - 胜者广播 + 自我抑制 防重复

### 修改
- `topology_walk_greedy()`: 新增 `query_anchor` 参数（输入锚定防漂移）
- `topology_walk_beam()`: 签名同步
- `topology_walk_cross()`: 签名同步
- `dialog_generate()`: 竞争队列首选，贪心走边回退

### 意图调制升级
- `(0.5 + 0.5 * intent_weight)` → `(0.5 + 0.5 * intent_weight * (0.3 + 0.7 * edge_bias))`
- `connection_motivational_bias` 作为边的"受体敏感度"

## 关联
- 审查: reports/017-A.md
- 修复: reports/017-B.md
