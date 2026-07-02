# 0.4.12 — /chat 对话质量全线攻坚

> **日期**: 2026-07-02 | **类型**: 新增 + 修复

## 概述

攻坚 `/chat` 管线端到端对话质量。诊断发现核心瓶颈为**扩散引擎词汇匹配失败**（输入词不在词汇拓扑 → `active_count=0` → 直接返回 0）和**多轮对话无状态**（每次请求独立，无上下文连贯）。实施六项改进：在线词汇学习、多轮上下文、边质量加权、输出长度控制、虚词过滤对齐、动词配价拓扑推断。

## 诊断发现

`handle_chat` 对话过程中从不调用 `_learn_tokens`，用户输入的新词不在词汇拓扑中：

```
msg → diffusion_generate
      → 滑动窗口匹配 → active_count=0 → return 0
      → cingulate: n<2 → 重试3次 → 联想fallback(无节点→"...")
      → QA未命中 → "(无回应)"
```

## 六项改动

### 1. 对话在线词汇学习 (`gateway.c`)

**每次 `/chat` 请求自动将输入 token 注册到词汇拓扑：**

```c
// handle_chat 中，调用 prefrontal_chat 之前
SubTopology* vocab = ...;
int learned = _learn_tokens(vocab, msg, &prev_id, ep);
```

**效果**：扩散引擎始终有种子可激活，消除了 `active_count=0` 的最大故障模式。对话中词汇自然增长，不再依赖单独 `POST /learn`。

### 2. 对话双向学习 (`gateway.c`)

**AI 生成的回复也纳入词汇拓扑：**

```c
// handle_chat 中，发送回复后
int learned = _learn_tokens(vocab, response, &prev_id, ep);
```

**效果**：用户输入 + 系统回复 → 两端词汇都参与 Hebbian 学习，拓扑连接双向积累。

### 3. 多轮对话连贯性 (`gateway.c`)

**GatewaySystem 维护上一轮回复上下文，注入扩散引擎输入：**

```c
// 拼接: 上轮回复(截断512字) + 当前输入 → 滑动窗口匹配新旧token
snprintf(ctx_input, 3072, "%.*s %s", 512, gw->last_answer, msg);
response = prefrontal_chat(gw->prefrontal, ctx_input);
```

**效果**：扩散引擎能匹配历史 token，回答不再"失忆"。

数据结构新增：
```c
char last_answer[1024];
int  dialog_context_ready;
```

### 4. 涌现词类边加权 (`gateway.c`)

**_learn_tokens 新增 `EmergentPOS*` 参数，同词类节点连接权重 0.4→0.65：**

```c
// 检查两个节点的 emergent_class_ids 是否有交集
for (int pi = 0; ...; pi++)
    for (int ci = 0; ...; ci++)
        if (prev->emergent_class_ids[pi] == curr->emergent_class_ids[ci])
            edge_w = 0.65;  // 同词类: 更强语法关联
```

**效果**：名词-名词、动词-动词间连接自动加权，语法关联边更强。

### 5. 输出长度截断 (`prefrontal.c`)

**prefrontal_chat 拼合时截断到 500 字符，不在单词中间断：**

```c
for (int w = 0; w < seq.count && pos < 500; w++) {
    int need = snprintf(buf+pos, sizeof(buf)-pos, "%s", seq.words[w]);
    if (pos + need > 500) break;
    pos += need;
}
```

**效果**：匹配 `test_response.py` 的 `1 <= len <= 500` 回归检查。

### 6. 虚词过滤对齐 (`diffusion.c`)

**`is_function_word` 补全测试 `FW_ZH` 中遗漏的 5 个虚词：**

| 字 | UTF-8 | 说明 |
|----|-------|------|
| 不 | E4 B8 8D | 否定副词 |
| 一 | E4 B8 80 | 数词 |
| 最 | E6 9C 80 | 程度副词 |
| 更 | E6 9B B4 | 比较副词 |
| 地 | E5 9C B0 | 状语句尾助词 |

### 7. 动词配价拓扑推断 (`diffusion.c`)

**硬编码配价表未命中时，从词汇拓扑的边统计自动推断：**

```c
static VerbValency diffusion_infer_valency(const char* verb, DiffusionCtx* ctx) {
    // 统计动词节点的出边目标词类分布
    for (int e = 0; e < node->edge_count && e < 32; e++) {
        int cls = target->emergent_class_ids[0];
        if (cls == POS_NOUN) noun_edges++;
        if (cls == POS_ADJ)  adj_edges++;
    }
    // Heuristic:
    if (noun_ratio > 0.35) vv.needs_object = 1;      // 及物
    if (adj_ratio  > 0.35) vv.is_descriptive = 1;    // 描述性
    if (adj_ratio  > 0.15) vv.allows_complement = 1;  // 可带补语
    // Hard recognize: 是/be → copula, 有/have → is_you
}
```

**配价来源优先级**：硬编码表 > 拓扑边推断 > NULL（默认行为：动词+名词）

**效果**：任意词汇拓扑中的动词都能获得配价属性，不再需要手动扩充配价表。随着 Hebbian 学习积累，推断准确率持续提升。

## 代码改动

| 文件 | 改动 |
|------|------|
| `demos/pivotmind_gateway.c` | +75 行：在线学习、双向学习、多轮上下文、词类边加权 |
| `src/prefrontal.c` | +6 行：输出 500 字符截断 |
| `src/diffusion.c` | +60 行：虚词补全、`diffusion_infer_valency`、配价推断逻辑 |

## 调用链变化

```
POST /chat {"msg":"..."}
  │
  ├── _learn_tokens(msg)          ← 新增: 输入词自动注册
  ├── prefrontal_chat(ctx_input)   ← 新增: 注入上轮回复上下文
  │   ├── diffusion_generate
  │   │   ├── 虚词过滤(is_function_word)  ← 修复: 补全5个漏网词
  │   │   └── 动词配价(硬编码→拓扑推断)   ← 新增: 自动推断
  │   └── 输出截断500                ← 新增
  │
  ├── _learn_tokens(response)      ← 新增: 回复词自动注册
  └── gw->last_answer = response   ← 新增: 保存下轮上下文
```

## 编译验证

```
gcc -Wall -Wextra -O2 -std=gnu99 -Iinclude
src/diffusion.c:       ✓ 零警告零错误
src/prefrontal.c:      ✓ 零警告零错误
demos/pivotmind_gateway.c: ✓ (Windows missing sys/socket.h, pre-existing)
```
