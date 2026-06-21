# 0.1.1 — 后台时钟循环 (BackgroundClock)

> **日期**: 2026-05-28 | **类型**: 新增

## 动机

玄枢系统当前是纯离散的：`fgets()` 阻塞等待输入 → 处理 → 输出 → 再次等待。在无人交互时系统完全静止，缺乏"生命感"。

**核心理念**：生命不是一个状态，而是一个持续过程。大脑即使在无外部刺激时也有持续的神经活动——背景放电、记忆巩固、情绪漂移。BackgroundClock 为玄枢注入这一基本特性。

## 改动内容

### 新文件

| 文件 | 说明 |
|------|------|
| `include/background_clock.h` | BackgroundClock 结构体与 API 声明 |
| `src/background_clock.c` | 后台时钟核心实现 |

### 修改文件

| 文件 | 改动 |
|------|------|
| `include/constants.h` | 新增 7 个 `PM_CLOCK_*` 常量 |
| `demos/digital_life.c` | 集成 BackgroundClock 的创建/启动/停止/销毁 |

## 架构设计

### 线程模型

```
主线程 (digital_life.c):
  fgets() 阻塞 → dialog_process() → 输出回复 → 循环

BackgroundClock 线程:
  while(is_running):
    sleep(1000ms)
    rdlock(拓扑) → 激活衰减 + 自发激活 → unlock
    认知状态漂移（无锁）
    每10tick: 记忆巩固
```

### 四个核心行为

| 行为 | 频率 | 锁策略 | 说明 |
|------|------|--------|------|
| **激活衰减** | 每 tick | rdlock | `node->activation *= 0.97`，<0.01 归零 |
| **自发激活** | 每 tick | rdlock | 按期望值随机注入弱激活 (~0.1-0.2) |
| **状态漂移** | 每 tick | 无锁 | drive/emotion 向基线 EMA 回归 |
| **记忆巩固** | 每 10 tick | memory 内部 mutex | STM→LTM 迁移 |

### 配置参数

```c
PM_CLOCK_TICK_INTERVAL_MS     1000     // 1秒
PM_CLOCK_DECAY_PER_TICK       0.97     // 每秒衰减 3%
PM_CLOCK_SPONTANEOUS_PROB     0.0001   // 0.01% 每节点每 tick
PM_CLOCK_SPONTANEOUS_STRENGTH 0.15     // 激活注入量
PM_CLOCK_CONSOLIDATE_INTERVAL 10       // 10秒一次巩固
PM_CLOCK_STATE_DRIFT_RATE     0.995    // 保持率
PM_CLOCK_ACTIVATION_FLOOR     0.01     // 归零阈值
```

### 线程安全

- **拓扑遍历**：使用 `MasterTopology.rwlock` 读锁，读锁之间不互斥
- **状态漂移**：浮点写入在 x86 上是原子操作，且竞争极低频
- **记忆巩固**：`memory_consolidate()` 内部已有互斥锁
- **性能**：9 拓扑 × ~10000 节点遍历 < 5ms，几乎不阻塞前台

## DigitalLifeSystem 集成

```c
digital_life_create()   → background_clock_create()
digital_life_start()    → background_clock_start()
digital_life_stop()     → background_clock_stop()
digital_life_destroy()  → background_clock_destroy()
```

初始化步骤从 5 步增加到 6 步（新增第 6 步：后台时钟）。

## 设计决策

1. **为何不把激活衰减放在对话流程里**：对话间隔不均匀（可能几秒到几小时），而生物神经系统需要均匀的时间基准
2. **为何用 rwlock 读锁而非独立锁**：复用已有基础设施，避免死锁风险
3. **为何状态漂移不用锁**：CognitiveState 很少同时被两线程修改，竞争窗口极小
4. **自发激活为何按期望值而非每节点 rand()**：全量 rand() 开销太大（90000 次/tick），按期望值随机选择节点更高效
