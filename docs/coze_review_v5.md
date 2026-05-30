# PivotMind 代码审查报告 v5

审查时间：2026-05-26 20:22
代码版本：afd-ll/PivotMind @ 090bf345（6 commits ahead of e603fc8）
审查方式：GitHub API 源码分析（网络原因无法 git pull）

---

## 一、总体结论

**6 个新提交修复了 2 个问题（sentinel 回归 + batch_learn），但引入了 2 个新的 P0 级 bug（BPTT 实现错误、Linear 层权重梯度缺失）。**

| 等级 | 数量 | 状态 |
|------|------|------|
| P0 | 3 | 1 修复，2 新增 |
| P1 | 1 | 新增：Linear 层权重梯度缺失 |

---

## 二、已确认修复

### ✅ sentinel 兼容回归（7dd44253）

commit `7dd44253` 完全按审查建议修复：

```c
// 正确版本
if (sentinel == 0xDEADBEEF) {
    // 新格式：有 sentinel，读取 cross_link_count
    if (fread(&expected_cross_count, sizeof(int), 1, fp) != 1)
        expected_cross_count = 0;
} else {
    // 旧格式：回退 4 字节
    fseek(fp, -(long)sizeof(uint32_t), SEEK_CUR);
    expected_cross_count = 0;
}
```

---

## 三、新增 P0 Bug：BPTT 实现错误

**位置：** `src/layer_rnn_backward.c` L1-115

`bptt_learner.c` 调用 RNN 反向传播进行序列学习（输入是用户对话每个字的特征向量，目标 AI 回复每个字的特征向量），但 `layer_rnn_backward` 的 BPTT 实现有根本性错误：

### Bug 1：所有时间步用零向量代替隐藏状态

```c
// layer_rnn_backward.c L93-100
for (int t = seq_len - 1; t >= 0; t--) {
    float* h_prev_zero = (float*)calloc(hidden_size, sizeof(float));
    float* h_t = hidden_data;   // ← 始终用最终隐藏状态
    float* h_prev = h_prev_zero; // ← 始终用零向量！
    ...
}
```

前向传播在每个时间步保存了 `h_prev`，但反向传播中 `h_prev` 被**始终设为零向量**。

- tanh 梯度 `tanh_grad[h] = 1 - h_t[h]^2` 使用的是**最后一个时间步**的隐藏状态，而非每个时间步各自的隐藏状态
- BPTT 的核心是通过链式法则把误差按时间步反向传播，正确做法是保存每个时间步的隐藏状态
- 对于 `seq_len > 1` 的情况，BPTT 完全失效

### Bug 2：d_hidden 梯度替换而非累积

```c
// L106-110
for (int h = 0; h < hidden_size; h++) {
    d_hidden_data[h] = d_h_prev[h];  // ← 替换，不是累积
}
```

每轮反向传播用 `d_h_prev` **替换** `d_hidden_data`，而非追加。当 t 从 seq_len-1 遍历到 0 时，后面时间步的梯度会覆盖前面的。对于 seq_len > 1 的情况，**只保留了最后一个有效时间步的梯度**。

### 影响

`bptt_learner` 对话学习（用每个字符的特征向量训练 RNN）实际上只训练了 seq_len=1 的情况，长序列的 BPTT 完全失效。

### 建议修复

1. 前向传播时保存所有时间步的隐藏状态（`data->outputs` 已有但未使用）
2. 反向传播时从后向前依次使用各时间步的真实隐藏状态
3. 将 `d_hidden_data[h] = d_h_prev[h]` 改为 `d_hidden_data[h] += d_h_prev[h]`（如果需要保留历史梯度）

---

## 四、新增 P1 Bug：Linear 层权重梯度缺失

**位置：** `src/layer.c` L214-251

`layer_linear_backward` 只计算了偏置梯度，**没有计算权重梯度**：

```c
// layer_linear_backward L244-246
// 注意:完整的权重梯度需要输入数据
// grad_weights = input^T @ grad_output
// 由于没有保存输入，这里返回NULL调用者可以自行计算
```

bptt_learner 中 Linear 层（hidden_dim → input_dim）的权重无法通过 BPTT 更新。

`layer_rnn_backward` 也有同样问题：没有保存中间激活，`d_Wh` 和 `d_Wx` 的梯度在 `seq_len > 1` 时计算不准确。

---

## 五、其他变更简评

### ✅ 新增 BPTT 学习器框架

`bptt_learner.c`（237 行）整体框架正确：
- `text_to_features`：从词汇拓扑节点提取特征向量，未找到的字用零向量
- `bptt_learn_from_dialog`：前向 → MSE loss → 反向 → Adam 更新，流程完整
- `autonomic_learn_from_text`：新增批量文本自主学习接口

### ✅ cognitive_controller 性能优化

`cognitive_controller.c` 的 `causal_path_score` 函数将因果图反向查找从 O(n²) 优化为 O(n)：通过一次遍历构建 `topo_to_cg` 映射数组，实现常数时间查找。

### ⚠️ 暂未集成

- Dialog 三模块（intent/semantic/verify）仍未接入 cognitive_controller
- GRU/LSTM 已实现但 cognitive_controller 无调用接口

---

## 六、变更统计（6 commits vs e603fc8）

```
src/bptt_learner.c              新增 237 行（BPTT 学习器）
src/layer_rnn_backward.c        新增 161 行（RNN 反向传播）
src/cognitive_controller.c      +227 -62（O(n²)→O(n) 优化 + 常量参数化）
src/autonomic_learner.c         +183 -23（autonomic_learn_from_text + 边压制）
src/active_learner.c           +48 -10（feedback_correct 路径跟踪）
src/multi_topology.c            +24 -9（valence 激活因子 + cross_adj）
include/bptt_learner.h          新增 77 行
include/active_learner.h        +6 行（路径信息字段）
changelogs/009A-009B            +185 行（valence 架构文档）
```

---

## 七、修复优先级建议

| 优先级 | 问题 | 工作量 |
|--------|------|--------|
| P0 | BPTT：保存每步隐藏状态，修复 d_hidden 累积 | 中 |
| P0 | Linear 层：保存输入，计算权重梯度 | 小 |
| P1 | RNN BPTT：seq_len>1 时 Wx/Wh 梯度错误 | 中 |
| P2 | cognitive_controller 集成 GRU/LSTM | 大 |
