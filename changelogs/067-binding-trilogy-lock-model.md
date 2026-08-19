# v0.5.23 — 绑定机制三件套与锁模型修复（2026-08-19）

## 背景

08-18 晚至 08-19 晨：绑定机制分析（pro）+ 实施（flash 子代理，一子代理一部分）+ 凌晨两次崩溃根因排查（R6 锁模型）+ 状态缩水事故调查（watchdog 双实例）。全部改动已部署，单测全绿（topology 3/3、tensor 13/13、learner 3/3、dialog 4/4）。

## 绑定机制三件套（涌现侧）

### top-K 缝合（候选组 + 综合打分）
- `topology_walk_greedy` 单链贪心（唯一 best_next_id）升级为 `topology_walk_greedy_topk`：每步候选排序取 top-K（默认 3，上限 8，`PM_WALK_TOPK_DEFAULT/MAX`）
- 综合分 = 边三维 + 候选自身激活值(×0.35) + 跨拓扑预加热投票 + 目标引力 + 路径回溯 + 语义分，再乘 valence/intent/heat 调制 + 三元组链奖励 + 模板锚点奖励 + POS 句式引导
- K=1 逐位等价（单测验证），其余 11 个调用方走旧入口零影响
- 意义：被跨拓扑预加热的候选（如"丈八蛇矛"）不再被垄断边（"武器→青龙偃月刀"）单链压死——绑定有了输出通道

### cross_hit 持久化（跨拓扑学习跨重启存活）
- 跨拓扑联合激活计数表（CrossTopoHitRecord，2048 槽开放寻址）序列化进状态文件：STATE_FORMAT_VERSION 7→8（尾部追加 count+记录+round 块）
- 旧格式（≤v7）加载表空=从零计数；v8 损坏 count 防御（整块丢弃）；其他块字节级不变
- 意义：跨拓扑自动建边（命中 ≥5 次，weight 0.5）不再因重启清零——"教它张飞→丈八蛇矛"能真正长成结构

### 种子词表（实体整词载体）
- 内置 16 个种子实体词（关羽/张飞/刘备/曹操/孙权/吕布/赵云/诸葛亮/青龙偃月刀/丈八蛇矛/赤兔马/方天画戟/桃园结义/饺子/武器/天气），`_ar_build_topo` 预注册整词节点
- 注册目标**强制 TOPO_VOCABULARY**（不受喂料领域拓扑影响，扩散第 0 步整词匹配可用）；幂等 + 与 PMI 词发现共存
- 开关：编译期 `PIVOTMIND_SEED_WORDS` / 运行期环境变量可关
- 意义：实体词不再被拆成字符碎片（"张飞"→张/飞），绑定机制有了节点载体

## 崩溃修复

### R6 锁模型（net->mutex 唯一权威锁）
- 根因（08-19 pro 分析，devlog/crash-0819-analysis.md）：`net->nodes` 两条互不排斥的 realloc 路径（`add_node` 持 net->mutex / `auto_extend` 持 master->rwlock）+ 读侧不持 net 锁 → 并发 double-realloc / 读野指针。凌晨全量重喂高并发下 SIGABRT（堆损坏）+ SIGSEGV（auto_learn_concepts）
- 方案 A 实施：写侧补 4 个缺锁点（auto_extend_topology_nolock / auto_shrink_topology / topology_load_balancing / remove_node_dynamic）+ 读侧补 3 个缺锁点（auto_learn_concepts 六处 nodes[] 解引用 / _ar_register_word_node / master_find_template_for_pair_nolock）
- 锁序恒 master→net（双锁按 topo_id 升序防 ABBA），无新增反向、无重入

## 其他修复

- **网关 token 持久化**：随机生成 + 记住有效 token（`gw_token` 文件，0600）——跨重启不变，黑匣子/feed 脚本永不失联
- **watchdog 防双实例**：`gateway_watchdog.sh` 加 flock（08-18 21:00 状态缩水事故元凶——双实例并发 restart，空模型写盘覆盖完整状态）
- **树莓派黑匣子探测**：适配 v0.5.22 网关绑 127.0.0.1（SSH 到板上本地 curl + 日志动态取 token）

## 质量

- 单测全绿：topology 3/3（含 top-K K=1 等价）、tensor 13/13、learner 3/3、dialog 4/4
- 全仓编译零警告

## 关键文档

- devlog/binding-check-20260818.md（绑定机制 3 检查点验证）
- devlog/crash-0819-analysis.md（凌晨 2 崩溃根因）
- devlog/r6-lock-fix-plan-20260819.md（R6 锁模型方案 A）
- devlog/shrink-investigation-20260819.md（状态缩水事故时间线）
- devlog/architecture-qna-20260818.md（跨拓扑信号/竞争/输出/共激活 4 问）
