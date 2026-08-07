# 0.5.8 — 感知异步化与 POS 语法涌现

> **日期**: 2026-08-07 | **类型**: 修复 + 功能 + 性能

## 概述

从 0.5.7 的"知识说得出"到 0.5.8 的"结构稳得住、语法长得出"：主循环彻底摆脱网络阻塞（感知/自学/漏锁三层根治，gdb 三次抓栈实锤），POS 池管道打通——喂料路径首次喂养语法拓扑，**额外词类 0→1 历史性破零**，玄枢开始从语料统计中涌现自己的词类。

## 背景

08-07 凌晨黑匣子哨兵告警：脑干连续卡死 19 分钟（ticks 停在 6223）。tick 速率从 2s 恶化到 20-50s 后彻底停摆。这是 0.5.7 以来最严重的稳定性事件——主循环被阻塞，但进程活着、爬虫线程还在跑（62.8% CPU），典型"主循环假死"。

## 根因链（gdb 三次抓栈实锤）

1. **感知搜索同步阻塞主循环**：`perception_tick` 在主循环内同步执行 `search_and_learn`（HTTP 请求），且所有请求共用 `g_fetch_lock` 全局锁；**DNS 解析不受 curl 超时控制**（getaddrinfo 同步阻塞）——网络一抖，主循环每轮白等 20-40 秒，某请求彻底挂起则永久卡死。
2. **自学线程锁内跑网络**：`self_learner_cycle` 持 master 读锁调 `perception_learn_concept`（curl）——持读锁等网络，主循环 wrlock 被饿死。
3. **主循环同步跑自学**：`brainstem_tick_learning_scan` 同步调 `self_learner_cycle`——修复 2 后暴露：主循环直接跑网络。
4. **两处漏锁**：`autonomic_compound_consolidate` 与 `_article_flush_locked` 各有 4 处提前 return 未解锁——任一路径触发（malloc 失败/拓扑瞬态），对应锁被永久持有，线程堆积雪崩（274 线程 → 内存压力 → gateway 挂）。
5. **POS 池喂养锁内聚类**：`article_reader` 建边时查询词性（tag_soft），内含 O(n²) 聚类（256² 相似度矩阵 2-5 秒）——锁内长持，学习线程排队堆积（35 线程稳态）。

## 核心变更

### Fixed

- **感知搜索异步化**（`perception.c`）：
  - 新增 `_perception_worker` 工作线程：16 槽环形缓冲 + 条件变量，串行执行网络搜索
  - `perception_tick` 只选词入队立即返回——**主循环永不碰网络**
  - 队列满则丢弃本次（不阻塞），搜索频率由 worker 自然节流
- **web_fetch 全局锁超时**（`web_fetch.c`）：`g_fetch_lock` 改 `pthread_mutex_timedlock`（8s），拿不到锁放弃本次请求——锁被长持时不再无限等待
- **自学锁外搜索**（`self_learner.c`）：`self_learner_cycle` 锁内只收集孤立节点概念名（≤32），解锁后统一入队——**持锁不碰网络**
- **主循环异步自学**：自学搜索改走新增的 `perception_enqueue_search`（异步入队）——主循环/自学线程任何路径都不再同步发 HTTP
- **autonomic 漏锁**（`autonomic_learner.c`）：4 处提前 return 补 unlock（goto 统一出口）——写锁不再永久持有
- **article_flush 漏锁**（`article_reader.c`）：`_article_flush_locked` 4 处提前 return 改 goto `_flush_out`——ar->mutex 不再泄漏
- **POS 池喂养锁外化**：锁内只 strdup 概念名（≤64），`article_flush` 解锁后统一 `emergent_pos_tag_soft`——聚类 O(n²) 不再阻塞学习线程

### Added

- **POS 池管道**（`article_reader.c` + `emergent_pos.c`）：feed/learn 路径首次喂养语法拓扑——
  - `article_reader_set_emergent_pos` setter + gateway 挂载（`gw->prefrontal->controller->emergent_pos`）
  - 喂料建边时查询词性 → 未分类新词进池 → 池满聚类 → 额外词类涌现
  - **额外词类 0→1**（08-04 基线为 0：此前 POS 池只吃生成/对话路径，玄枢不会说话 → 池永远空 → 聚类永不触发——死循环打破）
- **`perception_enqueue_search`**：异步入队式搜索，调用方零阻塞
- **聚类后清池**（`emergent_pos.c`）：`try_emerge` 检查后清空未分类池（无论成败）——散词聚不成簇不再反复触发 O(n²) 聚类

### Changed

- **版本号**：0.5.7 → 0.5.8（小步更新）
- **黑匣子哨兵**：no-agent 纯告警 → agent 模式（告警自动触发调查处理，Hermes 侧）

## 验证

- **主循环恢复**：重启后 tick 2s（卡死前 20-50s，快 10-25 倍）；4 倍速压力喂料（2000 字符/s）无 STUCK
- **线程稳定**：30 分钟采样 35 线程封顶（修复前 4→274 无顶上涨），RSS 483MB 稳定
- **POS 涌现**：额外词类 0→1 持久化（emergent_pos.bin），聚类 5 次涌现；POS 池喂养单轮 599 词查询
- **喂料推进**：呐喊/三国演义限速喂入（500 字符/s），节点 85046→89301，字符对 185 万
- **状态无损**：多次重启状态文件完好（180MB 加载 162s）

## 已知问题

- **字符对表高负载**：pair_count 185 万接近 2M 槽上限（负载 91%），每次 find 探测变慢——计划加表上限（如 100 万，满则停插新对）
- **学习线程 35 稳态偏高**：聚类并发占 CPU（板子 6 核），功能正常但可优化（聚类限频/线程池）
- **额外词类仅 1 个**：聚类阈值 0.50 较高 + 白话词多匹配锚点——需冷门/文言语料（古籍书库）积累更多新词类
- **POS 池上限 256**：喂料期间池频繁灌满，聚类触发密集——锁外化后不阻塞，但 CPU 开销存在
