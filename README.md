# 玄枢 PivotMind · 溯智网络认知引擎

**A Brain-Inspired Semantic Association Engine** — Pure C, Zero AI Framework Dependencies.

> 智能不是矩阵乘法的堆叠，而是激活在溯智网络中蔓延的涟漪。
> Intelligence is not a stack of matrix multiplications, but ripples of activation spreading through a reasoning network.

---

[中文](#中文) | [English](#english)

---

# 中文

## 是什么

玄枢是一套基于**溯智网络（HuarongTopologyNet）+ 赫布学习 + 多层扩散推理**的认知引擎。没有 Transformer，没有 embedding 向量，没有反向传播。只有节点、边、激活、衰减——以及一个永不停歇的后台时钟。

**当前版本：v0.4.0**

### 溯智网络

每个概念是一个节点，共现即建边。边携带**权重 × 置信度 × 动机倾向**三维属性。10 个子拓扑（词汇/语义/情绪/语法/上下文/领域/语用/文化/概念/模板）各为一张独立的溯智网络，通过跨拓扑连接形成多拓扑架构。激活沿多层同时扩散，竞争胜出者构成输出。

### 为什么这样做

| 传统 LLM | 玄枢 |
|----------|------|
| Token 预测，无状态 | 节点激活，有连续内部状态 |
| 梯度离线批量训练 | 赫布在线实时学习 |
| 单一 Embedding 空间 | 10 个子拓扑独立学习 |
| 神经网络黑盒 | 节点-边显式路径，完全可追溯 |
| 需要 GPU + 大量显存 | 仅 pthread + OpenMP，跑在 ARM 嵌入式板 |
| 推理与学习分离 | 对话即学习，学习即对话 |
| 无生理感知 | 内感受自检，三级健康响应 |

---

## 脑区架构

玄枢按哺乳动物大脑皮层的功能分区建模，9 个脑区各司其职，通过丘脑信号总线通信：

```
                          ┌──────────────────────┐
                          │   前额叶 Prefrontal    │ ← 对话/决策入口
                          │  + 前额叶执行器 PFE    │ ← 推理编排引擎
                          └──────────┬───────────┘
                                     │ 信号总线
        ┌────────┬────────┬─────────┼─────────┬────────┬────────┬────────┐
        ▼        ▼        ▼         ▼         ▼        ▼        ▼        ▼
   ┌────────┐┌──────┐┌──────┐┌──────────┐┌──────┐┌──────┐┌──────┐┌──────────┐
   │海马体   ││ DMN  ││杏仁核 ││ 感知皮层  ││布罗卡 ││ 小脑  ││脑干   ││ 下丘脑    │
   │记忆巩固 ││梦境  ││情绪   ││联网搜索   ││句式   ││微调   ││节律   ││需求/动机  │
   └────────┘└──────┘└──────┘└──────────┘└──────┘└──────┘└──────┘└──────────┘
                                     │
                          ┌──────────┴──────────┐
                          │    丘脑 Thalamus      │ ← 信号总线+资源门控
                          └─────────────────────┘
```

| 脑区 | 文件 | 职责 |
|------|------|------|
| **前额叶** (Prefrontal) | `prefrontal.c` | 对话生成，diffusion→ACC评估 |
| **前额叶执行器** (PFE) | `prefrontal_executive.c` | 推理编排：任务分解、子目标调度、冲突检测、综合输出 |
| **海马体** (Hippocampus) | `hippocampus.c` | 记忆巩固、QA重放、感觉皮层联动 |
| **DMN** | `dmn.c` | 默认模式网络：梦境联想、闲暇探索 |
| **杏仁核** (Amygdala) | `amygdala.c` | 情绪效价采样、探索/利用平衡 |
| **感知皮层** (Perception) | `perception.c` | 联网搜索、article_reader 语义理解管线 |
| **布罗卡区** (Broca) | `broca.c` | 模板自动构建与衰减调度 |
| **小脑** (Cerebellum) | `cerebellum.c` | BPTT微调、硬件资源保护 |
| **下丘脑** (Hypothalamus) | `hypothalamus.c` | 需求动态调控、昼夜耦合 |
| **丘脑** (Thalamus) | `thalamus.c` | 信号总线、资源门控、脑区间通信路由 |
| **脑干** (Brainstem) | `brainstem.c` | 节律心跳、激活衰减、自发激活、存盘调度 |
| **扣带回** (Cingulate/ACC) | `cingulate.c` | 四维序列评估（语义+模板+情绪+长度） |
| **想法竞技场** (IdeaArena) | `idea_arena.c` | 多候选五维竞争选择 |

---

## 核心机制

### 多层扩散引擎

输入经滑动窗口分词后，在四层网络同步扩散：

- **词汇层**：直接字面匹配，快速召回
- **语义层**：10 子拓扑跨层联想，触达相关概念
- **模板层**：识别句式模式，指导连接词插入
- **情绪层**：valence × arousal 加权，影响候选优先级

侧抑制机制保证输出多样性。

### 推理编排（PFE）

前额叶执行器自动判断问题复杂度，匹配 6 种推理模式：

| 模式 | 触发词 | 策略 |
|------|--------|------|
| DIRECT | 默认 | 单次扩散联想 |
| DECOMPOSE | 为什么/原因 | 定义→因果→综合 |
| COMPARE | 比较/区别 | 属性提取→对比 |
| HOWTO | 怎么/如何 | 前置条件→步骤序列 |
| ABDUCE | 如果/假设 | 基线→连锁反应 |
| ANALOGY | 类比/类似 | 结构映射 |

子目标递归分解（深度可配），冲突检测 + IdeaArena 竞争选出最佳路径，输出可解释推理链。

### 内感受自检

持续监测 RSS 内存、连接增速、推理延迟，三级响应：

| 级别 | 条件 | 动作 |
|------|------|------|
| 🟢 GREEN | 正常 | 正常运行 |
| 🟡 YELLOW | 预警 | 日志告警 + 提高学习门槛 |
| 🔴 RED | 紧急 | 存档 + 批量修剪弱边 |

---

## 快速开始

### 编译

```bash
# 需要 GCC + pthread + OpenMP（无其他依赖）
make all

# ARM 交叉编译
make CC=aarch64-linux-gnu-gcc all
```

### 启动

```bash
# 交互式网关（推荐）
./build/bin/pivotmind_gateway

# 命令行交互版
./build/bin/digital_life
```

网关默认监听 `:8080`。

### API 示例

```bash
# 提问
curl -X POST http://localhost:8080/ask \
  -H "Content-Type: application/json" \
  -d '{"query":"什么是意识？"}'

# 喂料学习
curl -X POST http://localhost:8080/learn \
  -H "Content-Type: application/json" \
  -d '{"text":"意识是大脑神经网络产生的主观体验。"}'

# 查看状态
curl http://localhost:8080/status
```

### 构建目标

| 命令 | 说明 |
|------|------|
| `make all` | 构建全部目标 |
| `make gateway` | 仅构建 HTTP 网关 |
| `make digital-life` | 构建命令行交互版 |
| `make seed-builder` | 构建种子拓扑工具 |
| `make batch-learn` | 批量训练工具 |
| `make clean` | 清理构建产物 |

---

## 项目结构

```
pivotmind/
├── src/               # 82 个核心源文件
├── include/           # 86 个头文件
├── demos/             # 网关与交互入口
├── tools/             # 训练/调试/数据处理工具
├── tests/             # 单元测试（23 项 PFE 测试 100% 通过）
├── scripts/           # 自动化脚本
├── changelogs/        # 版本变更记录
├── docs/              # 架构文档与图片
└── archived/          # 历史版本归档
```

---

## 版本历程

| 版本 | 亮点 |
|------|------|
| v0.1.x | 基础走边推理，竞争队列，状态持久化 |
| v0.2.x | 多层扩散引擎、海马体/DMN/感知皮层、内感受自检、突触缩放、认知调度中心 |
| **v0.3.0** | 前额叶执行器（6模式推理编排）、IdeaArena 五维竞争、策略权重自学习、递归分解、自适应调参、推理持久化 |
| **v0.4.0** | 代码简化（消除~200行重复）、脑区边界修复（11处统一查找）、Broca升级、下丘脑新脑区 |

> 详细变更：见 `changelogs/` 目录

---

## 已知局限

- **中文优先**：逐字拆分天然契合中文，英文/混合输入体验有限
- **回复流畅度**：联想路径输出的句子不如 LLM 自然，仍在迭代
- **无 GPU 加速**：纯 CPU + pthread + OpenMP
- **状态文件二进制**：不跨架构（x86_64 和 ARM 不互通）

---

## 长期目标

- [ ] FPGA 部署（终极目标：硬件级神经形态计算）
- [ ] 分布式多节点拓扑（跨设备激活传递）
- [ ] 视觉/听觉多模态输入接口
- [ ] 定时自动存档

---

*运行在 EAIDK-610（RK3399 ARM Cortex-A72，3.8GB RAM）*
*目标：打造可在嵌入式硬件上自主运行的分布式认知引擎*

---

# English

## What is PivotMind

PivotMind is a cognitive engine built on **HuarongTopologyNet + Hebbian Learning + Multi-Layer Diffusion Reasoning**. No Transformers. No embedding vectors. No backpropagation. Just nodes, edges, activation, and decay — powered by a relentless background clock.

**Current Version: v0.4.0**

### HuarongTopologyNet

Each concept is a node. Co-occurrence creates an edge. Edges carry a triple attribute: **weight × confidence × motivational bias**. Ten sub-topologies (vocabulary/semantic/emotion/syntax/context/domain/pragmatics/culture/concept/template) each form an independent reasoning network, interconnected through cross-topology links. Activation diffuses simultaneously across layers, with competition selecting the winner as output.

### Why This Approach

| Traditional LLM | PivotMind |
|----------|------|
| Token prediction, stateless | Node activation, continuous internal state |
| Gradient-based offline batch training | Hebbian online real-time learning |
| Single embedding space | 10 independent sub-topologies |
| Neural network black box | Explicit node-edge paths, fully traceable |
| Requires GPU + massive VRAM | pthread + OpenMP only, runs on ARM embedded boards |
| Inference separate from learning | Conversation IS learning |
| No physiological awareness | Interoceptive self-monitoring, 3-tier health response |

---

## Brain Region Architecture

PivotMind models mammalian cortical functional divisions — 9 brain regions, each with dedicated responsibilities, communicating through the Thalamus signal bus:

| Brain Region | File | Function |
|------|------|------|
| **Prefrontal** | `prefrontal.c` | Dialog generation, diffusion→ACC evaluation |
| **Prefrontal Executive (PFE)** | `prefrontal_executive.c` | Reasoning orchestration: task decomposition, subgoal scheduling, conflict detection, answer synthesis |
| **Hippocampus** | `hippocampus.c` | Memory consolidation, QA replay, perception coupling |
| **DMN** | `dmn.c` | Default Mode Network: dream association, idle exploration |
| **Amygdala** | `amygdala.c` | Emotional valence sampling, explore/exploit balance |
| **Perception** | `perception.c` | Web search, article_reader semantic pipeline |
| **Broca's Area** | `broca.c` | Template auto-building and decay scheduling |
| **Cerebellum** | `cerebellum.c` | BPTT fine-tuning, hardware resource protection |
| **Hypothalamus** | `hypothalamus.c` | Drive dynamics regulation, circadian coupling |
| **Thalamus** | `thalamus.c` | Signal bus, resource gating, inter-region communication routing |
| **Brainstem** | `brainstem.c` | Circadian heartbeat, activation decay, spontaneous activation, save scheduling |
| **Cingulate (ACC)** | `cingulate.c` | 4D sequence evaluation (semantic+template+emotion+length) |
| **IdeaArena** | `idea_arena.c` | Multi-candidate 5D competitive selection |

---

## Core Mechanisms

### Multi-Layer Diffusion Engine

Input is tokenized via sliding window, then diffuses simultaneously across four layers:

- **Vocabulary**: Direct literal matching, fast recall
- **Semantic**: Cross-topology association across 10 sub-topologies
- **Template**: Syntactic pattern recognition guiding connector insertion
- **Emotion**: valence × arousal weighting, modulating candidate priority

Lateral inhibition ensures output diversity.

### Reasoning Orchestration (PFE)

The Prefrontal Executive automatically assesses question complexity and matches one of 6 reasoning modes:

| Mode | Trigger Keywords | Strategy |
|------|-----------------|----------|
| DIRECT | default | Single diffusion association |
| DECOMPOSE | why/because | Definition→causality→synthesis |
| COMPARE | compare/difference | Attribute extraction→contrast |
| HOWTO | how to | Preconditions→step sequence |
| ABDUCE | what if/assume | Baseline→chain reaction |
| ANALOGY | analogy/similar | Structural mapping |

Subgoals are recursively decomposed (configurable depth). Conflict detection + IdeaArena competition selects optimal paths, producing explainable reasoning chains.

### Interoceptive Self-Monitoring

Continuously monitors RSS memory, connection growth rate, and reasoning latency with 3-tier response:

| Level | Condition | Action |
|------|-----------|--------|
| 🟢 GREEN | Normal | Normal operation |
| 🟡 YELLOW | Warning | Log alert + raise learning threshold |
| 🔴 RED | Critical | Emergency save + bulk prune weak edges |

---

## Quick Start

### Build

```bash
# Requires GCC + pthread + OpenMP (no other dependencies)
make all

# ARM cross-compilation
make CC=aarch64-linux-gnu-gcc all
```

### Run

```bash
# Interactive gateway (recommended)
./build/bin/pivotmind_gateway

# CLI interactive mode
./build/bin/digital_life
```

The gateway listens on `:8080` by default.

### API Examples

```bash
# Ask a question
curl -X POST http://localhost:8080/ask \
  -H "Content-Type: application/json" \
  -d '{"query":"What is consciousness?"}'

# Feed learning material
curl -X POST http://localhost:8080/learn \
  -H "Content-Type: application/json" \
  -d '{"text":"Consciousness is the subjective experience produced by neural networks in the brain."}'

# Check status
curl http://localhost:8080/status
```

### Build Targets

| Command | Description |
|------|------|
| `make all` | Build all targets |
| `make gateway` | Build HTTP gateway only |
| `make digital-life` | Build CLI interactive version |
| `make seed-builder` | Build seed topology tool |
| `make batch-learn` | Batch training tool |
| `make clean` | Clean build artifacts |

---

## Project Structure

```
pivotmind/
├── src/               # 82 core source files
├── include/           # 86 header files
├── demos/             # Gateway and interactive entry
├── tools/             # Training/debugging/data processing tools
├── tests/             # Unit tests (23 PFE tests, 100% pass)
├── scripts/           # Automation scripts
├── changelogs/        # Version changelogs
├── docs/              # Architecture docs and diagrams
└── archived/          # Historical version archives
```

---

## Version History

| Version | Highlights |
|------|------|
| v0.1.x | Basic walk reasoning, competitive queue, state persistence |
| v0.2.x | Multi-layer diffusion engine, Hippocampus/DMN/Perception, interoceptive monitoring, synaptic scaling, cognitive controller |
| **v0.3.0** | Prefrontal Executive (6-mode reasoning), IdeaArena 5D competition, strategy weight self-learning, recursive decomposition, adaptive parameter tuning, reasoning persistence |
| **v0.4.0** | Code simplification (~200 lines eliminated), brain region boundary fixes (11 unified lookups), Broca upgrade, Hypothalamus new brain region |

> Detailed changelogs: see `changelogs/` directory

---

## Known Limitations

- **Chinese-first**: Character-level tokenization naturally suits Chinese; English/mixed input experience is limited
- **Response fluency**: Associative path output is less natural than LLM-generated text (actively iterating)
- **No GPU acceleration**: Pure CPU + pthread + OpenMP
- **Binary state files**: Not cross-architecture (x86_64 and ARM are incompatible)

---

## Long-term Goals

- [ ] FPGA deployment (ultimate goal: hardware-level neuromorphic computing)
- [ ] Distributed multi-node topology (cross-device activation propagation)
- [ ] Visual/auditory multimodal input interfaces
- [ ] Scheduled auto-save

---

## License

Apache License 2.0

---

*Running on EAIDK-610 (RK3399 ARM Cortex-A72, 3.8GB RAM)*
*Goal: A self-sustaining distributed cognitive engine deployable on embedded hardware*
