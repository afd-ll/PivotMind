# 0.4.7 — 新增回归测试套件

> **日期**: 2026-06-27 | **类型**: 新增

## 概述

新增 `tests/regression/` 回归测试套件，覆盖基础功能验证：
- 冒烟测试：启动/健康/状态/基础对话不崩溃
- 回复质量：非空检查、长度合理性、虚词比例
- 虚词过滤：确保虚词输入不泄漏到输出
- 压力测试：并发请求稳定性

## 核心变更

### 测试模块

| 模块 | 项目数 | 内容 |
|------|--------|------|
| `test_smoke.py` | 11 | 启动、健康、状态、基础对话不崩溃 |
| `test_response.py` | 12 | 10 题回复检查、冷启动兼容 |
| `test_filter.py` | 6 | 3 组虚词输入过滤验证 |
| `test_stress.py` | 3 | 20 并发请求 + 事后健康检查 |

### 设计特点
- **冷启动兼容** — 未训练时返回 "(无回应)" 视为正确
- **自动启停 gateway** — 每个模块独立启动/停止
- **退出码反映结果** — 可在 CI 中直接使用
- **独立可运行** — `--smoke` / `--response` / `--filter` / `--stress`

## 改动文件

| 文件 | 变更 |
|------|------|
| `tests/regression/run_all.py` | 新增 |
| `tests/regression/helpers.py` | 新增 |
| `tests/regression/test_smoke.py` | 新增 |
| `tests/regression/test_response.py` | 新增 |
| `tests/regression/test_filter.py` | 新增 |
| `tests/regression/test_stress.py` | 新增 |
| `tests/regression/questions.json` | 新增 |
| `tests/regression/README.md` | 新增 |

## 运行验证

```
python3 tests/regression/run_all.py
→ 32/32 全部通过, 耗时 ~38s
→ Smoke 11/11, Response 12/12, Filter 6/6, Stress 3/3
```
