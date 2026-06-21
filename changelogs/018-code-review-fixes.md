# 0.1.2 — 代码审查修复

> **日期**: 2026-05-30 | **类型**: 修复

## 修复项

### 🔴 严重

#### 1. `create_reasoning_node` 空指针解引用
- **文件**: `src/trace_wisdom_topology.c:39-48`
- **问题**: `if (features)` 检查的是外部传入参数而非 `node->features` 分配结果。
  当 `malloc` 返回 NULL 而调用方传入非 NULL 的 `features` 时，直接 `memcpy` 到空指针导致崩溃。
- **修复**: 改为 `if (node->features)` 先验证分配成功。

#### 2. `trace_wisdom_net_dynamic_remove_node` use-after-free
- **文件**: `src/trace_wisdom_topology.c:396-409`
- **问题**: 删除节点后未清理其他节点 `connections[]` 中的悬空指针，后续走边/激活传播
  访问已释放内存。
- **修复**: 在释放节点前加 mutex 保护，遍历所有节点清理指向被删节点的连接引用。

### 🟠 重要

#### 3. STM/LTM 哈希表墓碑 bug
- **文件**: `src/memory_system.c`（6 个 static 函数）
- **问题**: 开放寻址哈希表删除条目时直接用 `entry_index = -1` 标记为空槽，
  导致查找遇到此槽时提前终止探测，后续被墓碑遮挡的键无法检索。
- **修复**: 引入 `HASH_TOMBSTONE = -2` 墓碑标记：
  - `_stm/ltm_hash_remove`: 写入 `HASH_TOMBSTONE` 而非 `-1`
  - `_stm/ltm_hash_lookup`: 只在遇到 `HASH_EMPTY` 时终止探测，跳过墓碑
  - `_stm/ltm_hash_insert`: 优先复用墓碑槽位后再用空槽

#### 4. `compute_intent` 丢弃上下文关联度
- **文件**: `src/cognitive_controller.c:404-457`
- **问题**: `compute_intent(ctx_activations)` 将参数显式转换为 `(void)`，意图向量计算
  缺少上下文关联度维度，与架构文档三因子融合公式不一致。
- **修复**: 移除 `(void)ctx_activations`，在合成意图权重时引入 `ctx_factor`：
  `w = base × ctx_factor × nf × vf × cf`

### 确认无需修复（已有保护）

- `cross_adj` 邻接表索引越界 → `master_add_cross_link` 已有动态扩容，所有访问点均有边界检查
- `background_clock.is_running` 内存屏障 → 头文件已声明 `volatile int is_running`
- STM 哈希表满 → `stm_store` 已有 LRU 淘汰逻辑
