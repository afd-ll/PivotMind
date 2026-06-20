# 032 - 搜索管线语义升级

## 日期
2026-06-19

## 概述
联网搜索管线从"浅层词袋"升级为"完整语义理解管线"，打通 6 个系统性断链。

## 六大修复

### 1. HTTPS 修复（传输层）
- **问题**: Linux 上 `openssl_https_get` 是空壳实现，实际上调用的是 BSD HTTP GET（裸连 443 端口，服务器拒连），所有 HTTPS 请求静默失败
- **修复**: 补全完整 OpenSSL 实现：`SSL_CTX_new(TLS_client_method())` → `SSL_new` → `SSL_set_fd` → `SSL_connect` → `SSL_write/SSL_read` → `SSL_shutdown`
- **影响**: 百度百科、百度搜索、中文维基全部恢复可用

### 2. Content-Type 字段
- `WebResult` 新增 `content_type` 字段
- `do_fetch` (Linux/Windows) 均提取 Content-Type 响应头
- `web_result_free` 同步释放

### 3. HTML 解析增强
- 原 25 行手写 HTML 解析器替换为分层策略：
  - 策略 1: `<meta name="description">` — 百度百科/维基标准摘要
  - 策略 2: `<h1>/<h2>` 标题 + 前 3 段 `<p>` 正文
  - 策略 3: 回退到原始 tag 剥离
- 新增公共 API: `web_extract_meta_description()`, `web_extract_headings()`, `web_extract_paragraphs()`

### 4. 多源搜索 + Provider 熔断
- 搜索源优先级：百度百科 → 百度搜索 → 中文维基
- 每个 Provider 独立熔断：连续失败 3 次 → 冷却 5 分钟（300 ticks）
- 最多使用 2 个源，文本拼接去重

### 5. article_reader 语义管线
- `search_and_learn` 不再做浅层词袋学习
- 搜索结果文本 → `article_process_line()` → 累积 → `article_flush()`
- 每 `article_flush_interval` 篇触发一次，生成词汇节点 + 跨拓扑模板连接
- 与训练语料走同一套理解管线

### 6. 三维度知识缺口驱动搜索
- 替代原来的随机好奇心采样
- 维度 1 (对话缺口): 最近激活但低置信度的节点
- 维度 2 (模板缺口): 高频但低连接数/低置信度的节点
- 维度 3 (拓扑缺口): 连接数低于阈值的孤立词汇节点
- 权重可配置 (默认 0.5/0.3/0.2)

## 改动文件

| 文件 | 改动 |
|------|------|
| `include/web_search.h` | `WebResult` 加 `content_type`；新增 3 个提取函数声明 |
| `src/web_search.c` | 补全 OpenSSL HTTPS；增强 HTML 解析；Content-Type 提取 |
| `include/perception.h` | 加 `ArticleReader*`；搜索缓存；Provider 熔断字段；3D 缺口配置 |
| `src/perception.c` | article_reader 管线；多源搜索；LRU 缓存；3D 缺口检测 |

## 配置变更

`PerceptionConfig` 新增 4 个字段：
```c
int   cache_ttl_seconds;         // 86400 (24h)
float gap_weights[3];            // {0.5, 0.3, 0.2}
int   topo_gap_edge_threshold;   // 3
int   article_flush_interval;    // 3
```

默认 `max_searches_per_cycle` 从 8 降为 6（更保守，避免百度反爬）。

## 编译状态
✅ `src/web_search.c` — 零错误（仅 Windows `#pragma comment` 预存警告）
✅ `src/perception.c` — 零错误零警告
✅ `tools/test_web_search.c` — 零错误零警告
