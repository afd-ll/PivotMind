# 0.0.13 — 集成缺口与一致性修复

> **日期**: 2026-05-27 | **类型**: 修复

## 概述

012 全量修复后的集成验证——发现剪枝函数未被调用（死代码）、状态文件 NULL 槽位、权重不一致等 4 项问题。

## 修复内容

| 优先级 | 文件 | 改动 |
|--------|------|------|
| **P0** | `src/autonomic_learner.c` | 刷盘前调用两个剪枝函数（内部边 conf<0.05 且 \|w\|<0.02；跨拓扑 w<0.05 且 use<2） |
| **P0** | `src/multi_topology.c` | `master_save_state` 写入前统计实际非 NULL link 数 |
| P1 | `src/cognitive_controller.c` | `get_intent_base_weights` 默认分支与 create 一致 |
| P1 | `src/dialog_system.c` | 删除重复 `#include "string_pool.h"` |

✅ 编译通过，0 error 0 warning。
