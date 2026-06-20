# 031 - 模板 POS 结构合并（Pipeline A/B 统一）

## 动机

模板系统存在两个独立的数据来源：

| Pipeline | 数据来源 | 产出 |
|----------|---------|------|
| A | 高频词对统计 + 不可分解性分析 | 三元组模板（`template_auto_build`） |
| B | POS 观测缓冲 | 语法句式模板（`template_build_from_pos_patterns`） |

两个 Pipeline 独立运行，但产出在模板拓扑中并存。当 A 和 B 生成相同 POS 结构（如 `[N][的][N]`）的模板时，下游匹配引擎需要扫描所有模板——冗余节点稀释了推理引导力。

核心设计原则：**A/B 在数据来源层独立，在模板表述层统一**。

## 设计决策

### 合并键

```
merge_key = (tpl_pos_len, tpl_pos_seq[0..len-1], 归一化 tpl_connectors[0..len-2])
```

两个模板合并当且仅当：
1. `tpl_pos_len` 相同
2. `tpl_pos_seq` 逐位相等
3. `tpl_connectors` 逐位归一化后相等（空字符串是独立键值，不与 `"的"` 等合并）

### 连接词归一化规则

| 输入 | 归一化输出 | 说明 |
|------|-----------|------|
| `NULL` | `""` | 统一空值表示 |
| `"的 "` | `"的"` | 去除尾部空白 |
| `"   "` | `""` | 纯空白等同空 |
| `"的"` | `"的"` | 正常保留 |

### 合并策略（选项 b：批量合并）

- A 和 B 各自独立产出模板，互不查询
- 在 `template_build_concepts` 阶段统一做 POS 结构聚类合并
- 下游匹配引擎只看到一种格式，零改动

### 标点处理（配套决策）

| 标点类型 | 处理 | 理由 |
|---------|------|------|
| 句末标点 。！？ | POS 序列硬边界截断 | 模板不跨句 |
| 顿号 、 | 纳入 connector | 语义等价于 `"和"` |
| 逗号 ，； | 不默认纳入 | 角色过多（列举/转折/因果），防组合爆炸 |

## 代码改动

### 1. `src/template_builder.c` — 核心实现

**新增辅助函数：**

```c
static void norm_connector(const char* src, char* dst);
// NULL→""，去首尾空白，纯空白→""

static uint32_t merge_key_hash(int pos_len, const int* pos_seq,
                                const char connectors[][TPL_CONNECTOR_BUF]);
// djb2 哈希，pos_seq 逐元素 + 归一化 connector 逐字节

static int merge_key_equals(int len_a, const int* seq_a,
                             const char conn_a[][TPL_CONNECTOR_BUF],
                             int len_b, const int* seq_b,
                             const char conn_b[][TPL_CONNECTOR_BUF]);
// 逐位比较（conn_b 归一化后与 conn_a 比较）
```

**新增主函数：**

```c
int template_merge_by_pos_structure(MasterTopology* master);
```

流程：
1. **Pass 1 — 哈希分组**：扫描 TOPO_TEMPLATE，按合并键分组（开放寻址，1021 槽）
2. **Pass 2 — 组内合并**：
   - 选 confidence 最高者为 **survivor**
   - 特征向量 = 组内加权平均（权重 = 各自 confidence，最小保底 0.01）
   - 置信度 = 组内最大值（取 max 而非均值：B 独立确认 A 的 POS 结构是新增证据，不应稀释 survivor 置信度）
   - 其余成员**软删除**：`tpl_pos_len = 0`, `confidence = 0.0`
3. 输出 `[TEMPLATE-MERGE]` 日志

**集成点：**

```c
// template_auto_build() — Pipeline A 路径
if (built > 0) {
    template_merge_by_pos_structure(master);           // ← 新增
    template_build_concepts(master, max_templates / 4);
}
```

### 2. `tools/batch_learn.c` — Pipeline B 合并

```c
int pos_built = template_build_from_pos_patterns(master, cc, 3);
if (pos_built > 0) {
    printf("  → POS 句式模板: %d 个\n", pos_built);
    int mg = template_merge_by_pos_structure(master);  // ← 新增
    if (mg > 0) printf("  → 模板合并: %d 组\n", mg);
}
```

### 3. `include/template_builder.h` — 公共声明

```c
int template_merge_by_pos_structure(MasterTopology* master);
```

## 调用链

```
Batch Learn 主循环
├── template_auto_build()
│   └── template_merge_by_pos_structure()  ← Pipeline A 内部合并
│   └── template_build_concepts()          ← 概念节点基于合并后模板
│
├── template_build_from_pos_patterns()     ← Pipeline B 产出
│   └── template_merge_by_pos_structure()  ← B 与 A 的 survivor 合并
│
└── Brainstem (每 300 tick)
    └── broca_build_templates() → template_auto_build()
        └── template_merge_by_pos_structure()  ← 运行时周期合并
```

## 下游兼容性

**零改动。** `master_find_template_for_pair_nolock` 遍历模板时自动跳过 `tpl_pos_len < 2` 的节点（软删除的模板），只匹配活跃的 survivor。

```
master_find_template_for_pair_nolock()
  → for (i = 0; i < node_count; i++)
      if (tn->tpl_pos_len < 2) continue;  ← 自动跳过已合并节点
```

## 编译验证

```
gcc 13.2.0 -Wall -Wextra -O2 -std=gnu99
src/template_builder.c: ✓ 零警告零错误
tools/batch_learn.c:   ✓ 预存错误与本改动无关
```
