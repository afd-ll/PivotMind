# v0.5.21 — 安全加固 + 并发根治 + 生成端修复（2026-08-15）

> 三路子代理（架构审核/漏洞扫描/生成端优化）+ 两路审核（优先级/修法）+ 执行子代理闭环。
> 分支 `fix/security-20260815` → main merge `db2c3a5`。

## Security（C1 远程 RCE 封堵）

- **execvp 化 7 个调用点**：`media_reader.c`（133/172/219/419）+ `visual_cortex.c`（187/189/279）的 `popen/system` 全部改 `fork+execvp`（参数数组传值，去 shell 解释层）——注入面消失（`a";touch /tmp/pwned;"` 实测无文件生成）。
- **`/media/*` token 鉴权**：启动时 `/dev/urandom` 生成 64 位 hex token 打印日志；`/media/feed`、`/media/status` 必须带 `X-Pivot-Token` 请求头，无 token 401。只锁媒体端点（RCE 入口），`/learn` `/chat` 内部端点保持兼容（喂料脚本不打断）。
- **realpath + S_ISREG + 媒体目录白名单**（默认 `/mnt/sdcard/media:/mnt/sdcard`，`PIVOTMIND_MEDIA_DIRS` 可覆盖）：灭 SSRF（ffmpeg 读任意本地文件）。
- **`"> NUL"` Windows-ism 修复**：`media_reader.c:171` 改 `/dev/null`（Linux 上原来生成字面 NUL 文件）。
- **B1 边界**：H1——状态加载校验 `from_node` 范围（防损坏文件负索引 OOB / GB 级 realloc OOM）；M4——去重分支 `return -1` → `return result`（疑似边暴涨推手）。

## Concurrency（B3/B4 锁改造——STUCK 根治）

- **B3 锁外快照写盘**：`master_save_state` 持锁写 330MB → 锁内（master 读→net 读→node_locks 三把同时拿）全量深拷贝 `SaveSnapshot`，锁外序列化写盘。**字节级等价**为验收门槛（新旧文件 cmp==0，加载端零改动）。回退：`PIVOTMIND_SAVE=locked` 环境变量走旧路径。
- **B4 learn 队列化**：`handle_chat` 同步学习改入队（输入学习带 flush 保当轮新词可用，回复学习 fire-and-forget）；worker 2→1，`_learn_tokens` 变单消费者（函数体零改动）。回退：`LEARN_WORKER_COUNT`/`LEARN_ASYNC_CHAT` 宏。
- **R6 标注**：`net->nodes` 双路径并发 realloc 隐患（add_node 拿 net->mutex / auto_extend 拿 master->rwlock）加交叉指认注释，快照侧"两把锁都拿"规避读侧；统一锁模型留后续排期。
- **效果**：13:43 换新二进制后 watchdog 零 STUCK（旧代码每 1.5-2h 必杀一次，当天已 8 次）。

## Generation（生成端修复）

- **B2 broca 越界**：`pos_tags[64]` 栈越界读（word_count>64）→ 动态分配；空格插入/NUL 无边界检查 → `bwr_reserve` 扩容 helper。
- **C1 side_inhibit 字节 bug**：`strncmp(...,2)` 比 2 字节（CJK 3 字节，"一/丁"共享前 2 字节误判）→ UTF-8 完整字符比较。
- **C2 词锚重叠去重**：词锚插入点三处（diffusion.c :960/:980/:1011）加字符重叠检查——修"方鸿+鸿渐"式裂词。
- **C3 MAX_REPLY_WORDS 常量化**：主路径与降级路径共用常量，便于 A/B。
- **实测**："围城方鸿鸿渐" → "围城方鸿"（叠字消失）。

## Experiment（4→6 词 A/B）

- 结论：6 词只放大词层拼接垃圾（"安市人属于级别行政区域设置挑战机构组成国家学习"），质量不升——**信息量瓶颈在选词/组句不在长度限制**，回退 4。生成端微调到此收手，等 dist_sig 数据上阶段 2.5/3。

## 验证

- 编译：`make gateway` 零新增警告（systemd-run 隔离）。
- 安全：401/放行/注入无文件 实测全过。
- 并发：快照路径行号确认 + 存盘期间 status 响应 + watchdog 0 STUCK（观察哨 30min cron 持续盯）。
- 字节等价：逐字段静态核对（文件头→节点→哨兵→cross_links→freq）。
