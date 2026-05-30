# 010B — 核心管线审查修复

> 日期: 2026-05-26 | 审查: 010A | 修复: 010B | 关联: 009B

## 修复清单

### P0-1 — feedback_correct 增加边置信度压制

**文件**: `include/active_learner.h`, `src/active_learner.c`, `src/dialog_generate.c`

- ActiveLearner 新增 `last_path_node_ids/topo_types/edge_ids/count` 字段
- `learn_from_dialog` 负面反馈分支新增边置信度压制：`conf *= 0.6`，底线 0.05
- 节点更新改为路径精确定位（解决 P1-3），兜底保留 strstr
- `dialog_generate` 在路径记录后同步到 `dsys->learner->last_path_*`

### P0-2 — dialog_generate global_visited 分配时机修复

**文件**: `src/dialog_generate.c`

- `global_visited` 的 `calloc` 从因果筛选后移到首次 sub 确定后（for 循环体内，visited 检查前）
- 修正了"先使用后分配"导致的跨起点去重失效

### P0-3 — autonomic_decay_all 竞争衰减

**文件**: `src/autonomic_learner.c`

- 改为三档差异化衰减：
  - 高置信"赢家"（>1.5x均值 且 >0.5）：衰减率 0.9995（几乎不衰减）
  - 低置信"输家"（<0.5x均值 且 <0.3）：衰减率 0.85（加速衰退）
  - 中间区域：标准衰减率 0.999
- 单边/无连接节点保持原行为

### P1-1 — evaluate_draft drive_score 改为路径效价

**文件**: `src/cognitive_controller.c`

- 从读取情绪拓扑全局平均效价 → 读取当前 draft 路径各节点 `fabsf(valence)` 均值
- 删除了对 `cc->memory` 和 `TOPO_EMOTION` 的无效依赖

### P1-2 — causal_path_score O(1) 反向映射

**文件**: `src/cognitive_controller.c`

- 函数入口构建 `topo_to_cg[max_topo_id]` 反向映射（一次 O(n)）
- 循环内反向查找从 O(cg->node_count) 降至 O(1)
- 函数出口 free 临时数组

### P1-3 — learn_from_dialog 全拓扑扫描消除

**文件**: `src/active_learner.c`

- 优先使用 `learner->last_path_*` 精确定位节点（O(path_len)）
- strstr 全拓扑扫描降为兜底

### P1-4 — dialog_generate 增加内感受评估门

**文件**: `src/dialog_generate.c`

- 因果筛选后新增 `evaluate_draft` 质量检查
- 构建临时 `PathResult`，填充 act_sum/conf_sum
- 不达标时 `continue` 试下一个起点

### P2-1 — boost_connection 字序编码

**文件**: `src/autonomic_learner.c`

- `boost_connection` → `boost_connection_weighted(topo, a, b, state, weight_mult)`
- 相邻字对（dist=1）：weight_mult=1.5，基础权重=0.45
- 非相邻字对（dist=k）：weight_mult=1.5/k，下限 0.3
- 交叉边（输入↔回复）：weight_mult=1.0
- 核心4的跨拓扑版本同步更新

### P2-2 — 删除 dialog_system.c 重复的 dialog_generate

**文件**: `src/dialog_system.c`

- 删除了 L797–L985 的重复 `dialog_generate` 实现（约 190 行）
- 统一到 `src/dialog_generate.c` 的单一实现（已含全部 010B 修复）
- 消除了函数双重定义导致的潜在链接问题和行为不一致

### P2-3 — 魔数集中到 constants.h

**文件**: `include/constants.h`, `src/autonomic_learner.c`, `src/active_learner.c`, `src/dialog_generate.c`, `src/cognitive_controller.c`, `src/multi_topology.c`

新增常量：
- `PM_AUTONOMIC_FLUSH_THRESHOLD` (50)
- `PM_AUTONOMIC_IDLE_FLUSH_SECS` (30)
- `PM_ACTIVE_LEARNER_INTERVAL` (300)
- `PM_WALK_MAX_OUTPUT` (20)
- `PM_CONCEPT_JUMP_LIMIT` (3)
- `PM_WALK_PRUNE_FLOOR` (0.03f)
- `PM_WALK_PRUNE_CEIL` (0.30f)
- `PM_EVALUATE_THRESHOLD` (0.5f)

所有引用点已更新。

### P2-4 — reader.c 废弃标记

**文件**: `tools/reader.c`

- 文件头注释标注"功能已由自主层部分取代，保留供离线语料导入"

### BPTT 集成 — RNN 反向传播接入对话管线

**文件**: `include/bptt_learner.h`, `src/bptt_learner.c`, `include/dialog_system.h`, `src/dialog_system.c`

- 新建 `BpttLearner` 结构体：模型 RNN(24→64)→Linear(64→24)，Adam 优化器
- 每轮对话将用户输入和 AI 回复映射为节点特征序列，训练 RNN 预测
- 与 `autonomic_learner` 互补：拓扑管边置信度，BPTT 管神经网络权重
- 接入 `DialogSystem`：创建时初始化，每轮对话后自动训练，销毁时输出统计

### Phase 3 — reader 内化：autonomic_learn_from_text

**文件**: `include/autonomic_learner.h`, `src/autonomic_learner.c`

- 新增 `autonomic_learn_from_text(master, text, text_len, state)` API
- 按句号/感叹号/问号/分号/换行切分文本为句子
- 逐对相邻句子调用 `autonomic_learn_from_dialog`，实现"读书即学习"
- 每 100 对打印进度，支持最大 10000 句的文本
- 与 `reader.c` 不同：不训练 embedding，直接在拓扑中建边（与架构哲学一致）

---

## 影响范围

| 文件 | 改动 | 风险 |
|------|------|------|
| `include/constants.h` | +8 宏 | 无风险 |
| `include/active_learner.h` | +4 字段 | ABI 变更，需全量重编译 |
| `src/active_learner.c` | learn_from_dialog 重写后半段 | 逻辑增强，原行为兼容 |
| `src/autonomic_learner.c` | decay_all 三档 + boost_connection→weighted + 字序 + learn_from_text | 新增 API，衰减行为变化 |
| `src/cognitive_controller.c` | evaluate_draft / causal_path_score 重写核心逻辑 | 评分语义变化 |
| `src/dialog_generate.c` | global_visited 提前 + evaluate_draft + learner 同步 + 魔数 | 控制流变化 |
| `src/dialog_system.c` | 删除重复 dialog_generate + BPTT 接入 | 行为统一 |
| `src/multi_topology.c` | prune floor/ceil 宏化 | 无行为变化 |
| `src/bptt_learner.c` | 新建（229行） | 新增模块 |
| `tools/reader.c` | 注释 | 无风险 |

## 未修复项

无 —— 010A 审查全部 12 项 + BPTT 集成 + reader 内化均已完成。

## 关联

- 审查: 010A
- 修复: 010B
- 前序: 009B
