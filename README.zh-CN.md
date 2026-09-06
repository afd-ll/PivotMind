<div align="center">

# 玄枢 PivotMind

### 纯 C 编写的、按脑区组织的认知引擎

**不依赖 Transformer 运行时 · 不使用预训练词向量 · 面向 ARM 板长期在线运行**

[English](README.md) · [简体中文](README.zh-CN.md)

[![版本](https://img.shields.io/badge/version-v0.5.24-blue.svg)](changelogs/)
[![许可证](https://img.shields.io/badge/license-Apache%202.0-green.svg)](LICENSE)
[![语言](https://img.shields.io/badge/C-99%2B-orange.svg)](https://en.wikipedia.org/wiki/C99)
[![平台](https://img.shields.io/badge/ARM-RK3399%20%7C%20x86__64-lightgrey.svg)](#快速开始)

</div>

---

## 这是什么

玄枢（PivotMind）是一个用 C 编写的认知引擎。不使用 Transformer 推理、预训练词向量或 AI 框架。它的基本学习回路是显式的：概念表示为节点，共现关系形成带权边，激活在多个拓扑中扩散，候选序列通过竞争产生输出。

项目围绕低资源设备上的持续在线学习设计。语料输入是主要学习信号；模型自身生成的输出不会被直接当作独立事实来源。

**当前版本：v0.5.24。** 这是一个处于玩具期的研究项目，不是大语言模型的替代品，也不是已经完成的生产级对话系统。

## 架构

### 以脑区命名的工程模块

脑区名称描述的是工程职责。它们是组织代码的类比，不代表软件已经完成了生物大脑模拟。每个名称都对应具体的 C 模块和运行时作用。

<p align="center"><img src="diagrams/brain-regions.png" alt="玄枢模块架构图" width="780"/></p>

| 模块 | 文件 | 职责 |
|---|---|---|
| 前额叶 | `prefrontal.c` | 对话生成与自适应门控 |
| 前额叶执行器 | `prefrontal_executive.c` | 六模式推理、任务分解、冲突检测 |
| 海马体 | `hippocampus.c` | 记忆巩固、回放、感知耦合 |
| DMN 默认网络 | `dmn.c` | 空闲联想与探索 |
| 杏仁核 | `amygdala.c` | 效价采样与探索/利用平衡 |
| 感知皮层 | `perception.c` | 搜索与文章阅读管道 |
| 布罗卡区 | `broca.c` | 模板构建与衰减调度 |
| 小脑 | `cerebellum.c` | 训练相关功能与资源保护 |
| 下丘脑 | `hypothalamus.c` | 四维驱力调节 |
| 丘脑 | `thalamus.c` | 信号总线、路由、资源门控、工具槽位 |
| 脑干 | `brainstem.c` | 心跳、衰减与自发激活 |
| 扣带回 ACC | `cingulate.c` | 综合语义、模板、效价和长度信号评估序列 |
| 想法竞技场 | `idea_arena.c` | 多候选竞争与侧抑制 |
| 网状结构 | `reticular.c` | 唤醒与警觉调节 |
| 视觉皮层 | `visual_cortex.c` | 帧、字幕与跨模态输入处理 |

### 溯智网络：12 层拓扑

玄枢使用 12 个拓扑空间：

`词汇 / 语义 / 情绪 / 语法 / 上下文 / 领域 / 语用 / 文化 / 概念 / 模板 / 视觉 / 主拓扑`

节点和边按拓扑存储，跨拓扑传播使用邻接索引。它不是查表系统：输入会激活网络的一部分，扩散引擎再把候选交给竞争阶段。

### 与传统 LLM 的差异

| 传统 LLM | 玄枢 |
|---|---|
| 在预训练参数上进行 Token 预测 | 在显式、可变的网络上进行节点激活 |
| 主要依赖离线梯度训练 | 从语料输入中进行在线共现学习 |
| 一个占主导地位的表示空间 | 多个承担不同职责的拓扑空间 |
| 面向请求的无持久状态推理 | 持久状态与持续后台活动 |

## 核心机制

### Hebbian 共现学习

共同出现的符号会加强彼此的连接。简化的更新形式是：

```text
w(i, j, t + 1) = w(i, j, t) + eta * cooccur(i, j)
```

实现中存在字符/词处理和晋升路径。新结构应由语料统计支持，而不是写入固定答案表。

### 扩散与竞争

扩散引擎是输出路径。激活衰减并在相关拓扑间传播，候选序列随后根据语义、模板、情绪/效价、长度和奖励相关信号进行竞争。实现位于 `src/` 与 `idea_arena.c`，不是远程模型调用。

### 涌现词性与模板结构

词性证据由喂料管道累积。POS 聚类和模板构建仍属于实验机制，需要通过生成结构质量来测量，尚未构成完整语法系统。

### 持久化与运行安全

- 使用临时文件加 rename 的原子存盘
- 由 systemd 管理的 HTTP 网关
- 部署环境中的内存保护与重启/加载验证
- 围绕学习队列、快照和拓扑访问持续修复并发问题

## 当前运行快照

以下数据于 2026-08-17 在 EAIDK-610 板上测得。这是一次重置/重建基线的快照，不是永久性能承诺。

| 指标 | 数值 |
|---|---|
| 版本 | `0.5.24` |
| 运行状态 | `running`；`/health` 返回 `ok` |
| 节点 | `17,105` |
| 拓扑 | `12` |
| 状态文件 | 约 `42.6 MiB` |
| 网关 RSS | 检查时约 `257 MiB` |
| 硬件 | RK3399、4 GB 内存、Armbian ARM64 |

此前的大状态文件已在 v0.5.21 重置基线中主动退役。README 旧版本中的 38 万+节点和 623 MB RSS 属于另一份状态，不能与当前快照直接比较。

## 快速开始

### 构建

```bash
# 需要 GCC、pthread、OpenMP、libcurl、OpenSSL 和 zlib
# 在 4 GB ARM 板上应限制并行度，不要使用 nproc 全核编译。
make -j1 gateway

# ARM 交叉编译
make CROSS_COMPILE=aarch64-linux-gnu- gateway

# 调试构建
make DEBUG=1 gateway

# 构建测试目标
make test
```

### 运行

```bash
# HTTP 网关，端口 8080
./build/bin/pivotmind_gateway 8080

# CLI 模式
./build/bin/pivotmind_cli
```

### API 示例

```bash
# 生成回复
curl -X POST http://localhost:8080/chat \
  -H "Content-Type: application/json" \
  -d '{"msg":"什么是政治？"}'

# 喂入外部学习材料
curl -X POST http://localhost:8080/learn \
  -H "Content-Type: application/json" \
  -d '{"msg":"你的学习文本"}'

# 查看运行状态
curl http://localhost:8080/status
curl http://localhost:8080/health
```

`/learn` 是主要学习入口。网络质量取决于输入语料以及当前正在评估的机制。

## 项目结构

```text
src/            引擎实现
include/        公共头文件
demos/          网关与 CLI 入口
tools/          状态和分析工具
tests/          单元测试与回归测试
changelogs/     版本历史
diagrams/       架构图
```

## 已知限制

- **生成语言默认不保证语法。** 当前输出仍主要是激活/邻近关系的结果，词层组装仍在开发中。
- **抽象概念尚未完整形成。** 语法和概念拓扑仍在积累结构，不能提供人类级别的抽象能力。
- **语料偏差会直接影响网络。** 网络反映它接收的材料；如果缺少现代口语材料，政治和历史文本就可能占据主导。
- **需要外部验证。** 内部生成可用于诊断和部分强化，但不能未经验证地替代语料证据。
- **项目处于玩具期。** 当前重点是通过实现和测量检验架构；不能从模块名称推断能力已经实现。

## 路线图

- 稳定重置后的分布状态并完成外部支撑度测量
- 恢复序列组装器，并在留出语料上评估
- 继续积累 POS 与语法拓扑结构
- 在把生成结构视为可靠结果前加入更强的支撑检查
- 核心运行时稳定后，探索具身/动作—观察学习

## 贡献

这是一个个人研究项目。欢迎提交 Issue 和 Pull Request。重大架构决策由作者负责并记录，提案前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。

## License

[Apache 2.0](LICENSE)
