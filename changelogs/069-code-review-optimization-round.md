# v0.5.25 — 网关并发化、线程池锁语义修复、种子原子写加固、gateway 按模块拆分（代码审查优化轮次）

> **日期**: 2026-09-09 | **类型**: 优化/修复

## 概述

按外部代码审查报告落地 P0/P1/P2 优化清单。核心四项：网关从"单线程串行 accept→处理"改为"每连接独立线程"（P0-1）；线程池批处理补全锁语义、修复 destroy/超时路径的 UAF 与跨代串批隐患（P1-1）；记忆种子保存改为原子写 + FNV-1a 完整性 footer、加载改为先校验后提交（P0-2）；网关日志/错误码子系统加固（P2-2/P2-3/P2-4/P2-7）。P2-6 gateway 单文件拆分（~2200 行 → 6 个模块文件）一并落地；P2-5（static 可变全局收敛）另开分支。复审与实测阶段追补三处 P0：种子 footer 哈希校验两端范围不对称（P0-3）、学习 worker 无退出机制导致关闭 UAF（P0-4）、种子加载第一遍循环边界把 footer 当记录解析致新格式种子恒加载失败（P0-5）。

## 核心变更

### P0-1 网关每连接独立线程（demos/pivotmind_gateway.c）

- 旧版主循环串行 `accept → handle_connection`：任一连接慢速上传（recv 超时 10s）或同步推理耗时都会阻塞后续所有连接，/health 健康探测被拖死。
- 改为 accept 后立即交独立线程处理，主循环即刻回到 accept；`g_conn_count` 原子计数（GCC `__sync`）限制并发，超上限 `GW_MAX_CONN=64` 直接回 503 关闭，防慢连接洪泛打穿线程资源。
- 退出路径有界轮询等待在途连接线程结束（上限 15s，覆盖 recv 超时 + 处理余量），避免 detached 线程在 main 栈 `gw` 释放后仍访问；引擎推理函数（handle_chat 内 pfe/prefrontal/qa_memory）当前架构下未新增额外风险，如未来需高并发调用须在引擎侧确认线程安全。

### P0-2 记忆种子原子写 + 完整性校验（src/memory_system.c）

- 旧版 `fopen(path,"wb")` 直接覆盖：崩溃/断电/磁盘满发生在写中途会截断原文件，长期记忆种子整体丢失且无备份可回退；文件无完整性校验，损坏文件被静默"部分加载"。
- 保存改为对齐 multi_topology/cross_edge_io 的 tmp+rename 原子写：先写 `<path>.tmp`，边写边对载荷计算 FNV-1a 64 哈希，末尾追加 16 字节 footer（MAGIC `PMSEED2` + hash）；fclose 成功后 rename 原子替换，失败清理 tmp 并保留原文件。
- 加载改为"整文件读入 → 边界解析 + footer 哈希校验通过 → 再 store"两遍解析：新格式校验哈希，不匹配拒绝提交返回 -1；旧格式（无 footer）正常解析保持兼容；条目中途截断/字段非法明确报损坏，不再静默部分加载；64MB 上限防异常膨胀；importance 越界钳制到 [0,1]。

### P1-1 线程池批处理锁语义修复（src/thread_pool.c）

- 批次状态重置（tasks/task_count/next_index/workers_done）移入锁内：旧版无锁重置 + volatile 不足以与 worker"上报完成"临界区建立 happens-before，属 C11 数据竞争 UB。
- `shutdown` 仅在「非批次进行中（!running）」才响应：destroy 恰逢批次进行时 worker 完成本批并上报 done，batch() 正常收尾，而非中途退出导致 workers_done 永远凑不齐、destroy 只能等超时。
- worker 用锁内快照 `batch_count` 窃取任务：防止本批超时强退后下一批重置 next_index/task_count，慢 worker 串批执行新批次任务（跨代干扰）。
- 等待 worker 完成不再"10s 超时强制结束并返回成功"——旧逻辑下调用方按"batch 已返回"释放 tasks（dialog_system/multi_topology 均为栈数组）会 use-after-free。batch 返回 == 本批全部执行完是 API 契约，超时只能持续告警、不能提前返回。
- batch 收尾补 `pthread_cond_broadcast(cv_done)`：唤醒可能在 destroy() 中等待批次结束的线程（旧版不广播，destroy 只能干等 5s 超时兜底）。
- `batch_count` 锁内快照处补注"安全前提"：本快照只能防串批，挡不住提前返回；一旦有人改回"10s 超时强制结束并返回成功"，调用方按 batch 已返回释放栈上 tasks 会立刻复现 UAF。注释固化该约束，防后人踩雷。

### 追补修复（复审发现的 P0，同轮次一并落地）

#### 种子哈希校验不对称（P0-3，src/memory_system.c）

- 保存端 footer 写作原为 `WRITE_AND_HASH(PMSEED_MAGIC, ...)` + `WRITE_AND_HASH(&h, ...)`，即魔数 `PMSEED2` 也被喂进哈希后才落盘 `h` → 保存端 `hash = FNV-1a(记录区 ‖ MAGIC)`。
- 加载端第一遍只对记录区（key_len/key/data_sz/data/type/importance）更新哈希，读到末尾 16 字节时仅取魔数与 stored_hash 比对 → 加载端 `hash = FNV-1a(记录区)`。
- 两端哈希范围不对称 → **所有新格式（带 footer）种子文件校验恒失败**，保存后立即重新加载必被判定"哈希校验失败，文件可能损坏"拒绝加载，P0-2 的完整性保护实际变成"保存即不可读"。旧格式（无 footer）路径不受影响，故未被注意。
- 修复：footer 魔数改裸 `fwrite`，**不参与哈希**（魔数是 footer 定界符，本就不该计入载荷校验）；仅有裸 `fwrite(&h,...)` 落盘哈希值。修复后保存端与加载端哈希范围都严格等于记录区载荷，两端对称，正常文件校验通过、损坏文件仍能被识别。

#### 种子加载第一遍循环边界把 footer 当记录解析（P0-5，src/memory_system.c）

- 该缺陷与 P0-3 同源、被 P0-3 掩盖：第一遍解析循环条件写成 `while (pos + 16 <= sz)`。最后一条记录读完后 `pos == sz - 16`，条件 `pos + 16 <= sz` 仍成立 → 再进一轮，把 footer 前 4 字节魔数 `"PMSE"` 当 `key_len` 读出（`0x45534D50 = 1163010384 > 4096`）→ `truncated = 1` → 直接判"记录截断/字段非法"拒绝加载。
- 后果：**所有新格式（带 footer）种子文件加载恒失败**——即使哈希完全正确（P0-3 修好后本测试实测：hash 对称性断言全部通过，加载仍返回 -1）。空种子（16 字节纯 footer）同样被拒。旧格式无 footer 路径不受影响。
- 修复：循环条件改 `while (pos + 16 < sz)`，仅当剩余字节严格多于 footer 时才尝试解析记录。实测：3 条记录文件（134B）加载返回 3 且数据/类型/重要性逐条一致，空种子返回 0，截断/篡改文件仍被拒绝。

#### 学习 worker 无退出机制（P0-4，demos/gateway_learn.c / demos/gateway_system.c）

- `g_learn_q.stop` 字段早已存在、`_learn_worker` 循环也实现了 `stop && count==0` 退出判定，但**全仓库无任何地方置位它，也无任何 join**：`gw_system_shutdown` 直接进入 brainstem/topology/memory/perception 的 destroy 流程。
- worker 循环持有 `g_gw`，消费学习任务时访问 `gw->topology`、`gw->perception`、`gw->prefrontal->controller->emergent_pos`、`gw->total_learning_cycles`。关闭时这些对象被 destroy 后 worker 可能仍在跑（队列里有任务 / 正卡在 cond_wait 被唤醒）→ **use-after-free**，表现为关闭瞬间的偶发 SIGSEGV 或静默堆损坏。
- 修复：新增 `learn_queue_shutdown()`（gateway_learn.c 导出、gateway_internal.h 声明）：
  1. 锁内置 `g_learn_q.stop = 1` 并 `pthread_cond_broadcast`，唤醒所有阻塞在 cond_wait 的 worker；
  2. `pthread_join` 全部 `g_learn_workers`，返回即代表无 worker 再触碰 gw 资源；队列尚有任务时 worker 先排空再退出，已入队学习任务不丢；
  3. 防御性排空残留队列（flush 型唤醒等待者由其自行释放，非 flush 型直接 free）。
- 调用点置于 `gw_system_shutdown` **最前**（`if (!gw) return;` 之后、brainstem_stop 之前）：此刻 gw 各对象仍全部有效，worker 可安全处理在队任务；晚于任何 destroy 都会重现 UAF。
- 附带：`learn_queue_init` 增幂等守卫 + `g_learn_inited` 标志，`learn_queue_shutdown` 据此守卫，避免未初始化就 join 或重复 join。

### 网关安全/解析加固（demos/pivotmind_gateway.c）

- **token 常量时间比较**：旧版 strcmp 逐字节提前返回，攻击者可借响应时间逐位爆破 token；改为全程遍历 + XOR 累积（长度不等直接拒绝，64 位 hex token 长度不构成可利用信息）。
- **Content-Length 严格解析**：只在 header 区间 [0, header_end) 内查找，防止 body 中出现同名文本被误解析为头部；数字用 strtol 校验替代 atoi，非法头按无 body 处理。
- **token 打印脱敏**：启动日志不再整段明文打印 token，仅显示前 4 后 4，完整值仍存 GW_TOKEN_FILE（0600），防日志被旁路读取即泄露完整凭据。

### P2-2 端口占用探测去重（demos/pivotmind_gateway.c）

- 删除 3 处重复的 `connect(127.0.0.1)` 预探测块与 2 处重复的 `/tmp/pivotmind.port` 写入（各保留 1 处）。
- 预探测只连 127.0.0.1，与实际 bind 地址（可能 0.0.0.0/局域网 IP）不一致：既可误报也可漏报，且探测与 bind 之间天然存在 TOCTOU 竞态。改为直接 bind，失败按 errno==EADDRINUSE 判定端口占用。

### P2-3 错误码扩展（include/error.h、src/error.c）

- `ErrorCode` 末尾追加 8 个子系统通用错误码：ERR_IO_ERROR / ERR_TIMEOUT / ERR_BUSY / ERR_UNAUTHORIZED / ERR_BAD_REQUEST / ERR_PARSE_FAILED / ERR_INVALID_STATE / ERR_CHECKSUM_MISMATCH。
- 头文件注明"新增枚举必须追加在末尾，禁止中间插入"——错误码已持久化/对外接口按数值引用（种子文件、HTTP 响应等），重排破坏兼容。
- error_string() 补齐对应描述；全库仅 error.c 一处 switch(ErrorCode)，无遗漏。

### P2-4 日志线程安全 + 可落盘（src/error.c、include/error.h、demos/pivotmind_gateway.c）

- error.c 新增 `log_set_output(FILE*)` 重定向日志输出（NULL 恢复 stderr）；日志输出加 pthread 互斥锁，保证多线程整行日志不被并发写交错（Windows 侧空实现）。
- 刷新策略从"仅 LOG_FATAL 刷新"改为 `level >= LOG_WARNING` 即 fflush，避免进程崩溃时关键日志滞留缓冲区。
- gateway 支持 `PIVOTMIND_LOG_FILE=路径` 将 stdout/stderr 一并落盘：用 dup2 而非 freopen（crash handler 直接 write(2, ...)，fd 重定向后 [CRASH] 崩溃现场同样写入文件，配合服务托管可回溯崩溃），fd 带 O_CLOEXEC。

### P2-7 CI 增 ASan/UBSan 门禁（.github/workflows/ci.yml）

- 新增 `asan-ubsan` job：`-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1` 编译 + 核心单测在 sanitizer 下运行，`halt_on_error=1`。
  - **更正（第三批）**：原实现把 ASan 旗标在 CI 里**另写了一份**、与 `Makefile` 的 `ASAN_CFLAGS` 不一致（CI 有 `-DHAS_OPENSSL`/`-lssl -lcrypto -lz`，本地 `make asan` 没有，而默认出货构建有）→ "CI 绿 ≠ 本地 asan 绿"。已收敛为单一来源（`Makefile` 的 `ASAN_CFLAGS`/`ASAN_LDFLAGS` + 新增 `make asan-test`），CI 只调 `make asan-test`。
  - **更正（第三批）**：原 `ASAN_OPTIONS` 含 **`detect_leaks=0`**，**关闭了 LeakSanitizer**，与本条"内存错误…直接红"的宣称不符（nn 层满是大块 `malloc`，恰是最该开的地方）。已改为 `detect_leaks=1`。故本条的准确表述应为：**内存错误（含泄漏）与未定义行为直接红**。

### P2-6 gateway 单文件按模块拆分（demos/pivotmind_gateway.c → 6 文件）

- 原 `demos/pivotmind_gateway.c`（~2200 行，含 HTTP 解析、系统初始化、学习 worker、全部 REST handler 与 main）拆为 6 个文件，按职责收敛：
  - `demos/gateway_internal.h`：共享类型（HttpRequest/GatewaySystem/LearnTask 前向 typedef）、宏与跨模块函数原型（185 行）；
  - `demos/gateway_http.c`：JSON 转义/抽取、http_send/http_json、常量时间 token 比较、parse_request（HTTP 解析，含 Content-Length 边界加固逻辑）；
  - `demos/gateway_system.c`：gw_system_init（static，仅 init 线程调用）/ init_thread / shutdown —— 引擎初始化、seed 落盘、train_mode 管理；
  - `demos/gateway_learn.c`：LearnTask 队列、_learn_tokens 单消费者 worker、flush 等待；`g_learn_q`/`g_learn_workers` 保持模块内 static；
  - `demos/gateway_handlers.c`：handle_chat/learn/feedback/media_feed/media_status/status/root/scheduler/scheduler_self_stats/health/qa 全部 REST handler；
  - `demos/pivotmind_gateway.c`（瘦身 ~590 行）：API token 生命周期、handle_connection 路由、P0-1 每连接线程、crash handler 与 main。
- 路由层不再内嵌 handler 实体：`handle_connection` 只做方法/路径分发并调用 gateway_handlers 导出符号；handler 内部逻辑原样搬迁，零行为改动。
- 跨模块符号经 gateway_internal.h 显式声明（导出面收敛：仅 handler 路由所需的 handle_*、http 工具、parse_request、learn_queue_*、gw_*_thread/shutdown 等 23 个符号）；模块内部辅助函数（utf8 校验、learn_task_complete/abandon、gw_system_init 等）保持 static，不扩大 API 面。
- Makefile：gateway 链接规则纳入 GATEWAY_OBJ（5 个 .o），demos/*.c 通配自动编译各拆分模块。

## 验证

- WSL gcc `-Wall -Wextra` 语法校验零告警（5 个 gateway 文件 + internal.h）。**注意：这是"语法/编译级"校验，不是构建产物验证，也不是运行验证。**
- **【可复现的编译验证（仓内，任何人可重跑）】**：`make -j2 gateway` 在 aarch64 上 EXIT=0、`-Wall -Wextra` 下 0 warning / 0 error，产出 `libpivotmind.a`(≈4.85 MB) 与 `build/bin/pivotmind_gateway`(≈498 KB, ELF aarch64)。此条由 2026-09-10 的独立审查（`E-nn-build-tests.md` 构建实录）在 `/tmp` 副本上实测复现。
- 符号完整性审计：拆分后函数集合与 HEAD 38 个顶层函数逐一比对无缺失；P0-1（GW_MAX_CONN/g_conn_count 原子计数）、P0-2（memory_seed 原子落盘）改动均保留在拆分模块内。
- git diff 共 16 文件变更（含 4 新增 .c + 1 internal.h + Makefile）；CHANGELOG 同步。
- 记忆种子新旧格式兼容路径与哈希校验路径经代码审查核对。
- 复审追补修复后重跑 WSL gcc `-Wall -Wextra` 语法校验（src/memory_system.c、src/thread_pool.c、demos/gateway_learn.c、demos/gateway_system.c、demos/gateway_internal.h、gateway_http.c、gateway_handlers.c、pivotmind_gateway.c）零告警。
- 种子哈希对称性核对：保存端哈希输入 = 记录区（key_len/key/data_sz/data/type/importance 逐项），加载端第一遍哈希输入 = 同序列；footer 8 字节魔数 + 8 字节 hash 均不参与哈希，两端范围一致。
- 种子保存/加载验证：3 条记录（FLOAT/STRING/BINARY，134 字节）保存后重新载入返回 3，逐条数据/类型/重要性一致；落盘 hash == `FNV(记录区)` 且 != `FNV(记录区‖MAGIC)`；篡改 1 字节 → 返回 -1 且未部分加载；空种子（16 字节纯 footer）往返返回 0；截断文件 → 返回 -1。
  - **⚠️ 验证方式与可复现性（如实声明）**：以上结论来自 **WSL gcc 15 上编译 `memory_system.c` 并运行一个仓外临时测试程序（20 项断言）**。该程序**不在本仓库中**（`tests/` 下无对应文件），因此**不可复现、不受 CI 保护**，也不会在后续改动中自动回归。
  - **待办（归属第三批 I 组）**：把这些断言固化为仓内测试，本报告第 E-P1-11 条与本轮"门禁与诚实批"已记录该缺口。
- 学习 worker 生命周期核对：`learn_queue_shutdown` 为唯一 stop 置位点与 join 点，调用点是 `gw_system_shutdown` 第一步，早于全部 destroy。

## 修改文件

| 文件 | 类型 |
|------|------|
| demos/pivotmind_gateway.c | P0-1 / P2-2 / P2-4 / P2-6 拆分后主入口 / 安全加固 |
| demos/gateway_internal.h | P2-6 新增：共享类型/宏/跨模块原型；P0-4 原型声明 |
| demos/gateway_http.c | P2-6 新增：HTTP/JSON 工具 + parse_request |
| demos/gateway_system.c | P2-6 新增：系统初始化/保存/关闭；P0-4 shutdown 首步停 worker |
| demos/gateway_learn.c | P2-6 新增：学习队列与 worker；P0-4 learn_queue_shutdown |
| demos/gateway_handlers.c | P2-6 新增：REST handler |
| Makefile | P2-6 gateway 链接规则 |
| src/memory_system.c | P0-2 / P0-3 哈希对称性修复 |
| src/thread_pool.c | P1-1（含 batch_count 安全前提注释） |
| include/error.h | P2-3 / P2-4 |
| src/error.c | P2-3 / P2-4 |
| .github/workflows/ci.yml | P2-7 |
| include/pivotmind_version.h | 版本 bump 0.5.25 |
| README.md | 版本同步 |
| CHANGELOG.md | 本版本条目 |
