# 0.4.7 — 架构审查全量修复 (P0+P1+P2)

> **日期**: 2026-06-27 | **类型**: 新增 + 修复

## 概述

基于 [044 架构审查报告](../reports/044-architecture-review.md)，实现 5 项优化：
- P0: 对话+扩散单元测试
- P1: 运行时 JSON 配置系统 + 脑区生命周期管理
- P2: BPTT 桥接到认知调度 + 跨拓扑连接重评估

## 核心变更

### P0 — 测试覆盖
- `tests/unit/test_dialog.c` — 5 项对话输入解析测试
- `tests/unit/test_diffusion.c` — 3 项扩散引擎安全测试
- Makefile: 新增 test-dialog-unit, test-diffusion-unit 目标

### P1 — 配置系统
- `include/json_config.h` + `src/json_config.c` — ConfigContext 结构体 (topology/learning/inference/clock/brain_regions)
- 最小化 JSON 解析器，零外部依赖
- 网关启动时可选加载 pivotmind_config.json，缺失则回退 constants.h 默认值

### P1 — 脑区生命周期
- `thalamus.h/c`: enabled[9] 标志 + thalamus_enable_region / is_region_enabled API
- `brainstem.c`: perception/DMN/hippocampus/cerebellum/hypothalamus/broca tick 检查 enabled
- `pivotmind_gateway.c`: 启动时按配置禁用脑区

### P2 — BPTT 桥接
- `bptt_learner.h/c`: bptt_get_confidence() — loss 归一化 → 0.0~1.0
- `cognitive_controller.h/c`: nn_confidence 字段 + compute_intent 增加 nn 因子 (1.0 + 0.1 * confidence)

### P2 — 跨拓扑重评估
- `multi_topology.h/c`: master_reevaluate_cross_links()
- `brainstem.c`: synapse_scale 每 600 tick 调用
- 公式: transfer_rate = 0.4 + 0.6 * norm(use_count) * weight

### 杂项
- CONTEXT.md 删除 (AB 分板规则过时)
- changelogs/README 新增「报告规则」章节
- README×5 语言版本全部重写至 v0.4.7
- 版本号统一: pivotmind_version.h → v0.4.7

## 改动文件

| 文件 | 变更 |
|------|------|
| `tests/unit/test_dialog.c` | 新增 |
| `tests/unit/test_diffusion.c` | 新增 |
| `include/json_config.h` | 新增 |
| `src/json_config.c` | 新增 |
| `reports/044-architecture-review.md` | 新增 |
| `changelogs/044-architecture-review-full-fix.md` | 新增 (本文件) |
| `demos/pivotmind_gateway.c` | config 集成 + 脑区禁用 |
| `include/thalamus.h` + `src/thalamus.c` | enabled 标志 + API |
| `src/brainstem.c` | 5 处 tick 检查 enabled + 重评估调用 |
| `include/bptt_learner.h` + `src/bptt_learner.c` | bptt_get_confidence |
| `include/cognitive_controller.h` + `src/cognitive_controller.c` | nn_confidence |
| `include/multi_topology.h` + `src/multi_topology.c` | master_reevaluate_cross_links |
| `include/pivotmind_version.h` | 0.4.3 → 0.4.7 |
| `Makefile` | 2 个新测试目标 |
| `README.md` ×5 | 全部重写 |
| `changelogs/README.md` | 报告规则 |
| `CONTEXT.md` | 删除 |

## 编译验证

```
make clean && make gateway
→ 全部 86 个源文件零错误零警告
→ 仅标准 LTO 序列化警告
→ pivotmind_gateway 链接成功
→ test-dialog-unit: 5/5 通过
→ test-diffusion-unit: 3/3 通过
```
