# 0.4.10 — POS 词性重排接入扩散引擎输出

> **日期**: 2026-07-01 | **类型**: 新增

## 概述

将涌现状词类系统（EmergentPOS）接入扩散引擎输出管线。扩散生成的候选词在输出前按词性优先级重排（NOUN→VERB→ADJ→ADV→OTHERS），使输出序列符合中文自然语序。修复前次 heap-alloc POS 数组导致的 segfault/abort。

## 核心变更

### 1. DiffusionCtx 新增 EmergentPOS 引用

**文件**: `include/diffusion.h` + `src/diffusion.c`

- `DiffusionCtx` 新增 `EmergentPOS* emergent_pos` 字段，前向声明避免循环依赖
- `diffusion_init()` 初始化为 NULL（调用者可选注入）

### 2. 扩散输出 POS 重排

**文件**: `src/diffusion.c` (第4步输出组装)

两遍算法，全部栈分配（零 heap），彻底避免 segfault：

**第一遍** — 收集有效候选词 + POS 标注：
- 遍历 `final[]` 候选，过滤垃圾词/虚词/重复
- 调用 `emergent_pos_tag(ctx->emergent_pos, ctx->master, word)` 标注词性
- 结果存入栈数组 `word_buf[32]` + `word_pos[32]`（384 字节）

**第二遍** — 按词性优先级输出：
- 优先级顺序：NOUN → VERB → ADJ → ADV → NUM → PRON → PREP → CONJ → PARTICLE → INTERJ → UNKNOWN
- 同词类内保持原有扩散评分序

**降级路径**：`ctx->emergent_pos == NULL` 时回退原始模板连接词行为（PFE 路径等无控制器的调用点）。

### 3. 散布函数签名更新

| 文件 | 变更 |
|------|------|
| `include/cingulate.h` | `cingulate_diffusion_evaluate()` 新增 `EmergentPOS* emergent_pos` 参数 |
| `src/cingulate.c` | 将参数设置到 `dctx.emergent_pos` |
| `src/prefrontal.c` | 传入 `pf->controller->emergent_pos` |
| `src/prefrontal_executive.c` | 传入 `NULL`（PFE 无 CognitiveController） |

### 4. test_chinese 跨平台修复

**文件**: `tests/unit/test_chinese.c`

- `windows.h` / `SetConsoleOutputCP` 改为 `#ifdef _WIN32` 条件编译
- 中文字符替换为 ASCII 等价描述（避免 Linux CI 编码问题）

## 改动文件

| 文件 | 变更 |
|------|------|
| `include/diffusion.h` | 修改 — 新增 `EmergentPOS* emergent_pos` 字段 + 前向声明 |
| `src/diffusion.c` | 修改 — 引入 `emergent_pos.h` + init 初始化 + 输出组装重写为 POS 重排 |
| `include/cingulate.h` | 修改 — 函数签名加 `EmergentPOS*` 参数 |
| `src/cingulate.c` | 修改 — 接受并传递 `emergent_pos` 到 `dctx` |
| `src/prefrontal.c` | 修改 — 传入 `pf->controller->emergent_pos` |
| `src/prefrontal_executive.c` | 修改 — 传入 `NULL` |
| `tests/unit/test_chinese.c` | 修改 — 跨平台条件编译修复 |

## 编译验证

```bash
make clean && make -j4
```

全部源文件编译通过（86 .c + 对应 .h），零新增警告。

## 测试验证

| 测试 | 结果 |
|------|------|
| `test_diffusion_unit` | 3/3 PASS |
| `test_model` | 10/10 PASS |
| `test_metrics` | 14/14 PASS |
| `test_chinese` | 编译运行通过 |
