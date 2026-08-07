# Changelog

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
