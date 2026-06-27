# 044 — v0.4.7 架构审查报告

> **日期**: 2026-06-27 | **状态**: 已完成

---

## 一、审查概览

| 维度 | 评估 |
|------|------|
| 脑区架构 | ✅ 13 个脑区全部完整实现，无占位代码 |
| 多学习器并行 | ✅ 4 种学习器 + 预训练系统，覆盖全生命周期 |
| 神经网络子系统 | ✅ LSTM/GRU/RNN/Tensor/Trainer 完整可用 |
| 内存安全 | ✅ v0.4.2 三轮审计，悬空指针/竞态已修复 |
| 双子系统桥接 | ⚠️ 拓扑网与神经网络单向连接，未反馈 |
| 配置管理 | ⚠️ 参数散落 5+ 头文件，无运行时配置 |
| 测试覆盖 | ⚠️ 对话管线/扩散引擎无自动化测试 |
| 脑区生命周期 | ⚠️ 全量静态初始化，无按需启动机制 |
| 跨拓扑质量 | ⚠️ 边质量评估依赖单一余弦相似度策略 |

---

## 二、问题分析

### 02.1 双子系统缺乏统一桥接层

**现状**：系统存在两套计算范式：

| 拓扑溯智网络 | 神经网络子系统 |
|---|---|
| `multi_topology.c` (3,714行) | `tensor.c` (889行) + LSTM/GRU/RNN/Trainer |
| 走边 + 激活扩散 + Hebbian 学习 | 矩阵运算 + 梯度下降 + BPTT |
| 显式路径，可追溯 | 隐式表征，高表达力 |
| ~9,300 行核心代码 | ~5,400 行核心代码 |

当前唯一的桥接在 `bptt_learner.c`：

```c
// bptt_learner.c — 构建模型
bl->model = model_create();
Layer* rnn = layer_create_simple_rnn(bl->input_dim, bl->hidden_dim);
model_add_layer(bl->model, rnn);
Layer* linear = layer_create_linear(bl->hidden_dim, bl->input_dim, true);
model_add_layer(bl->model, linear);
bl->optimizer = optimizer_create_adam(0.001f, 0.9f, 0.999f, 1e-8f);
```

**问题**：这是单向桥——神经网络可以拿拓扑数据训练，但训练结果不流回拓扑网络。

**影响**：
- BPTT 训练后的权重没有用于调节走边评分的五维权重
- 预训练（Skip-gram, embedding_dim=64）的特征向量与拓扑中的 512 维 Hebbian 特征向量完全独立
- 两个子系统各自消耗内存和 CPU，但没有协同增益

**建议方向**：
1. 用神经网络层的输出作为拓扑走边的 `intent_weight` 动态调节器
2. 把涌现式聚类的锚点中心（Emergent POS centroid）反馈给预训练的特征表示
3. 在 `cognitive_controller.c` 的意图向量计算中引入神经网络输出的置信度作为额外因子

### 02.2 配置管理分散

**现状**：参数分布在至少 5 个位置：

| 位置 | 参数 |
|------|------|
| `include/common.h` | NODE_FEATURE_DIM, ACTIVATION_THRESHOLD, DECAY_RATE |
| `include/constants.h` | PM_TOKEN_*, PM_BUF_*, PM_LEARN_*, PM_TOPOLOGY_* |
| `include/cognitive_params.h` | 认知调度相关参数 |
| `include/pretrain.h` | embedding_dim=64, window_size=5, lr=0.025 |
| 各脑区 `.c` 文件内 | 硬编码阈值 (如 `prefrontal.c` accept=0.15, block=0.08) |

**影响**：
- 在不同硬件上部署（x86 vs ARM vs FPGA）需要改头文件重编译
- 调参过程中无法热加载，迭代周期长
- 没有参数版本管理——不知道当前用的是哪组参数跑出的哪种效果

**建议方向**：
1. 统一的 JSON/YAML 配置文件，启动时加载
2. 运行时可 reload 的配置（至少学习率、衰减率、阈值等关键参数）
3. 参数快照——每次训练/运行与状态文件关联，支持 A/B 对比

### 02.3 测试覆盖盲区

**现状**：

| 已有测试 | 缺失测试 |
|----------|----------|
| tensor/model/trainer 单元测试 | dialog_system.c (2,096行) |
| cognitive_controller 测试 | diffusion.c (501行) |
| PFE 23 项单元测试，100% 通过 | multi_topology.c 走边评分函数 (3,714行) |
| web_fetch 测试 | autonomic_learner.c 赫布并发更新 (1,142行) |
| integration 测试 | dialog_generate.c 回复生成管线 (582行) |

**影响**：
- 走边算法的五维评分公式没有回归保护——任何系数调整都可能无声无息地改变输出质量
- 扩散引擎虚词过滤 (043) 改完只能通过手工验证
- dialog_system 作为最大的对话模块没有自动化测试，每次上线靠"跑起来看看"

**建议方向**：
1. 优先为 `dialog_system.c` 写集成测试：给定固定状态的拓扑，验证输入→输出的一致性
2. 为扩散引擎加 mock 拓扑的回归测试：验证虚词过滤、跨层索引、active set 更新
3. 对话质量自动化评估：用预定义的问答对比较拓扑走边输出和期望输出，计算重合度

### 02.4 脑区生命周期全量静态初始化

**现状**：`pivotmind_gateway.c` 启动时一次性创建全部 13 个脑区：

```c
// gateway 启动顺序
create_prefrontal() → create_brainstem() → create_thalamus()
→ create_perception() → create_hippocampus() → create_cerebellum()
→ create_amygdala() → create_pfe() → create_arena()
→ create_broca() → create_hypothalamus()
```

**影响**：
- Perception（联网搜索）在网络断开时仍然占用插槽和线程
- 在更小内存的设备上（< 1GB），全量初始化的内存开销不可忽视
- 未来要上 FPGA 软核，需要做功能裁剪

**建议方向**：
1. 脑区按需启动/休眠接口：`brainstem_enable_region(REGION_PERCEPTION, false)`
2. 启动时根据配置和环境检测决定哪些脑区初始化
3. 丘脑（Thalamus）已有资源门控框架，可以扩展为脑区生命周期管理器

### 02.5 跨拓扑连接质量依赖单一策略

**现状**：跨拓扑连接（CrossTopologyLink）的质量评估依赖：

1. 节点 512 维特征余弦相似度（`rebuild_cross_connections`）
2. `CrossTopoHitRecord` 动态跟踪（走边过程中的 co-activation 记录）

**问题**：
- 余弦相似度是静态的初始化策略，大量训练后边的实际使用模式可能与初始相似度偏离
- 没有"边质量重评估"——训练 10 轮后的跨拓扑边和训练 1000 轮后的跨拓扑边用的是同一套 `transfer_rate`
- v0.4.3 涌现式词类实验已经证明：从使用模式中涌现的聚类比人工预设的相似度更有价值

**建议方向**：
1. 在 `rebuild_cross_connections` 后增加周期性重评估：用 `use_count` 和实际传递效果调整 `transfer_rate`
2. 将涌现式聚类思想推广到跨拓扑关系发现：不依赖余弦相似度，而是让走边过程中的激活共现模式自然孕育新的跨层关联

---

## 三、优先级排序

| 优先级 | 问题 | 理由 |
|--------|------|------|
| P0 | 02.3 测试覆盖 | 影响每次代码改动的验证效率和质量保障 |
| P1 | 02.2 配置管理 | 跨平台部署的刚需，且实现成本低（读 JSON 即可） |
| P1 | 02.4 脑区生命周期 | 与 FPGA 路线图直接相关，丘脑框架已有基础 |
| P2 | 02.1 双子系统桥接 | 架构升级的关键，但需要在方向明确后动手 |
| P2 | 02.5 跨拓扑质量 | 长期优化，随着训练规模增长价值越来越高 |

---

## 四、关联文件

| 文件 | 涉及问题 |
|------|---------|
| `src/bptt_learner.c` | 02.1 — 现有的唯一桥接点 |
| `src/cognitive_controller.c` | 02.1, 02.2 — 参数集中地 + 意图向量入口 |
| `src/multi_topology.c` | 02.3, 02.5 — 走边评分 + 跨拓扑重建 |
| `src/dialog_system.c` | 02.3 — 对话管线最大盲区 |
| `src/diffusion.c` | 02.3 — 扩散引擎无测试 |
| `demos/pivotmind_gateway.c` | 02.4 — 全量初始化 |
| `include/constants.h` | 02.2 — 常量分散最严重 |
| `include/pretrain.h` | 02.2 — 预训练参数硬编码 |
| `src/huarong_topology.c` | 02.5 — 跨拓扑边管理 |

---

## 五、审查范围

本次审查基于 `/home/cx/pivotmind` 完整代码库扫描，覆盖：
- 85 个源文件 (~48,600 行) + 89 个头文件 (~12,600 行)
- 13 个脑区模块全部已读
- 构建系统、测试、工具、changelogs 全部已核

审查方法：静态代码分析 + 架构文档对照 + 数据流追踪，未涉及运行时性能 profiling。

---

## 六、修复完成记录

> 完成日期: 2026-06-27

### P0: 测试覆盖 — 已完成 ✅
- 提交: `5e56827`
- `tests/unit/test_dialog.c` — 5 项测试 (input create/destroy, empty, NULL, 单字, 标点)
- `tests/unit/test_diffusion.c` — 3 项测试 (NULL safety, uninitialized master, constants)
- Makefile: 新增 test-dialog-unit, test-diffusion-unit 目标
- 全部 8/8 测试通过

### P1: 统一配置系统 — 已完成 ✅
- 提交: `9c60dbf`
- `include/json_config.h` — ConfigContext 结构体 (topology/learning/inference/clock/brain_regions)
- `src/json_config.c` — 最小化 JSON 解析器 + config_load/config_write_default
- `demos/pivotmind_gateway.c` — 启动时加载配置
- 配置文件缺失时全部回退 constants.h 默认值

### P1: 脑区生命周期管理 — 已完成 ✅
- 提交: `1137ead`
- `include/thalamus.h` / `src/thalamus.c` — enabled[9] 标志 + enable_region/is_region_enabled API
- `src/brainstem.c` — perception/DMN/hippocampus/cerebellum/hypothalamus/broca tick 检查 enabled
- `demos/pivotmind_gateway.c` — 启动时按配置禁用脑区

### P2: BPTT 桥接增强 — 已完成 ✅
- 提交: `1137ead`
- `include/bptt_learner.h` / `src/bptt_learner.c` — 新增 bptt_get_confidence()
- `include/cognitive_controller.h` / `src/cognitive_controller.c` — nn_confidence 字段 + compute_intent nn 因子

### P2: 跨拓扑连接质量重评估 — 已完成 ✅
- 提交: `1137ead`
- `include/multi_topology.h` / `src/multi_topology.c` — 新增 master_reevaluate_cross_links()
- `src/brainstem.c` — synapse_scale 每 600 tick 调用重评估

### P0+: 回归测试套件 — 已完成 ✅
- 提交: `4e001fa`
- `tests/regression/run_all.py` — 主入口 (--smoke/--response/--filter/--stress)
- `tests/regression/test_smoke.py` — 启动/健康/状态/基础对话 (11 项)
- `tests/regression/test_response.py` — 回复质量检查 (12 项)
- `tests/regression/test_filter.py` — 虚词过滤验证 (6 项)
- `tests/regression/test_stress.py` — 并发稳定性 (3 项)
- 全部 32/32 测试通过，冷启动兼容

