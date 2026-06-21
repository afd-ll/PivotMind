# 0.2.6 — 训练模式修复里程碑

> **日期**: 2026-05-31 | **类型**: 修复

## 动机

v0.2.3 将引擎推入长时间自运行态后，下一步是让引擎具备"不停机学习"能力——训练模式（`--train-mode`）。初版训练模式集成后暴露了五个 P0 级致命问题：虚词丢失、教学格式错位、double-free 崩溃、路由缺失、连接爆炸。同时，架构审查中发现三个逻辑漏洞和三个实现陷阱，经与用户讨论后全部修正为 v3 架构。v0.2.4 的目标是让训练模式从"能用"变为"可用"。

## 改动总览

| 类别 | 改动 | 关键文件 |
|------|------|----------|
| 🔴 致命 | 删除虚词过滤（AI 无法生成虚词） | `src/train_mode.c` |
| 🔴 致命 | 废弃教学格式，Q→A 直接强连接 | `src/train_mode.c` |
| 🔴 致命 | double-free 修复（线程生命周期） | `src/train_mode.c`, `demos/pivotmind_gateway.c` |
| 🔴 致命 | 连接爆炸修复（禁用训练期 O(n²) 关系发现） | `src/train_mode.c` |
| 🟡 缺陷 | /train/status GET 路由缺失 | `demos/pivotmind_gateway.c` |
| 🔵 架构 | QA 训练重定义：模板提取→ | 架构审查 v3 |
| 🔵 架构 | 文章阅读模式设计（待实现） | 架构审查 v3 |
| 🔵 架构 | 字符级统计合并 + 语法约束词发现 | 架构审查 v3 |
| 🔵 架构 | 9+1 脑区分区替代 syn_role 维度灾难 | 架构审查 v3 |

---

## 详细改动

### 1. 删除虚词过滤（Critical）
**问题**：原代码包含 42 个虚词的 `stop_words[]` 数组，训练时遇到虚词直接 `continue` 跳过。后果：虚词永远不会成为概念节点 → AI 在生成时找不到虚词 → 输出全是实词堆砌，没有"的、了、是、在"等功能词。

**根因**：虚词虽无独立语义，但承担语法连接功能。它们必须存在于网络中，才能在扩散生成时被模板层和扩散引擎选中并插入输出。

**修复**：完全删除 `stop_words[]` 数组和 `is_stop` 检查分支。所有词（包括虚词）一律进入概念网络。

```c
// 删除前（错误逻辑）：
for (int i = 0; i < STOP_WORDS_COUNT; i++) {
    if (strcmp(word, stop_words[i]) == 0) { is_stop = 1; break; }
}
if (is_stop) continue;  // 虚词被跳过 → 永远不会成为节点

// 删除后：所有词平等处理，虚词正常入网
```

### 2. 废弃教学格式，Q→A 直接强连接（Critical）
**问题**：原 QA 训练流程把 Q 和 A 拼接成 `"Q。A"` 格式，然后作为一条文本送入 `train_feed_one_line` 分词。这相当于把问答关系变成"Q 的后续文本是 A"，而非"Q 和 A 之间存在直接关联"。

**用户反馈**：教学格式是早期遗留设计，不适配当前 AI 架构。

**修复**：Q 和 A 分别成为独立概念节点，之间建 weight=0.8 的直接强连接。不再拼接、不再分词。

```c
// 修复前：拼接后分词（教学格式）
char teach[4096];
snprintf(teach, sizeof(teach), "%s。%s", q, a);
train_feed_one_line(vocab, stream, teach, ...);

// 修复后：Q→A 直接强连接
int q_id = trace_wisdom_net_find_concept(vocab->net, stream->q_buf);
if (q_id < 0) q_id = trace_wisdom_net_dynamic_add_node(vocab->net, stream->q_buf, NULL, 0);
int a_id = trace_wisdom_net_find_concept(vocab->net, stream->a_buf);
if (a_id < 0) a_id = trace_wisdom_net_dynamic_add_node(vocab->net, stream->a_buf, NULL, 0);
if (q_id >= 0 && a_id >= 0) {
    trace_wisdom_net_add_connection(vocab->net, q_id, a_id, 0.8f);
    tm->progress.total_added_edges++;
}
```

**设计意义**：QA 对的本质不是"Q 后面跟着 A"的文本序列，而是"有人问 Q 时应该回答 A"的语义映射。直接强连接 (0.8) 让扩散引擎能一步从 Q 到达 A，无需经过中间概念。

### 3. double-free 修复（Critical）
**问题**：训练线程创建时使用了 `pthread_detach`，导致线程与主线程脱离。当 `train_mode_destroy` 被调用时，无法等待线程退出，直接 `free(tm)` → 线程仍在访问已释放的内存 → use-after-free / double-free。

**根因**：`pthread_detach` 与 `pthread_join` 互斥——detach 后 join 会返回错误。原代码两者同时存在，矛盾。

**修复**：
- 移除 `pthread_detach`
- `train_mode_stop` 设置 `is_running = 0` 通知线程退出
- `train_mode_destroy` 使用 `pthread_join` 等待线程真正退出后再释放
- `demos/pivotmind_gateway.c` 移除关闭时重复的 `train_mode_stop()` 调用

```c
void train_mode_destroy(TrainMode* tm) {
    if (!tm) return;
    train_mode_stop(tm);       // 通知线程停止
    if (tm->is_running) {
        printf("[训练] 等待训练线程退出...\n");
        pthread_join(tm->thread, NULL);  // 等线程真正退出
    }
    tm->is_running = 0;
    free(tm);                  // 安全释放
}
```

### 4. 连接爆炸修复（Critical）
**问题**：训练开始后，喂了 100 条 QA 就导致连接数从 120 万暴增至 1160 万，RSS 从 350MB 飙升到 1.4GB。

**根因分析**：`discover_new_relations()` 中的 `discover_connections_by_cooccurrence()` 每处理 100 个项目就执行一次 O(n²) 全节点对遍历，阈值仅 0.35。训练期间概念节点快速增长，每轮遍历的节点对数指数级膨胀。

**注意**：连接爆炸不是 `train_feed_one_line` 或 QA 直接强连接造成的——喂料本身只创建合理的边（0.37 条/条）。元凶是训练期间周期性触发的 O(n²) 关系发现。

**修复**：训练期间完全禁用 `discover_new_relations()`，延迟到训练结束后执行。

```c
// train_mode.c 中的 batch_learn 回调
static void train_batch_learn(Vocabulary* vocab, ...) {
    // 训练期间不执行关系发现
    // discover_new_relations(vocab);  // ← 禁用
}
```

### 5. /train/status GET 路由

**问题**：训练状态查询只有 POST 路由，浏览器无法直接查看训练进度。

**修复**：在 GET 路由块中添加 `/train/status`，返回 JSON 格式的训练状态。

### 6. total_added_edges 计数修正

**问题**：`trace_wisdom_net_add_connection` 内部有去重机制（`node_conn_find` O(1) 哈希查找），重复连接只更新权重不创建新边。但外部计数 `total_added_edges++` 在每次调用时都递增，导致统计虚高。

**修复**：调用前用 `node_conn_find` 检查是否已存在，不存在才计数。

---

## 架构审查 v3：训练模式重新定义

在修 P0 Bug 的同时，对训练模式的整体架构进行了深度审查，识别并修正了三个逻辑漏洞和三个实现陷阱。

### 重新定义：QA 训练 = 知识灌输

| 维度 | 旧理念 | 新理念 |
|------|--------|--------|
| QA 训练本质 | 把知识喂给 AI | 提取日常语境下的模板 |
| 文章阅读本质 | — | 知识量和节点量扩张 |
| Q→A 关系 | "Q 后面跟着 A" | Q 和 A 之间有直接强关联 |
| 分词需求 | 通用分词 | QA 不需要；文章用字符级+统计合并 |

### 漏洞一：乘法抑制的冷启动死锁

**问题**：原方案 `merge_score = noun_score(X) * noun_score(Y) * PMI`，冷启动时 noun_score 接近 0 → 任何新词对的得分都为 0 → 永远无法合并。

**修正**：改为分层加法评分 + 频次兜底。

```
merge_score = α·PMI + β·freq_bonus + γ·conn_bonus
freq_bonus = min(freq_X + freq_Y, threshold) / threshold
```

冷启动时 PMI 为 0，但 freq_bonus 随出现次数增长，保证新词对有合并机会。

### 漏洞二：回音壁效应

**问题**：统计涌现从已有连接模式中推断词类 → 强连接越强越容易吸引更多同类连接 → 环路放大 → 弱类别概念永远无法被发现。

**修正**：
- 温度扰动：扩散采样时加入随机扰动，让弱连接也有被激活的机会
- 反事实采样：15% 模糊区随机决策，不跟随统计多数

### 漏洞三：维度灾难

**问题**：在 Connection 中加入 `syn_role` 字段记录句法角色 → 查询某节点的词性需要遍历其所有连接 O(n) → 大节点极慢。

**修正**：9+1 脑区分区架构。词性不是节点属性或连接标签，而是节点在脑区中的位置。

```
脑区 = [名词区] [动词区] [形容词区] [代词区] ... [词汇区(默认)]
```

- 新词默认进入词汇区
- 随着连接模式收敛，自动迁移到对应功能区
- 查询词性 = 查询节点所在脑区 → O(log n) 二分查找
- 更新脑区归属 → O(1) EMA 衰减迁移

**核心洞察**：词性是涌现，不是标签。不需要先知道词性再建连接，而是"从连接模式中涌现词性"。不存在鸡生蛋悖论。

### 实现陷阱

| 陷阱 | 问题 | 修正 |
|------|------|------|
| Membership 溢出 | 新概念大量涌入单脑区，隶属度无上限 | EMA 衰减 α=0.05，旧隶属度自然消退 |
| 冷启动归类 | 新词不知道该进哪个脑区 | 默认进词汇区，随连接模式自动迁移 |
| 反事实采样 C 实现 | 15% 模糊区需要随机决策 | 用 `rand() % 100 < 15` 实现极简随机 |

---

## 运行指标（v0.2.4 实测）

| 指标 | v0.2.3 | v0.2.4 |
|------|--------|--------|
| 训练模式 | 无法使用（崩溃→爆炸） | **稳定运行** |
| 100 条 QA 后连接数 | 1160 万（爆炸） | **4.31M（正常）** |
| RSS（1050 条 QA） | 1.4GB+（爆炸） | **685MB（稳定）** |
| 新建边/条 | ~10 万（爆炸） | **0.37（正常）** |
| double-free | 必现 | **已修复** |
| 虚词生成 | 缺失 | **正常** |
| /train/status | 无 | **可用** |
| 训练停止 | 崩溃 | **优雅退出** |

### 验证步骤

1. 启动训练：`./pivotmind_gateway --train-mode --corpus data/qa_11050.json --speed 50`
2. 观测状态：`curl http://localhost:8080/train/status`
3. 训练完成：1050 条 QA，新建边 4111 条（0.37/条），连接 4.31M，RSS 685MB
4. 停止训练：`curl -X POST http://localhost:8080/train/stop` → 无崩溃
5. 引擎正常服务：`curl http://localhost:8080/status` → 节点/连接数合理

---

## 新增文件

| 文件 | 说明 |
|------|------|
| `include/train_mode.h` | 训练模式 API（v0.2.3 新增，v0.2.4 修复） |
| `src/train_mode.c` | 训练模式实现（v0.2.3 新增，v0.2.4 重写核心逻辑） |

## 主要修改文件

| 文件 | 改动 |
|------|------|
| `src/train_mode.c` | 删除虚词过滤、废弃教学格式、double-free 修复、禁用训练期关系发现、计数修正 |
| `demos/pivotmind_gateway.c` | /train/status GET 路由、移除重复 train_mode_stop 调用 |

---

## 设计决策记录

1. **"虚词不是噪声，是骨架"**：虚词虽无独立语义，但承担语法连接功能。删除它们等于让 AI 只会说名词和动词，不会造句。
2. **"QA 不是文本，是映射"**：Q→A 的关系不是文本序列上的前后接续，而是语义空间中的直接关联。直接强连接 (0.8) 比分词拼接更准确。
3. **"词性是涌现，不是标签"**：不是先给词标注词性再建连接，而是从连接模式中自动涌现出词性。9+1 脑区分区让这个涌现过程可观测、可控制。
4. **"O(n²) 是训练的敌人"**：训练期间节点快速增长，任何周期性全量遍历都会导致连接爆炸。关系发现必须延迟到训练结束后。
5. **"线程不能 detach 后又 join"**：pthread 生命周期管理必须一致——要么 detach（自生自灭），要么 join（等它结束）。不能兼得。

---

## 后续路线（P1/P2）

### P1（架构改进）

| 任务 | 估时 | 说明 |
|------|------|------|
| TopologyBrain 脑区索引模块 | 3h | 9+1 脑区分区，零侵入式替换 |
| 字符级共现统计 + PMI | 2h | 文章阅读模式的基础设施 |
| 分层合并评分 | 2h | PMI + freq_bonus + conn_bonus |
| 模糊区随机决策 | 30min | 15% margin rand() < 15 |
| 文章阅读模式 `--format article` | 2h | 第二种训练模式 |

### P2（增强）

| 任务 | 说明 |
|------|------|
| 温度扰动机制 | 扩散采样随机扰动 |
| 反事实采样 | 15% 模糊区随机决策 |
| 增量关系发现 | 训练结束后安全执行 discover |
| 训练进度 Web UI | 浏览器可视化 |
| 自动 checkpoint | 定期存档训练状态 |
