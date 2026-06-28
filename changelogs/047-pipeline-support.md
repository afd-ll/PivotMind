# 0.4.9 — QA 记忆检索 + 训练管线 + 词汇清理工具

> **日期**: 2026-06-27 | **类型**: 新增 + 工具

## 概述

新增 QA 记忆检索模块（QAMemory）作为扩散引擎的 fallback 机制：扩散产出太短/无回应时自动检索预存 QA 对。配套新增 QA 语料集（xiaohuangji 50 万 + egret-wenda）、训练/喂料脚本、词汇污染清理工具，以及 GPU 可用性验证工具。

## 核心变更

### 1. QA 记忆检索模块

**新增文件**: `src/qa_memory.c` (155 行) + `include/qa_memory.h` (30 行)

- 轻量 QA 对存储：从 `Q|A` 格式的 pipe 文件加载
- Token 交集评分：对输入 UTF-8 分词后，计算与每条 Q 的 token 交集，用 `match / sqrt(q_len + 1)` 打分
- 返回最高分 A 文本，有最低阈值防噪音匹配
- FAQ 式检索：输入"什么是AI" → 命中匹配的 Q → 返回预设 A
- `MAX_TOKENS=64`、`MAX_QA_LINE=512`

**API**:
- `qa_memory_create(pipe_path, max_entries)` — 从文件加载
- `qa_memory_query(m, input)` — 检索最佳匹配
- `qa_memory_destroy(m)` / `qa_memory_count(m)`

### 2. Gateway 集成

**文件**: `demos/pivotmind_gateway.c`

- 引入 `#include "qa_memory.h"` (第 43 行)
- Context 结构体新增 `QAMemory* qa_memory` 字段 (第 86 行)
- 后续在 `/chat` 流程中作为扩散引擎的 fallback（扩散无产出时检索 QA 对）

### 3. QA 语料集

**新增目录**: `corpus/` (~55 MB, 23 个文件)

| 语料 | 文件 | 规模 |
|------|------|------|
| 小黄鸡 QA | `xiaohuangji_pipe.txt` | ~50 万对 (21.7 MB) |
| 小黄鸡 5k 精选 | `xhj_5k.txt` | 5,000 对 |
| 小黄鸡原始格式 | `xiaohuangji50w_nofenci.conv` | 22.4 MB |
| egret-wenda 问答 | `qaq_corpus_pipe.txt` | 504 对 (Q\|A) |
| egret-wenda JSON | `qaq_corpus.json` | 2,017 行 |
| 领域 YAML (18 个) | `ai.yml`, `emotion.yml`, `psychology.yml`… | 各领域语料 |

### 4. 训练/喂料脚本

| 脚本 | 行数 | 功能 |
|------|------|------|
| `scripts/train_qa.py` | 96 | QA 语料训练：逐条送入 `/learn`，Q+A 合并 token 建词汇拓扑 |
| `scripts/feed_all.py` | 161 | 后台喂料（10 轮）— 每轮遍历书籍 + 抓百度/搜狗新闻 |

`train_qa.py` 用法: `python3 scripts/train_qa.py [--limit N] [--port P]`
`feed_all.py` 配置: `BOOK_DIR=~/本地书库`, `CHUNK_SIZE=300`, `ROUNDS=10`

### 5. 词汇污染清理工具

**新增文件**: `tools/clean_vocab.c` (144 行)

- 清理 strtok 旧版 /learn 产生的长中文节点（整句 token → 污染节点）
- 规则：含中文且字节数 > 6（即超过 2 个汉字）→ 删除
- 遍历全部 11 个子拓扑，调用 `huarong_net_dynamic_remove_node`
- 支持 `--dry-run` 模式（仅统计不写入）
- 编译: `gcc -O2 -Iinclude -I. tools/clean_vocab.c libpivotmind.a -lm -lpthread -o tools/clean_vocab`

### 6. 压力测试脚本

**新增文件**: `stress_test.py` (115 行)

- 三阶段递增并发：10并发×50 → 20并发×200 → 50并发×500
- 多线程 (ThreadPoolExecutor)，POST `/chat` 接口
- 实时进度报告、QPS 统计
- URL 默认 `localhost:8080`

### 7. GPU 可用性验证

**新增文件**: `gpu_test.c` (156 行) + `gpu_test` (已编译)

- 测试 EAIDK-610 Mali-T860 GPU 的 OpenGL ES 3.1 Compute Shader 能力
- EGL 无窗口初始化 → GLES 3.1 Compute Shader 编译/链接/执行 → 结果验证
- 编译: `gcc -o gpu_test gpu_test.c -lEGL -lGLESv2 -lm`
- 结论: Mali-T860 通过 Compute Shader 可用

### 8. 运行时配置

**新增文件**: `pivotmind_config.json`

```json
{"brain_regions": {"perception": false}}
```

- 支持脑区开关（当前: 关闭感知皮层）
- 由 JSON 配置系统（044/P1 新增）加载

### 9. 状态文件

**文件**: `pivotmind_state.dat` — 69 MB → 1.3 MB

- 经 clean_unicode_escapes + clean_vocab 清理后重保存
- 移除脏节点和 UTF-8 膨胀

## 改动文件

| 文件 | 变更 |
|------|------|
| `src/qa_memory.c` | **新增** — QA 记忆检索模块 |
| `include/qa_memory.h` | **新增** — QA 记忆接口 |
| `demos/pivotmind_gateway.c` | **修改** — 集成 QAMemory (+2 行) |
| `corpus/` | **新增** — QA 语料集 (23 文件, 55 MB) |
| `scripts/train_qa.py` | **新增** — QA 语料训练脚本 |
| `scripts/feed_all.py` | **新增** — 后台喂料脚本 |
| `tools/clean_vocab.c` | **新增** — 词汇污染清理工具 |
| `stress_test.py` | **新增** — gateway 压力测试 |
| `gpu_test.c` | **新增** — GPU Compute Shader 验证 |
| `pivotmind_config.json` | **新增** — 运行时脑区配置 |
| `pivotmind_state.dat` | **修改** — 清理后重保存 (69 MB → 1.3 MB) |

## 编译验证

QA 记忆模块编译（需集成到 Makefile 后测试）:
```bash
gcc -c -Iinclude -O2 -std=gnu99 src/qa_memory.c -o build/qa_memory.o
```

词汇清理工具:
```bash
gcc -O2 -Iinclude -I. tools/clean_vocab.c libpivotmind.a -lm -lpthread -o tools/clean_vocab
```

GPU 测试:
```bash
gcc -o gpu_test gpu_test.c -lEGL -lGLESv2 -lm
```
