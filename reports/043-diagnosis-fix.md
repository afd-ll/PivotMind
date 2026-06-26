# 043 — 扩散引擎虚词污染诊断与修复报告

> **日期**: 2026-06-26 | **状态**: 已完成

---

## 一、现象

**英文**：回复全是高频虚词串，如：
```
Q: What is your name?
A: benotthe是tohave在andin是youare在ofwill是...
```

**中文**：乱码或空回应。

系统有 30000 节点、57 次对话历史，但对话质量完全不可用。

---

## 二、诊断过程

### 2.1 引擎识别

网关 `/chat` 实际走的是 **扩散引擎 (Engine B)**，调用链：

```
gateway → prefrontal_chat() → cingulate_diffusion_evaluate() → diffusion_generate()
```

不是 walk-based 的 dialog_system (Engine A)。

### 2.2 扩散引擎管线分析 (`diffusion.c`)

```
输入分词 → vocab 匹配 → activation += 0.2
         → 3跳扩散 (vocab↔semantic, vocab→template)
         → 加权排序: total = vocab*0.45 + sem*0.25 + tpl*0.20 + emo*0.10
         → 模板导向 (冷系统下模板为空 → 使用回退连接词)
         → 输出
```

### 2.3 定位两个根因

**根因 1：虚词劫持整个管线**

- 活跃集更新 (line 378-385)：扩散每轮取 vocab 得分最高的 top-K 作为下轮种子，高频虚词天然高分
- 加权排序 (line 293-311)：虚词在候选表中排列靠前，内容词被挤到后面
- 输出 (line 365-392)：只过滤 strlen<2 和 @ 符号，虚词通行无阻

**根因 2：跨层索引张冠李戴**

```c
// line 300-302：i 是 vocab 节点下标，sn/tn/en 是其他拓扑节点数
sem_scores[i % sn];  // 无意义映射，随机噪声
```

vocab→semantic→vocab 的跨层回流已通过 `_cross_by_name()` 写入 `vocab_scores[]`，独立索引不仅冗余而且错误。

### 2.4 关联影响评估

| 改动 | 序列化 | 模板系统 | 学习系统 | 网关 |
|------|--------|---------|---------|------|
| 虚词过滤 | 安全 | 正面受益 | 正面受益 | 安全 |
| 跨层索引修复 | 安全 | 无影响 | 无影响 | 安全 |

详细分析见 043 对话记录。

---

## 三、修复方案

**只改一个文件 `src/diffusion.c`**，三处插入虚词过滤，一处移除错误索引：

### 3.1 新增 `is_function_word()` 

中英文虚词 ~130 个（英文 ~100 + 中文 ~70）。

### 3.2 活跃集更新过滤

扩散每轮的 top-K 活跃集跳过虚词，避免污染后续传播。

### 3.3 评分阶段过滤 + 索引修复

- 候选表收集时跳过虚词
- `i % sn/tn/en` 移除，`total_score` 直接使用 `vocab_scores[i]`（已含跨层回流）

### 3.4 输出阶段兜底过滤

在 `concept_is_printable` 等价检查后增加 `is_function_word` 检查。

### 3.5 最坏情况

候选表为空时 `prefrontal_chat` 返回 NULL，网关输出 `{"reply":"(无回应)"}` —— 比输出垃圾好。

---

## 四、改动文件

| 文件 | 行数变化 | 说明 |
|------|---------|------|
| `src/diffusion.c` | +~95 | 新增 `is_function_word()` + 三处过滤 + 索引修复 |
| `changelogs/043-diffusion-function-word-filter.md` | 新增 | 改动报告 |

---

## 五、编译与运行验证

```
make clean && make
→ src/diffusion.c 零错误零警告
→ pivotmind_gateway 链接成功
→ 网关启动，监听 19531 正常
```

冷启动状态下节点少（45个），返回 "(无回应)" 符合预期。效果验证需积累语料后测试。

---

## 六、提交信息

```
b35f6a4 fix: diffusion 虚词过滤 + 跨层索引修复 (0.4.7)
```

已推送至 forge (`/srv/forge-git/pivotmind.git`)。
