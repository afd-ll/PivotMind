# PivotMind 检测项目与标准 — v0.4.8

> 最后更新: 2026-06-27 | 对应版本: v0.4.8

## 一、检测体系架构

```
P0 核心检测 (每次提交必须通过)
├── 编译检测    gcc -Wall -Wextra 零错误
├── CI 检测     GitHub Actions x86_64 + ARM 交叉编译 (16 项)
├── 单元测试    make test (15 模块) + Unicode 专项 (13 项)
└── 冒烟测试    /health /status 基础响应

P1 功能检测 (发版/PR 前必须通过)  
├── 回归测试    对话质量 + 虚词过滤 (32 项)
├── 状态完整性  边计数 + 格式校验 + uXXXX 零残留
├── 认知控制器  test-cc + test-cc-full
└── 集成测试    test-integration

P2 压力检测 (重大变更前运行)
├── 并发测试    20 并发 + 事后健康
├── 训练追踪    train_track.py 趋势
└── 内存检测    网关 RSS 监控
```

### v0.4.8 实测基线

| 指标 | 实测值 | 来源 |
|------|-------|------|
| 状态文件节点 | 30,021 (30000 词汇 + 6 语义 + 15 语法) | convert_state.py |
| 状态文件边 | 84,202 | convert_state.py |
| 平均度 | 2.8 edges/node | convert_state.py |
| 状态文件大小 | 67,501 KB (~66 MB) | ls -lh |
| 格式版本 / 特征维度 | v5 / 512-dim | convert_state.py |
| 加载边恢复率 | 84,196/84,202 = 99.99% | gateway Pass 2 日志 |
| 加载耗时 | ~1 秒 | gateway 日志 |
| uXXXX 残留 | 0 (全文件扫描) | clean_unicode_escapes.py |
| 网关节点 (运行时) | 30,027 | /status |
| 网关 RSS | ~100 MB (空闲) | health_monitor |
| 网关版本 | 0.4.8 | /status |

---

## 二、P0 核心检测（每次提交必须通过）

### 2.1 编译检测

| 项目 | 命令 | 通过标准 |
|------|------|---------|
| x86_64 全量编译 | `make clean && make linux` | 零错误，零 `-Wall -Wextra` 警告 |
| 网关编译 | `make gateway` | 产出 `build/bin/pivotmind_gateway` (非零大小) |

### 2.2 CI 检测 (GitHub Actions)

| 项目 | 触发条件 | 通过标准 |
|------|---------|---------|
| build-x86_64 | 每次 push | 编译通过 + 16 核心测试全绿 |
| build-arm | 每次 push | ARM 交叉编译 + readelf 验证 ELF |

CI 当前覆盖的测试: `make test-tensor, test-model, test-metrics, test-trainer, test-io, test-cc, test-web-fetch, test-dialog-unit, test-diffusion-unit, test-topology-unit, test-memory-unit, test-learner-unit, test-causal-unit, test-forgetting-unit, test-integration`

### 2.3 单元测试 (make test — 15 项，全部 PASS)

| # | 目标 | 模块 | 检测内容 | 通过标准 |
|---|------|------|---------|---------|
| 1 | `test-tensor` | Tensor 运算 | create/add/multiply/broadcast/pool | 所有用例 PASS |
| 2 | `test-model` | 模型 | create/destroy/forward/pass | 所有用例 PASS |
| 3 | `test-metrics` | 训练指标 | accuracy/loss | 所有用例 PASS |
| 4 | `test-trainer` | Trainer | SGD/gradient/batch | 所有用例 PASS |
| 5 | `test-io` | 基础 I/O | stdout/stderr | 所有用例 PASS |
| 6 | `test-cc` | 认知控制器 | retry 降级/satisfaction 评分 | 所有用例 PASS |
| 7 | `test-web-fetch` | Web Fetch | response 分类/init/destroy/cooldown | 所有用例 PASS |
| 8 | `test-dialog-unit` | 对话系统 | 输入解析/生命周期 | 所有用例 PASS |
| 9 | `test-diffusion-unit` | 扩散引擎 | init 安全/basic lifecycle | 所有用例 PASS |
| 10 | `test-topology-unit` | 拓扑 | walk_greedy/node add-remove/edge ops | 所有用例 PASS |
| 11 | `test-memory-unit` | 记忆系统 | STM/LTM/permanent store-retrieve | 所有用例 PASS |
| 12 | `test-learner-unit` | Hebbian 学习器 | online learning/create-update | 所有用例 PASS |
| 13 | `test-causal-unit` | 因果推理 | graph lifecycle/edge/query | 所有用例 PASS |
| 14 | `test-forgetting-unit` | 灾难性遗忘 | EWC config/defaults | 所有用例 PASS |
| 15 | `test-integration` | 集成 | model pipeline create→train→save→load→verify | 所有用例 PASS |

### 2.4 冒烟检测 (回归套件 — smoke)

| 项目 | 命令 | 通过标准 |
|------|------|---------|
| 网关启动 | `python3 tests/regression/run_all.py --smoke` | 4 项全部 PASS |
| 健康端点 | `curl /health` → `{"status":"ok"}` | HTTP 200 + JSON 有效 |
| 状态端点 | `curl /status` | 返回 `total_nodes` ≥ 0, `version` = 当前版本 |

---

## 三、P1 功能检测（发版/PR 前必须通过）

### 3.1 状态文件完整性

**v0.4.8 基线**: 30,021 节点, 84,202 边, 2.8 avg, 67MB, uXXXX=0

| 项目 | 检测方法 | 通过标准 |
|------|---------|---------|
| 格式版本 | `convert_state.py --info` 零 WARN | format_version ≥ 5, feature_dim = 512 |
| 节点数 | 同上 | 30,000 ± 10% (词汇拓扑) |
| 边数 | 同上 | ≥ 80,000 或 ≥ 当前基线的 95% (84,202 × 0.95 = 79,992) |
| 平均度 | 同上 | ≥ 2.0 edges/node (基线 2.8) |
| uXXXX 残留 | `clean_unicode_escapes.py --dry-run` | 节点修复 = 0, 边修复 = 0 |
| concept_len | 无 `bad concept_len` 警告 | 零 WARN |
| 加载边恢复率 | 网关 Pass 2 日志 | 恢复边 ≥ 文件总边数 × 0.99 (基线 99.99%) |
| 文件大小 | `ls -lh` | 60-70 MB |

### 3.2 对话质量

| 项目 | 命令 | 通过标准 |
|------|------|---------|
| 回复非空 | `run_all.py --response` | 所有问题回复非空 |
| 长度合理 | 同上 | 1-500 字符 |
| 虚词比例 | 同上 | EW ≤ 40%, ZH ≤ 30% |
| 冷启动兼容 | 同上 | 无崩溃/超时，空回复可接受 |

### 3.3 虚词过滤

| 项目 | 命令 | 通过标准 |
|------|------|---------|
| 英文虚词输入 | `run_all.py --filter` | 无虚词泄漏到输出 |
| 中文虚词输入 | 同上 | 无虚词泄漏 |
| 统计验证 | count_fw() 返回 0 | 输出不含虚词 |

### 3.4 认知控制器

| 项目 | 命令 | 通过标准 |
|------|------|---------|
| retry 三级降级 | `make test-cc` | 所有用例 PASS |
| 完整管线 (含状态) | `make test-cc-full` | 所有用例 PASS |
| intent weights | 同上 | 权重值在 [0,1] 范围 |
| fuzzy input | 同上 | 模糊输入正确降级 |

### 3.5 集成测试

| 项目 | 命令 | 通过标准 |
|------|------|---------|
| model pipeline | `make test-integration` | create→train→save→load→verify 全链 PASS |

---

## 四、P2 压力检测（重大变更前运行）

### 4.1 并发压力

| 项目 | 命令 | 通过标准 |
|------|------|---------|
| 20 并发 | `run_all.py --stress` | 全部完成，≥80% OK |
| 事后健康 | 同上自动检测 | /health 返回 ok |

### 4.2 训练效果追踪

| 项目 | 命令 | 通过标准 |
|------|------|---------|
| 10 轮训练 | `train_track.py --rounds 10` | 回复率上升趋势 |
| CSV 输出 | 同上 | 每轮记录 node/reply/len/fw% |

### 4.3 内存监控

| 项目 | 检测方法 | 通过标准 |
|------|---------|---------|
| 网关 RSS | `ps aux` 或 health_monitor | < 400MB (RK3399 3.8GB 总量) |
| 内存增速 | health_monitor RED 告警 | 无 RED 状态持续 >60s |
| 泄漏检查 | 运行 30min 后 RSS | RSS 不再持续增长 (增速 < 5MB/min) |

---

## 五、专项检测（按模块）

### 5.1 JSON Unicode 解析

| 项目 | 命令 | 通过标准 |
|------|------|---------|
| \uXXXX 解码 | `python3 tools/test_unicode_escape.py` | 13/13 PASS |
| 代理对 (emoji) | 同上 | ud83dude80 → 🚀 |
| 误伤防御 | 同上 | u0041 (ASCII) 不解码 |
| 状态文件 round-trip | 同上 | dirty→clean→write→read→verify |

### 5.2 神经网络子系统

| 项目 | 命令 | 通过标准 |
|------|------|---------|
| Tensor | `make test-tensor` | PASS |
| Model | `make test-model` | PASS |
| Metrics | `make test-metrics` | PASS |
| Trainer | `make test-trainer` | PASS |
| Integration | `make test-integration` | PASS |

---

## 六、快速检测清单（日常使用）

```bash
# P0: 每次提交前
make clean && make linux        # 编译
make test                        # 单元测试 14 项
python3 tools/test_unicode_escape.py   # Unicode 专项

# 网关在线状态
curl http://localhost:19531/health
curl http://localhost:19531/status | jq .total_nodes

# 状态文件完整性
python3 tools/convert_state.py pivotmind_state.dat --info | grep -E "Total|edges|WARN"
python3 tools/clean_unicode_escapes.py pivotmind_state.dat --dry-run | grep "修复"

# P1: 发版前
python3 tests/regression/run_all.py      # 回归 32 项
make test-cc && make test-cc-full        # 认知控制器

# P2: 重大变更前
python3 tests/regression/run_all.py --stress   # 并发
python3 tests/regression/train_track.py --rounds 10  # 训练追踪
```

---

## 七、待补项 (TODO)

| 优先级 | 项目 | 说明 |
|--------|------|------|
| P1 | 集成 test-cc-full 到 CI | 需要有效的 state file 作为测试 fixture |
| P1 | 新增 pre-push hook | `.git/hooks/pre-push` 跑 `make test` |
| P2 | 新增状态文件基准测试 | 比较 save→load→save 的边数守恒 |
| P2 | 新增 \uXXXX 格式自动化回归 | CI 中检测状态文件零 uXXXX |
| P2 | 实现训练前后边数快照对比 | 量化每次训练的净增益 |
