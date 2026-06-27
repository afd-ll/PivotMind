# PivotMind Regression Tests

回归测试套件，验证基础功能在代码改动后不会崩溃或退化。

## 运行

```bash
# 全部测试
python3 tests/regression/run_all.py

# 仅冒烟测试
python3 tests/regression/run_all.py --smoke

# 仅虚词过滤
python3 tests/regression/run_all.py --filter

# 自定义端口/二进制
python3 tests/regression/run_all.py --port 18080 --binary ./build/bin/pivotmind_gateway
```

## 测试模块

| 模块 | 内容 | 耗时 |
|------|------|------|
| `test_smoke.py` | 启动/健康/状态/基础对话不崩溃 | ~35s |
| `test_response.py` | 10 题回复非空、长度合理、虚词比 | ~40s |
| `test_filter.py` | 虚词输入不泄漏到输出 | ~35s |
| `test_stress.py` | 20 并发请求稳定性 | ~40s |

## 设计原则

- **不依赖训练好的状态** — 冷启动也能跑，重点是"不崩溃"而非"答得好"
- **每题独立** — 一题失败不影响后续
- **自动启动/停止 gateway** — 无需手动管理进程
- **退出码反映结果** — 可在 CI 中直接使用

## 添加新题目

编辑 `questions.json`，添加 `{"q": "你的问题", "lang": "zh或en"}`。
