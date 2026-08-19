# v0.5.22 — 安全加固与崩溃根因修复（2026-08-18）

## 背景

08-17 GPT 外部审查 + 08-18 pro 独立复审（deepseek-v4-pro）确认 8 项问题，按"pro 分析 → flash 局部修改（一子代理一部分）"工作流逐项清除，并独立发现修复崩溃真根因。全部改动经 `make gateway` 零警告 + 压测验证。

## Security

### 网关全端点鉴权（demos/pivotmind_gateway.c）
- `X-Pivot-Token` 从仅 /media/* 扩展到**除 /health、/healthz 外所有端点**（/chat /learn /feedback /qa /debug /force_templates /train/* /status /scheduler /brain 等），与 /media 同一 header 约定、同一 401 语义、空 token 拒绝。
- 默认绑定 **127.0.0.1**（原 INADDR_ANY 局域网裸奔），`PIVOTMIND_BIND_ADDR=0.0.0.0` 环境变量可覆盖回全网卡；启动日志打印实际绑定地址。
- token 每次启动随机生成（/dev/urandom 32 字节 → 64 hex，打印日志）。⚠️ 调用方需从日志动态提取：/mnt/sdcard/work/gw_token.py（grep `token: ` 取最后一条）。

### qa_crawler 注入面清除（tools/qa_crawler.c，+105/-18）
- `system()` 三处调用全清零 → `run_exec()`（fork+execvp，argv 数组直传，子进程 stdio→/dev/null，父进程 waitpid 轮询 + 超时 SIGKILL）。
- URL 校验三级收紧：`url_scheme_ok()` 仅 http/https + 拒绝空白/控制字符；`url_matches_token()` 域名 host 相等/子域边界匹配（封堵 `strstr` 子串伪造 `evil.com/?x=白名单域`）；未知域**默认拒绝**（原默认放行）。
- 预期行为变化：文章抓取仅限白名单 12 站，RSS 摘要训练不受影响。

## Fixed

### SIGSEGV 真根因：对象池扩容越界（src/nn/memory_arena.c）
- `object_pool_acquire` 池空扩容分支：`free_count = new_capacity - total_capacity` 恒为 0，随后 `--free_count` 取 `free_list[-1]` —— 越界读返回垃圾指针。
- 触发条件：infer 建图边数 > 池容量（causal_graph_create 时 128）——"薛定谔的猫/量子纠缠"类因果查询（PFE 推理建大图）偶发 SIGSEGV。
- 崩溃栈（addr2line 符号化）：`handle_chat → pfe_solve_subgoal → infer_causal_graph_from_master_topology → add_causal_edge_no_check`，崩溃指令 `stp w1, w23, [x20]`（x20 = acquire 返回的坏指针）。
- **GPT 审查与 pro 复审均误判为"缓存悬垂"**（causal_reasoning.c:2467）。D 是真 bug 但非崩溃根因。教训：外部审查的"高度同源"推断须验证后再当定论。
- 修复：扩容后按实际分配数设置 `free_count`，malloc 失败正确中断；`total_capacity` 不再被无条件覆盖。
- 验证：6/6 因果查询 HTTP 200，MainPID 全程不变，NRestarts 0→0，0 新增 CRASH（历史 CRASH 60 条均为旧二进制）。

### causal_reasoning 缓存悬垂（src/causal_reasoning.c:2466-2474）
- `causal_associative_search` early-return 路径原代码 `causal_graph_destroy(graph)`（graph 即共享缓存 g_cg_cache，destroy 后未置 NULL 且锁外 destroy）→ 改为直接 return，缓存图统一由指纹变化分支销毁重建。
- 全文件 destroy 调用点审计：g_cg_cache 是唯一共享缓存，本 bug 类已清干净。

### UTF-8 标点比较（demos/pivotmind_gateway.c:950-951）
- 单字节 char 与 '。' 等多字节字符常量比较恒 false（-Wmultichar ×4 + -Wtype-limits ×4）→ `strncmp(p, "。", 3)` UTF-8 字节序列识别；汉字标点首次真正计入 punct_cnt 统计。

### corpus_train fread 缓冲（tools/corpus_train.c:248）
- fread 返回值未捕获 → 潜在 buffer 未终止，按实际读取数定 NUL 位置。

## Changed

### Makefile 并行度（Makefile:16）
- 删 `MAKEFLAGS += -j$(nproc)`（3.8GB 板全核编译 OOM 根因）→ `JOBS ?= 2` + `MAKEFLAGS += -j$(JOBS)`；命令行 `make -jN` 永远优先（GNU Make 4.3 实测）。

### _ar_find_pair 查找优化（src/article_reader.c，+45/-28）
- **澄清**：词对查找本就是开放定址哈希（DJB2+线性探测），O(n²) 真根因（三字扩展双层迭代 + b[8] 截断污染）已于 3878e84 修复。
- 剩余热点优化：PairEntry 加 `h` 字段固化键哈希；每槽比较 2×strcmp → 1 次 int 短路；rehash 免 snprintf 重哈希。新哈希与旧 `_ar_hash("a|b")` 逐字节同值（10 万随机对验证），探测序列/表布局/扩容清理语义零漂移。
- 收益：常规负载 ~5-10×，85-95% 退化区间 ~100×。

## Quality

- **test_tensor 13/13**：reshape 3×5→`{1,15}`（原 `{1,3}` 乘积≠size）；matmul 2×3·3×2 → size 4（原 6）；NULL 输入测试传参错误修复（原传合法矩阵）。
- **全仓 -Wall -Wextra 警告清零**（20 条）：autonomic_learner 未用变量（删 3 个 switch case）、diffusion 保留声明 `__attribute__((unused))`、node_cache/web_fetch/corpus_train/batch_learn 未用参数、semantic_growth int vs size_t 符号比较、test_model/test_trainer 断言清理、edge_builder 等。
- 网关鉴权/警告清理共触 10 文件，全部增量编译零警告零错误。

## 工作流备注（08-18 首次实战）

- **成本**：pro 复审 1.35 元 + 4 批 flash 修改（0.42/0.53/0.52/0.69 元）+ 压测验证，合计 ~3.5 元（空闲时段半价）。
- **分层**：pro 分析出方案（8 项核实 + 4 项独立发现）→ flash 一子代理一部分执行 → 主理人验证/拍板。
- **踩坑**：pro 复审根因判定翻车（误判缓存悬垂）——大范围审查可信，根因定性须实测验证。
