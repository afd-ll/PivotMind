# 0.0.7 — GRU/LSTM 反向传播实现

> **日期**: 2026-05-27 | **类型**: 新增

## 概述

实现 GRU 和 LSTM 层的完整 BPTT（Backpropagation Through Time）反向传播。

## 修复内容

### GRU 反向传播
- `GRULayer` 增加 `x_t` 缓存 + 6 个偏置梯度字段
- `gru_forward_step` 修复 `h_prev` 泄漏 + 缓存 `x_t`
- `gru_backward_step` 实现完整反向传播
- `gru_backward_sequence` 实现 BPTT

### LSTM BPTT
- `LSTMLayer` 增加 `dh_prev_out`/`dc_prev_out`
- `lstm_backward_step` 存储链式梯度
- `lstm_backward_sequence` 实现 BPTT 循环

✅ 0 error 0 warning。
