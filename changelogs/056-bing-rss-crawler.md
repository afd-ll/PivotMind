# 0.4.15 — QA 爬虫 v2: Bing RSS 搜索 + 文章抓取训练

> **日期**: 2026-07-10 | **类型**: 新增

## 概述

QA 爬虫从搜狗微信搜索（JS 渲染无法抓取）切换到 Bing RSS 搜索（XML 格式，5KB/请求，无需 JS），并增加文章全文抓取 + article_reader PMI 词发现 + 文本语料保存功能。

## 核心变更

### 搜索端点

- **旧**: `https://weixin.sogou.com/weixin?type=2&query=...`（JS 渲染，curl 抓不到结果）
- **新**: `https://cn.bing.com/search?q=...&format=rss&count=10`（XML，含标题/URL/摘要）

### 文章抓取

- `--fetch-articles`: 从搜索结果 URL 抓取文章全文
- HTML→纯文本提取（去除 script/style/embed，HTML 实体解码）
- `system(curl)` 替代 `fork/exec curl`，避免子进程 hang
- 域名白名单过滤（跳过 知乎 question 页、B 站等 JS 渲染站）

### 训练管线

- 文章文本通过 `article_reader` PMI 词发现管线创建多字词节点
- 文本保存到 `/tmp/pm_corpus/` 供后续工具使用

### 搜索词

33 个覆盖多领域的搜索词：AI/编程/数据结构/网络/OS/数学/物理/化学/历史/地理/经济/心理/哲学/社会/健康/健身/睡眠。

## 运行结果

- 33 查询 × 1 轮 = 52 篇文章，726K 字符，4,733 个 PMI 词节点
- 知乎返回 116 字（反爬页），CSDN/菜鸟教程/博客园效果最好（5K-65K 字/篇）

## 改动文件

| 文件 | 变更 |
|------|------|
| `tools/qa_crawler.c` | 重写：Bing RSS 搜索 + 文章抓取 + 文本保存 |
| `Makefile` | 添加 `--fetch-articles` 编译支持 |
