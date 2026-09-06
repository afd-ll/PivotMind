# v0.5.24 — 流式批化存盘与空批 OOM 根治（2026-09-06）

## 背景

8-23 起实施的「候选A 流式批化快照存盘」重构（根治锁内写路径饿死）遗留一个语义缺陷：主循环用 `if (!batch)` 一刀切判 OOM，把「空拓扑/空批」这个合法状态误判成 OOM，导致主状态文件从 9-03 13:07 起长期无法成功更新。9-06 完成根因修复 + 实测验证（喂料 50 条 + 跨重启持久化零丢失）。

## 流式批化存盘（候选A，8-23）

### 根治锁内写路径饿死
- 病灶：旧 B3 全量快照（锁内一次拷 499MB → 锁外写盘）被旧内存门卫（est > MemAvailable-512MB → 回退锁内写）架空；锁内写以 master 读锁贯穿 40s+ 磁盘 I/O，配合 rwlock 写优先饿死脑干 tick + 学习线程（STUCK）。
- 方案：删除旧内存门卫；主流程改流式批循环——每批（MASTER_BATCH_MAX=100 节点）短持锁深拷贝 → 锁外序列化追加写 .tmp → 批间释放；磁盘 I/O 100% 在锁外，单批内存峰 ≤80MB。
- 字节布局 v8 逐块不变，加载端零改动；保留 PIVOTMIND_SAVE=locked env 为显式运维逃生开关（不默认启用）。

## 空批 OOM 根治（9-06）

### 根因
- master_capture_batch 对「空批」（该窗口无任何 node->concept 非空节点，written==0）正常返回 NULL 且 out_count==0；情绪拓扑（TOPO_EMOTION）惰性初始化 node_count==0 长期空批。
- 主循环 if (!batch) 把空批误判成 OOM → break 放弃整轮 → 情绪拓扑起后续所有拓扑 + 尾部段（cross_links/freq/cross_hit）全部不落盘。
- 后果：主状态文件自 9-03 13:07 起每约 1.5 分钟报「批捕获 OOM（topo=2 start=0）」一次，从未成功更新，增量全堆内存，重启即丢。

### 修复
- 主循环改为「先以 cnt==0 判空批（推进窗口跳过），再以 batch==NULL 判真 OOM」。
- 顺带清理死代码：master_capture_batch 的 out_concept_by_id/out_concept_count 孤儿参数（冻结边导出早已改用 net->nodes[id]->concept 直接查，无需 concept_by_id 查表副本）。

## 崩溃 handler 纯 async-signal-safe（brainstem）
- v0.5.11 版虽去 malloc 仍调 backtrace()/backtrace_symbols_fd()，实际内部走 dlopen/dladdr 解析符号，会再入动态连接器与 malloc；堆损坏时 handler 内再入堆锁自锁死锁，进程永久冻结，watchdog 误判存活反复重启。
- 彻底删 backtrace 系列：预分配 static 缓冲拼出 [CRASH] 信号行，仅 write(2) 写出，随后 _exit(128+sig)。

## 其他加固
- 激活记录 pthread_key 化（autonomic_learner）：__thread 线程局部存储改堆分配 + pthread_key（pthread_once + pthread_setspecific），支持可重入。
- 常量扩容（constants.h）：PM_EDGE_TRACK 128→256、PM_ACTIVATED_PAIRS 4096→8192。
- concept 解析直查（node_cache/causal/memory）：快照序列化中目标 concept 从 concept_by_id 查表改为 net->nodes[id]->concept 直接查（配合流式批化，锁外安全）。
- 内存安全防御（causal/dialog/memory）：pq/graph 空指针防御、strdup 失败处理、snprintf 截断安全（%.120s 防 concept 名溢出）。
- bs_yield 忙转让步（gateway）：sched_yield() 防脑干空转。

## 验证
- 编译 -Wall -Wextra 零警告；单测 topology 3/3、memory 4/4 通过。
- 实测：喂料 50 条全部 accepted，节点 30167→30367（+200）、链接 +15011；每约 70s 稳定落盘；跨重启加载节点数一致（30367），零丢失。
- OOM 报错 11:48:56 后归零。

## 顺带处理
- /tmp（tmpfs 1.9G）曾 100% 满：清理 9-03 内核编译残留 linux-src（1.9G）+ linux-6.12.107.tar.xz（34M），解决 gateway LTO 链接「ld: could not close arguments file」。
