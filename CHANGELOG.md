# Changelog

## v0.5.26 — 2026-09-10

> 来源：**全量基线审查（19 P0 / 26 P1）+ 修复轮 2**。commit `5d6c9b1`，相对基线 `3eb2e6e`，35 个文件 `+1442 / −437`，按五批落地：① 数据保命 ② 并发与生命周期 ③ 门禁与诚实 ④ N17 扩散上限 ⑤ 验证期发现的既有缺陷。完整发布说明（含验证证据、红线声明、已知未修问题）见 [changelogs/070-review-round2-persistence-concurrency-gates.md](changelogs/070-review-round2-persistence-concurrency-gates.md)。

### Fixed

**数值与解析**
- **小矩阵乘法读未初始化内存（E-P0-1，数值正确性）**：`matrix_multiply_naive` 对 `tensor_create` 分配的未置零缓冲做 `+=` 累加；现于入口 `memset`，并补逐元素数值断言（`tests/unit/test_tensor.c`）。此前 `test_tensor` 只断言 shape/size，且历史上用"调小期望值"的方式让红灯变绿（见 v0.5.22 更正）。
- **`_sample_negative` 均匀采样分支无循环上限（P2-4 复核）**：`vocab->size <= 5` 时 `sampled < 5` 恒真 → 死循环；另补 `vocab` 空指针守卫。报告中"`pretrain.c:240` 除零"经核对**不成立**（该函数入口已有 `vocab->size <= 5` 守卫，该行不可达）。
- **`model_io.c` 加载畸形文件可致堆溢出（P2-1，32 位目标）**：`ndim` 补上界、`input*output*sizeof(float)` 补回绕守卫（权重与偏置两路）。
- **`lr_reduce_on_plateau` 未过滤 NaN（P2-3）**：非有限 `val_loss` 现直接跳过平台期更新。

**① 数据保命（第一批）**
- **种子加载失败不再被空状态覆盖（G2）**：加载失败原会继续走空状态初始化并回写覆盖原文件，等于"读不出就抹掉"；现加载失败即拒绝回写，保留原种子文件。
- **种子 footer 完整性校验——缺 footer 默认拒绝（D2）**：0 字节文件、尾部 1~15 残字节、16 字节非 `PMSEED2` footer 一律拒绝加载；无 footer 的旧格式**默认拒绝**，须显式 `PIVOTMIND_ALLOW_LEGACY_SEED=1` 才按旧格式一次性迁移接受。
- **死索引 `index_map` 停用（D3）**：全仓"只写不读"（create/store/destroy 之外零引用），且驱逐路径存在 `index_map[index_size]` 堆越界写；现停止维护该冗余索引（恒空、`index_size` 恒 0，与实际占用一致）。
- **`fsync(file) + fsync(dir)` + 唯一 tmp 名（A-P1-2）**：临时名加 pid + 单调序号防并发/重入互踩；先 `fflush + fsync(file)` 再 `fclose` 再 `rename`，并对目录 fd 补 `fsync`，确保目录项本身落盘（`memory_system.c`）。
- **`emergent_pos` 原子写（A-P1-5）**：同样 tmp+rename + `fsync(file)/fsync(dir)`；逐写检查返回值，磁盘满/写失败不再谎报"持久化完成"（`src/emergent_pos.c`）。
- **四处"0 节点存盘"门卫（D1）**：与既有"拓扑 total>=20"门卫对等，种子内容按 LTM 条目数判定（下限 `MEMORY_SEED_MIN_ENTRIES=1`），空状态不回写覆盖。

**② 并发与生命周期（第二批）**
- **关闭期 UAF：网关连接线程槽表 + join（C1）**：旧版无法 join 连接线程，只能"轮询计数 15s 后强拆 main 栈上的 `gw`"，慢连接线程随后访问已释放内存；新增连接线程登记槽表（`g_conn_mutex` 保护，`used`/`ever` 双标记）+ 主循环机会式回收，关闭时对 `ever==1` 的槽逐个 join。
- **init 线程 join（H2）**：初始化线程此前无人 join，关闭时可能仍在加载并重建 worker；现关闭路径显式 join init 线程，join 返回即代表加载流程已停。
- **learn 队列生命周期守卫（C3）**：关闭已开始（`stop` 置位）后绝不再入队；`learn_queue_shutdown` 只 join "创建成功"的 worker 槽（原实现无条件 join 全部槽，`pthread_create` 失败时 join 的是零值 `pthread_t`，属未定义行为）。
- **共享线程池批次闸门（C4）**：单例池同一时刻只允许一个批次；池忙时 `batch()` 立即返回 `THREAD_POOL_BUSY(-2)` 且不执行任何任务，调用方（`dialog_system.c` / `multi_topology.c`）据此串行降级，消除两处并发提交互串批次。
- **线程池 shutdown 守卫 + 共享状态加锁**：批次进行中不响应 `shutdown`，worker 完成本批后由 `batch()` 正常收尾；批次状态重置移入锁内。
- **`learning_scheduler` 引入 `thread_started` 旗标（B-P1-2）**：以旗标而非对象地址判定句柄是否有效，决定 `stop` 能否 join；`stop` 幂等（含 destroy 内部那次重复 stop 直接返回），避免 `join(0)`。

**⑤ 验证期发现的既有缺陷（第五批，多为基线自带）**
- **`autonomic_stop_async_flush` 用 `state->initialized` 当存活判据 → `pthread_join(0)`**：`initialized` 并不代表 flush 线程句柄有效，导致 `join(0)` ——glibc 2.43 下 SIGSEGV、glibc 2.39 下静默返回 `ESRCH`。该缺陷**基线自带**（`ab1f79e`，2026-06-04 引入）。改用 create 成功之后才置位的 `flush_started` 旗标判定。
- **`master_topology_create` 从不初始化 `master->node_cache`**：`malloc` 不置零，字段是脏指针而全仓使用点都写成 `if (master->node_cache)` 守卫形式 → 非 NULL 垃圾值绕过守卫解引用野指针（ASan 下 `diffusion.c` 解引用 `node_cache->auto_thaw_ok` 已实测 SEGV）。现显式初始化 `node_cache`/`ext_dict`/`cognitive_state_ptr`/`_pad_parallel_mode`。
- **`multi_topology.c` 每步 `calloc` 被剪枝 `break` 跳过 `free`**：`path_target_weights` 在循环体内每步 `calloc`、循环体末尾 `free`，但循环内多个 `break` 退出点（剪枝 / 语义场休止 / 候选耗尽）会跳过释放 → 库侧泄漏。现上提到循环外单次分配，每步 `memset` 复位（语义等价），在函数唯一返回路径释放。
- **`tests/unit/test_memory.c` 自身 `strdup` 未释放**：测试内的泄漏，已补 `free`。

### Changed
- **N17 扩散上限（第四批）**：删除 `src/diffusion.c` 的 `SPREAD_MAX_EXTRA=256` 与栈数组上限；改为**每跳累加全部入边贡献后按强度闸门裁决**（θ = max(0.001, 0.15×本跳峰值)），消除"首边独占、顺序即命运"。实测：候选 2000 = 点亮 1000 + 落选 1000，点亮数不再被 256 卡住，且把边顺序整体对调后账行**逐字节相同**。

### Quality
- **CI 真的跑测试了（E-P1-A）**：原 "Run tests" step 调的 `make test-*`（Makefile:248-271）是**纯构建别名、从不执行**，15 个测试必挂也让 CI 变绿；现改为 `make test`（构建 + 执行 + 汇总 + 退出码门禁）。
- **桩测试清出**：删除 8 行 Hello World `test_io.c`；`test_chinese.c` 改写为 UTF-8/CJK 真断言（不再需要 windows.h，CI 不再跳过）；`tests/scratch/test_tensor_broadcast.c` 移入 `tests/unit/` 并接线到 `make test`（即此前闲置的广播测试接进 CI）。
- **测试目标集统一**：`test:` 前置与 `TEST_BINS` 逐项一致；`test-integration`/`test-semantic-growth`/`test-tensor-broadcast` 全部纳入，消除"定义了却没人跑"。
- **ASan 门禁收敛（E-P1-12）**：旗标单一来源（Makefile），本地 `make asan-test` 与 CI 一致且覆盖 `-DHAS_OPENSSL` 出货代码路径；`detect_leaks=1`（此前为 0），与 changelogs/069 宣称一致。
- **随机种子可复现（P2-2）**：新增 `init_random_seed()` / `init_random_from_env()`（`PIVOTMIND_SEED` 环境变量），既有调用点零改动。
- ARM 交叉构建删除纯 C 项目无意义的 `-static-libstdc++`。
- **文档措辞如实化（第三批）**：把"验证过"的表述标注为"**仓外临时程序、不可复现、不受 CI 保护**"（同步 changelogs/069 的验证章节）。
- **验证状态（如实）**：**aarch64 / armbian**（glibc 2.39）`make -j2 all` exit 0、全套 **23/23 通过**、ASan（`detect_leaks=1`）**5/5 干净、零泄漏**；**x86_64 / G15-WSL**（gcc 15.2、glibc 2.43）ASan **5/5 干净**、全量 **22/23**——`test_cognitive_controller` 崩溃，已定位为上述 `autonomic_stop_async_flush` 的 `pthread_join(0)`，**本版已修**。
- **x86_64/TSan（首次在 x86_64 上跑并发检测）**：抓到 `dialog_topo_worker` 的 heap-use-after-free（与基线 `3eb2e6e` 同型，见 Known Issues）及若干独立并发站点；**两条须连同结论一并采信的边界**：(1) TSan 运行中出现过 `WARNING: ThreadSanitizer: memory layout is incompatible, possibly due to high-entropy ASLR`，此类警告下报告需谨慎采信；(2) `test_cognitive_controller` **几乎没有任何断言**，其在 aarch64 上的"通过"说服力很弱（它恰是 x86_64 下崩的那一支）。另：修复树 TSan 因 OpenMP 报告洪水未跑完（工具限制），UAF 报告在首分钟内取得。

### Known Issues
- **未修（基线自带 `heap-use-after-free`）**：`src/dialog_system.c:149` `dialog_topo_worker`——主线程 `dialog_reasoning_create` 用完批次任务即 `free`（本版 `:814`／基线 `:810`）而 worker 仍在读，基线 `3eb2e6e` 报出**同型** UAF（`:149`/`:162`）故属**既有债**；本版新增的 `in_batch` 批量闸门（C4）只堵"并发第二批次"、**不覆盖该路径**（根因是 `thread_pool_batch` 的完成屏障本身不成立——`workers_done` 记的是"趟数"却被当"人头数"用），**本版未修**，round 3 方案在修（分支 `fix/round3-concurrency`）。此处如实记录，不作"已修"或含糊表述。

### Notes
- **红线声明（本版一律未动）**：所有限边 / 截断 / 周期性稀疏化 / 跨拓扑上限 / 队列满丢任务逻辑，按作者架构红线本版均未触碰。

---

## v0.5.25 — 2026-09-09

### Fixed
- **线程池 destroy/超时路径 UAF 与串批隐患（P1-1）**：批次状态重置移入锁内（旧版无锁重置 + volatile 属 C11 数据竞争 UB）；`shutdown` 仅在「非批次进行中」才响应，destroy 恰逢批次进行时 worker 完成本批并上报 done，batch() 正常收尾；worker 用锁内快照 `batch_count` 窃取任务，防本批超时强退后慢 worker 串批执行下一批任务；等待完成不再"10s 超时强制结束返回成功"（会 UAF 栈上 tasks），超时改持续告警等待；batch 收尾广播 `cv_done`，destroy 不再干等 5s 超时。
- **记忆种子截断即全损且无校验（P0-2）**：旧版 `fopen(path,"wb")` 直接覆盖，崩溃/断电写中途截断原文件无备份可回退；加载边读边 store，损坏文件被静默"部分加载"。改为 tmp+rename 原子写 + FNV-1a 64 哈希 footer（MAGIC `PMSEED2`），加载两遍解析先校验后提交，损坏拒绝加载，旧格式（无 footer）保持兼容。
- **记忆种子哈希校验两端不对称（P0-3）**：保存端把 footer 魔数 `PMSEED2` 也喂进哈希后才落盘 hash，加载端只对记录区算哈希 → 保存端 `FNV(记录区‖MAGIC)` vs 加载端 `FNV(记录区)`，所有新格式种子文件校验恒失败、保存后即不可读。修复：魔数改裸 `fwrite` 不参与哈希，与加载端第一遍解析范围严格对齐。
- **种子加载循环边界把 footer 当记录解析（P0-5）**：第一遍解析循环用 `while (pos + 16 <= sz)`，最后一条记录读完后剩余恰好 16 字节仍进循环，把 footer 魔数前 4 字节 `"PMSE"` 当 `key_len`（1163010384 > 4096）→ 恒判"记录截断"→ 新格式种子即使哈希正确也 100% 加载失败，空种子（16B 纯 footer）同样被拒。修复：条件改 `while (pos + 16 < sz)`。实测 3 条记录文件往返一致、空种子返回 0、篡改/截断文件仍被拒。
- **学习 worker 无退出机制（P0-4）**：`g_learn_q.stop` 无任何置位点、worker 从未 join，`gw_system_shutdown` 直接销毁 brainstem/topology/perception 等，而 worker 仍在消费任务访问 `gw->topology`/`gw->perception` → 关闭时 use-after-free。新增 `learn_queue_shutdown()`（stop + broadcast + join 全部 worker + 防御性排空残留），并在 `gw_system_shutdown` 最前调用，早于任何 destroy。
- **网关 token 比较时序侧信道（C1）**：strcmp 逐字节提前返回可被响应时间逐位爆破 → 全程遍历 + XOR 累积的常量时间比较（长度不等直接拒绝）。
- **Content-Length 解析歧义**：只扫描 header 区间 `[0, header_end)`，防 body 内同名文本被误当头部；`atoi` → `strtol` 严格校验非法数字。

### Changed
- **网关每连接独立线程（P0-1）**：旧版单线程串行 accept→处理，慢上传/同步推理阻塞后续所有连接、/health 被拖死；改独立线程 + `g_conn_count` 原子计数限并发（`GW_MAX_CONN=64`），超限直接 503；退出有界等待在途连接线程（15s），避免 detached 线程访问已释放的 gw。
- **token 打印脱敏**：启动日志不再整段明文打 token，仅显前 4 后 4，完整值存 GW_TOKEN_FILE（0600）。
- **端口占用探测去重（P2-2）**：删 3 处重复 connect 预探测与重复 `/tmp/pivotmind.port` 写入；预探测只连 127.0.0.1 与实际 bind 地址不一致且存在 TOCTOU，改直接 bind 按 errno==EADDRINUSE 判定。
- **日志线程安全 + 可落盘（P2-4）**：error.c 新增 `log_set_output(FILE*)`（NULL 恢复 stderr），日志整行加 pthread 互斥防并发写交错；`level >= LOG_WARNING` 即 fflush；gateway 支持 `PIVOTMIND_LOG_FILE=路径`，dup2 重定向 stdout/stderr（crash handler 直接 write(2) 同样落盘，fd 带 O_CLOEXEC）。
- **错误码扩展（P2-3）**：`ErrorCode` 末尾追加 ERR_IO_ERROR/ERR_TIMEOUT/ERR_BUSY/ERR_UNAUTHORIZED/ERR_BAD_REQUEST/ERR_PARSE_FAILED/ERR_INVALID_STATE/ERR_CHECKSUM_MISMATCH，头文件注明"新增必须追加末尾禁中间插入"（错误码按数值持久化/对外引用）；error_string() 补齐。

### Refactor
- **gateway 单文件按模块拆分（P2-6）**：`demos/pivotmind_gateway.c`（~2200 行）拆为 6 文件——`gateway_internal.h`（共享类型/宏/跨模块原型 185 行）、`gateway_http.c`（JSON/HTTP 工具 + parse_request）、`gateway_system.c`（初始化/保存/关闭，gw_system_init 保持 static）、`gateway_learn.c`（LearnTask 队列与 worker，队列状态模块内 static）、`gateway_handlers.c`（11 个 REST handler）、主文件瘦身 ~590 行（token 生命周期/路由/P0-1 连接线程/crash/main）。handle_connection 只做路由分发，handler 逻辑零改动搬迁；导出面收敛为 23 个跨模块符号，模块内部辅助函数保持 static。Makefile 链接规则纳入 GATEWAY_OBJ。

### Quality
- CI 新增 `asan-ubsan` job（P2-7）：ASan/UBSan 全量编译 + 7 个核心单测（model/memory/topology/dialog/learner/causal/forgetting）跑 sanitizer，`halt_on_error=1`。
- WSL gcc `-Wall -Wextra` **语法校验**零告警（gateway 6 文件 + memory_system/thread_pool 等改动文件）——注：语法校验 ≠ 构建验证 ≠ 运行验证。另有**仓内可复现**的构建验证：`make -j2 gateway` 在 aarch64 上 EXIT=0、0 warning。
- 记忆种子保存/加载验证 20 项断言全通过（往返一致 / 哈希对称 / 篡改与截断拒绝 / 空种子）——**验证方式：WSL gcc 15 上的仓外临时测试程序（不入库、不可复现、不受 CI 保护）**，详见 changelogs/069「验证」章节的可复现性声明。
- 符号完整性审计与 HEAD 38 个顶层函数逐一比对无缺失；YAML 合法；git diff 共 16 文件（含 4 新增拆分 .c + internal.h + Makefile）。

详见 [changelogs/069-code-review-optimization-round.md](changelogs/069-code-review-optimization-round.md)

---

## v0.5.24 — 2026-09-06

### Fixed
- **存盘空批误判 OOM（根治）**：候选A 流式批化存盘主循环用 `if (!batch)` 一刀切判 OOM，把「空拓扑/空批」（情绪拓扑惰性初始化 node_count==0）误判成 OOM → break 放弃整轮，主状态文件自 9-03 起长期无法更新。改为「先以 cnt==0 判空批推进，再以 batch==NULL 判真 OOM」。
- **锁内写路径饿死**：删除旧内存门卫（est+512MB 回退锁内写），主流程改流式批循环（每批 ≤100 节点短持锁深拷贝 + 锁外序列化追加写），磁盘 I/O 100% 在锁外。
- **崩溃 handler 纯 async-signal-safe**：彻底删 backtrace 系列（内部走 dlopen/dladdr 会再入动态连接器与 malloc，堆损坏时自锁死锁），仅 write(2) 输出信号行后 _exit。

### Changed
- **激活记录 pthread_key 化**：__thread 线程局部存储改堆分配 + pthread_key，支持可重入。
- **常量扩容**：PM_EDGE_TRACK 128→256、PM_ACTIVATED_PAIRS 4096→8192。
- **concept 解析直查**：快照序列化目标 concept 从 concept_by_id 查表改为 net->nodes[id]->concept 直接查。

### Quality
- 编译 `-Wall -Wextra` 零警告；单测 topology 3/3、memory 4/4。
- 实测：喂料 50 条节点 30167→30367、跨重启加载零丢失，OOM 报错归零。

详见 [changelogs/068-streaming-batch-save-oom-fix.md](changelogs/068-streaming-batch-save-oom-fix.md)

---

## v0.5.23 — 2026-08-19

### Added
- **top-K 缝合（绑定三件套①）**：`topology_walk_greedy_topk` 每步候选 top-3 + 综合打分（激活+跨拓扑预加热+模板），单链贪心升级；K=1 逐位等价，其余调用方零影响。
- **cross_hit 持久化（绑定三件套②）**：跨拓扑联合激活计数落盘（STATE_FORMAT_VERSION 7→8），跨重启不归零，≥5 次自动建边闭环可达成。
- **种子词表（绑定三件套③）**：16 个实体种子词预注册整词节点，强制 TOPO_VOCABULARY，带开关；实体词不再被拆成字符碎片。

### Fixed
- **R6 锁模型**：net->nodes 双路径 realloc 竞态（add_node net->mutex / auto_extend master->rwlock 互不排斥）→ net->mutex 唯一权威锁，写侧补 4 缺锁点 + 读侧补 3 缺锁点，锁序恒 master→net。修复凌晨全量重喂下 SIGABRT（堆损坏）+ SIGSEGV（auto_learn_concepts 读野指针）。
- **种子词落错拓扑**：预注册强制 TOPO_VOCABULARY（原随喂料领域拓扑）。
- **网关 token 持久化**：随机生成 + gw_token 文件（0600）记住有效 token，跨重启不变。
- **watchdog 双实例并发**：gateway_watchdog.sh 加 flock（08-18 21:00 状态缩水事故元凶）。

### Changed
- **树莓派黑匣子探测**：适配网关绑 127.0.0.1（SSH 板上本地 curl + 日志动态取 token）。

### Quality
- 单测全绿：topology 3/3、tensor 13/13、learner 3/3、dialog 4/4；全仓编译零警告。

详见 [changelogs/067-binding-trilogy-lock-model.md](changelogs/067-binding-trilogy-lock-model.md)

---

## v0.5.22 — 2026-08-18

### Security
- **网关全端点鉴权**：`X-Pivot-Token` 覆盖除 /health、/healthz 外所有端点（/chat /learn /feedback /qa /debug /force_templates /train/* 等）；默认绑定 127.0.0.1（`PIVOTMIND_BIND_ADDR=0.0.0.0` 可覆盖），局域网直接访问面关闭。
- **qa_crawler 注入面清除**：`system()` 全清零 → `fork+execvp`（argv 直传杜绝 shell 注入）+ 父进程超时 SIGKILL 保护；URL 白名单默认拒绝 + 协议/域名/路径三级校验（封堵子串伪造）。

### Fixed
- **SIGSEGV 真根因修复**：`object_pool_acquire` 扩容分支 `free_count` 恒 0 → `free_list[-1]` 越界读返回垃圾指针——infer 建图边数超过池容量时崩溃（"薛定谔的猫"类因果查询偶发 SIGSEGV；GPT 审查/pro 复审曾误判为缓存悬垂）。压测 6/6 因果查询零崩溃。
- **causal_reasoning 缓存悬垂**：`causal_associative_search` early-return 路径 destroy 共享缓存 `g_cg_cache` 后未置 NULL（且锁外 destroy）→ 改为不销毁，统一由指纹变化分支管理。
- **UTF-8 标点比较**：gateway CJK 多字节字符常量 vs 单字节 char 恒 false → strncmp UTF-8 序列比较，汉字标点真正计入统计。
- **corpus_train fread 缓冲未终止**：按实际读取数定 NUL 位置。

### Changed
- **Makefile 并行度**：删 `MAKEFLAGS += -j$(nproc)`（3.8GB 板全核编译 OOM）→ `JOBS ?= 2`，命令行 -j 优先。
- **_ar_find_pair 查找优化**：PairEntry 固化键哈希 + 每槽 2×strcmp → 1 次 int 短路 + rehash 免 snprintf 重哈希（哈希同值性 10 万对验证，语义零漂移）。澄清：查找本就是开放定址哈希，O(n²) 真根因（三字扩展双层迭代）已于 3878e84 修复。

### Quality
- **test_tensor 13/13**：3 处断言修复（reshape 3×5→{1,15}、matmul size 6→4、NULL 输入测试传参错误）。
  - **⚠️ 更正（第三批「门禁与诚实」）**：其中 "**matmul size 6→4**" 是**把期望值改小以让红灯变绿**——该用例（2×3 乘 3×2）的正确结果尺寸本就是 4，真正的缺陷是**测试从不校验元素数值**，而 `matrix_multiply_naive`（小矩阵路径）用 `+=` 累加到 `tensor_create` 的**未初始化缓冲**（对照 `matrix_multiply_blocked` 有 `memset`）→ 堆一复用就产生随机错误结果。本批已修正：naive 路径入口补 `memset`，并补逐元素数值断言（期望值 `{9,12,9,12}`，注释中给出心算过程）。
- **全仓编译警告清零**：20 条 -Wall -Wextra 全消（未用变量/未用参数/符号比较/多字节字符常量等）。

详见 [changelogs/066-security-hardening-crash-fix.md](changelogs/066-security-hardening-crash-fix.md)

---

## v0.5.21 — 2026-08-15

### Security
- **C1 远程 RCE 封堵**：`media_reader.c`/`visual_cortex.c` 7 个 `popen/system` 调用点全部 execvp 化（去 shell 解释层）；`/media/*` 加 `X-Pivot-Token` 鉴权（启动生成 64 位 hex token 打印日志，无 token 401）；realpath+S_ISREG+媒体目录白名单灭 SSRF；`"> NUL"` Windows-ism 修复。
- **B1 边界**：H1 状态加载 `from_node` 范围校验（防损坏文件 OOB/OOM）；M4 去重分支 `return -1` → `return result`。

### Fixed
- **B3 锁外快照写盘**（STUCK 根治）：存盘从"持 master 读锁写 330MB"改为"三锁同时拿全量深拷贝 + 锁外写盘"，字节级等价，`PIVOTMIND_SAVE=locked` 回退。
- **B4 learn 队列化**：`handle_chat` 同步学习入队（输入 flush / 回复 fire-and-forget），worker 2→1，`_learn_tokens` 零改动。
- **B2 broca 越界**：`pos_tags[64]` 栈越界读 → 动态分配；空格插入/NUL 无边界 → 扩容 helper。
- **C1 side_inhibit 字节 bug**：`strncmp(...,2)` 比 2 字节（CJK 误判"一/丁"）→ UTF-8 完整字符比较。
- **C2 词锚重叠去重**：三处词锚插入点加字符重叠检查，修"方鸿+鸿渐"裂词。

### Changed
- **C3 `MAX_REPLY_WORDS` 常量化**：主路径与降级路径共用，便于 A/B 实验。

### Experiment
- **4→6 词 A/B**：6 词只放大词层拼接垃圾、质量不升 → 回退 4。信息量瓶颈在选词/组句，不在长度限制；生成端微调收手，等 dist_sig 数据上阶段 2.5/3。

详见 [changelogs/065-security-concurrency-generation.md](changelogs/065-security-concurrency-generation.md)

---

## v0.5.9 — 2026-08-07

### Added
- **字符对/字符表记忆化**（用户拍板架构方向：一切统计=记忆，遗忘≠删除）：`PairEntry`/`CharEntry` 加巩固度/活跃度/短期长期层级；Hebbian 巩固（0.02×(1-c)），≥0.5 晋升长期永不删；表满清理只清「未巩固+长期不活跃」（重建式删除防探测链断裂）；冷启动零误杀。
- **/learn 队列化**：固定 2 worker + 256 队列（满丢最旧）——不再每请求 spawn 线程，线程 35→6（40 次采样全 6）。
- **POS 池 256→64**：聚类快 16 倍。
- **探索记录生命周期**：悬垂回收（节点冻结/删除的记录）+ 最久未探索覆盖。
- **路径频率表淘汰修正**：低频+长期不活跃才驱逐（旧 count 优先会误杀刚学路径）。

### Changed
- **字符对表哲学**：记账本 → 突触（形成→巩固→降权→不删除），与 Hebbian 边权/冻结/三重记忆统一。
- **/learn 并发模型**：每请求一线程 → 队列+固定 worker（与感知 worker 同构）。

详见 [changelogs/064-charpair-memory-learning-queue.md](changelogs/064-charpair-memory-learning-queue.md)

---

## v0.5.8 — 2026-08-07

### Fixed
- **主循环网络阻塞根治（gdb 三次抓栈实锤）**：感知搜索异步化（`perception_tick` 只入队，worker 线程串行搜索——主循环永不碰网络）；自学锁外搜索（`self_learner_cycle` 锁内只收集概念名）；主循环自学改 `perception_enqueue_search` 异步入队；`g_fetch_lock` 改 timedlock（8s 超时放弃）。
- **两处漏锁（各 4 处提前 return）**：`autonomic_compound_consolidate` 漏解锁 → master 写锁永久持有；`_article_flush_locked` 漏解锁 → ar->mutex 永久持有 → 274 线程堆积雪崩。均改 goto 统一出口。
- **POS 池喂养锁外化**：tag_soft 含 O(n²) 聚类（256² 矩阵 2-5 秒），锁内执行导致学习线程排队堆积——改锁内只收集概念名，`article_flush` 解锁后喂养。

### Added
- **POS 池管道**：喂料路径（article_reader）首次喂养语法拓扑——新词查询词性进池 → 池满聚类 → **额外词类 0→1 历史性破零**（08-04 基线 0：POS 池只吃生成/对话路径，玄枢不会说话 → 死循环打破）。
- **`perception_enqueue_search`**：异步入队式搜索，调用方零阻塞。
- **聚类后清池**：`try_emerge` 检查后清空未分类池，散词不再反复触发 O(n²) 聚类。

### Changed
- **版本策略**：0.5.7 → 0.5.8（小步更新）。
- **黑匣子哨兵**：no-agent 纯告警 → agent 模式（告警自动触发调查处理）。

详见 [changelogs/063-perception-async-pos-pipeline.md](changelogs/063-perception-async-pos-pipeline.md)

---

## v0.5.7 — 2026-08-01

### Fixed
- **rwlock 死锁根治**：glibc 写锁内嵌套读锁返回 EDEADLK，忽略返回值后错误 unlock → 锁状态损坏 → 永久卡死。`cross_link_exists` 拆 `nolock` 变体。
- **知识被清光多层根因**：load_protect 未初始化（保护期从未生效）+ 时间保护期 30 分钟 + 保底激活 0.3 + 边权保底 0.2 + 修剪跳过 is_cooled + RED 阈值放宽。
- **词巩固并发 SIGSEGV**：全程持 master 写锁（防对话线程 realloc 悬垂）。
- **PFE 四大 O(N²) 卡点**：因果图去重 → djb2 哈希桶；加边查重/统计/环检查 → no_check 批量版；边查询 → 邻接表（outgoing 存边索引）；A* 扩展 5 万 target → 上限 2000 + 探索 100。
- **因果图缓存悬垂**：causal_associative_search 不再 destroy 共享缓存图。
- **上下文注入串扰**：禁用上轮回复拼接（词锚定命中旧回复词）。
- **子目标主语提取**：extract_subject 重写（"历史为什么重要"→"历史"）。

### Added
- **词层架构**：词巩固（相对/绝对强度双通道涌现 + 虚字过滤 + 高频方向定词序）→ 概念拓扑晋升 + cross-link 回字；词锚定优先输出。
- **话题性三层**：relevance 评分（候选与锚定集边权，<0.3 过滤）+ 有界联想回退（0.3-0.5 次相关）+ 话题序组装（≤4 实词短句 + 中文单字过滤）。
- **PFE 推理管线跑通**：推理词保底触发 + 子目标分解 → 因果搜索/diffusion 求解 → 推理链合成（6 子目标 + 置信度）；28s 卡死 → 2s。
- **词级语义场**：feed_cli 2 字窗口配对词候选 + 词-词 Hebbian 共现边 + diffusion 词邻居扩散。
- **词级语义拓扑**：词聚类语义生长（激活排序采样 + 特征 0 填充）+ 语义场查询（词→概念→成员词）。

### Changed
- **词级虚字表**：缩小到"绝对虚字"——"过去/将来/上面"等含字级虚字但整体实义的词保留（用户纠正）。
- **功能词兜底**：纯"很+X"功能词回复 → "好的。"
- **版本策略**：只能小步更新（0.5.7 这种），大版本（0.6）必须用户拍板。

详见 [changelogs/062-word-semantic-field-reasoning-pipeline.md](changelogs/062-word-semantic-field-reasoning-pipeline.md)

---

## v0.5.6 — 2026-07-27

### Fixed
- **node_hash 整数溢出崩溃**：`node_hash_reserve` 参数 `int` → `size_t`，节点数达 40009 后 `calloc(负数)` → crash。结构体 `bucket_count`/`node_count` 同步改为 `size_t`，去掉人工上限，靠 OOM 兜底自然增长。
- **编译警告清零**：`node_hash.c` 修复 5 处 `size_t` 格式串与符号比较警告。

### Changed
- **内感受驱动冻结**：`brainstem_tick_freeze` 接入 `health_monitor` 健康等级，不再固定 10/轮：
  - GREEN → 600 tick, 10/轮
  - YELLOW → 200 tick, 50/轮
  - RED → 1 tick, 不限量 + 删孤立冻节点
- **RED 级节点回收**：冻结后调用 `prune_isolated_nodes` 真删 0 边冻节点 + `master_prune_dead_nodes` 清理死节点，不再只砍边不删节点。
- **RED 时暂停语义生长**：`semantic_grow_from_vocab` 在 RED 时跳过，先腾内存再说话。

### Added
- **按需解冻机制**：对话管道 `dialog_process` 在语义理解后，只对当前输入的实体词和分词调用 `master_find_or_thaw` → `node_hash_find_or_thaw`，精准解冻当前话题涉及的冻节点，不盲扫无关领域（如科技对话不解冻医疗节点）。
- **内存安全阀**：`NodeCache.auto_thaw_ok` 由 `health_monitor` 每 120tick 同步更新，YELLOW/RED 时禁止全部解冻（含按需解冻），防止越救越糟。
- **`NodeHashTable` 注入管线**：新增 `cache` + `net` 引用，gateway 初始化时注入，`node_hash_find_or_thaw` 直接可用。
- **`master_find_or_thaw`** — 跨拓扑查找概念并自动解冻的便捷 API。

---

## v0.5.5 — 2026-07-18

### Added
- **网关启动保护**：启动前检测端口占用，若已被占用则拒绝启动，防止重复实例冲突
- **动态端口文件**：启动时将端口号写入 `/tmp/pivotmind.port`，脚本无需硬编码端口

### Changed
- QA 对统一走 `/learn` 通道，与语料一致走 PMI 共现拓扑，不做独立检索匹配
- 撤回 `/qa` REST 端点（含 `qa_memory_add`），简化架构

### Fixed
- 删除重名 NetworkManager 连接，BSSID 锁死 `7C:FD:FD:CA:B2:B0`，解决双频 WiFi 漫游断连问题

---

## v0.5.5 — 2026-07-13

### Added
- 语义约束管线：组合节点 `P(B|A) ≥ 0.5, N ≥ 10` + 自举分词
- 两跳激活扩散 `λ=1.0/0.4` + Jaccard → `node_act` 权重 0.35
- 语义休止 `node_act < 0.05`
- 语言感知扩散（同语言 `×1.3` / 跨语言 `×0.4`）
- 英文词间空格自动插入
- 搜索 → learn 管线打通 (`perception.c`)
- `semantic_growth.c` hook

### Changed
- 存盘周期改为 60 tick（~17 分钟）
- 跨语言边禁建，按 token 首字节分流（ASCII → 英文 PMI，CJK → 中文 PMI）
