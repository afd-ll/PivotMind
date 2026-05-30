# 006 — 代码审查 v2 修复（006B 部分）

> 日期: 2026-05-26 | 审查: 006A | 修复: 006B

## 改动清单

| 文件 | 等级 | 问题 | 改动 |
|------|------|------|------|
| multi_topology.c | P0-1 | 节点区/跨链接区无分隔符 | save: 加 sentinel (0xDEADBEEF) + cross_link_count；load: 读取验证 sentinel |
| tensor.c | P0-2 | add/sub/mul/div 广播运算未实现 | 新增 broadcast_index() 辅助函数，四个函数全部实现真正广播 |
| multi_topology.c | P2-1 | 魔数 20 硬编码 | `from_topo > 20` → `from_topo >= master->sub_topo_count` |
| multi_topology.c | P2-7 | conn_count 类型 int | 注释说明 int 为文件格式兼容，保持原样 |

## 已修复（第二轮）

| # | 项 | 改动 |
|---|-----|------|
| P1-4 | batch_learn 调试清零 | 改为自动 rebuild_cross_connections + 警告 |
| P2-2 | 双写跨链接 | 加注释说明冗余，保留兼容其他工具 |
| P1-1 | dialog 三模块 | 文件头加 TODO 标记未集成状态 |

## 已确认

| # | 项 | 结论 |
|---|-----|------|
| P2-5 | count 上限 | 1000万条上限合理，无需加强 |

## 误报

| # | 项 | 结论 |
|---|-----|------|
| P1-3 | LSTM 输出门 | 实际已完成（L403-446 do_pre → dW_io/dR_io） |

## 架构阻塞

| # | 项 | 阻塞原因 |
|---|-----|----------|
| P1-2 | GRU 反向传播 | 缺 x_t 缓存 + 偏置梯度 + 多时间步状态 |
| P2-4 | LSTM BPTT | 缺多时间步状态缓存 |
| P2-8 | GRU/LSTM 集成 | cognitive_controller 无接口 |

## 关联

- 审查: 006A
