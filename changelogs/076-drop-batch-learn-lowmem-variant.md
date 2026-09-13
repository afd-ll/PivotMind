# 076 — 删除 `batch_learn_lowmem` 变体 + 清除死宏 `CROSS_REBUILD_INTERVAL`

- **版本**：v0.5.32（v0.5.31 → v0.5.32）
- **日期**：2026-09-13
- **工作区**：`/home/cx/pm-fix`（Pi 3B），分支 `feat/paths-callsite-migration`
- **来源**：老大决策 —— v0.5.31 体检提出的 `[待决策]` 二选一，老大选 **① 删掉该变体**
- **前置**：[075 — 工具层并入构建 + 三编译器告警清零](075-tools-build-integration-warning-sweep.md)

## 决策

v0.5.31 体检发现：`tools/batch_learn.c` 里的 `CROSS_REBUILD_INTERVAL` 是**死宏** —— 全文件除自身
两行 `#define` 外**零引用**。真实重建发生在**每个 epoch 结束**与**训练收尾**两处，所谓「每 5000 条 QA
重建一次」的机制早已被重构掉。后果：`batch_learn_lowmem` 与 `batch_learn` 产出的二进制**逐字节相同**
（md5 已实测），「低内存版」当时**无任何实际差异**。两条路线：

| 选项 | 含义 | 代价 |
|---|---|---|
| ① **删除该变体**（本次采纳） | 承认 lowmem 从未成立，清掉假选项 | 失去「低内存」名义入口 |
| ② 把 `LOW_MEM` 接回重建判定 | 真做一个低内存行为 | 涉及训练内存语义，需重新设计与实测 |

老大选 **①**。理由：一个**行为等价**的二进制变体是纯粹的认知负担 —— 它让人以为存在「低内存方案」，
而实测并无差异。**留着一个假开关，比没有更危险。**

## 改动清单

**源码**
- **删除** `tools/batch_learn_lowmem.c` —— 13 行薄壳翻译单元（`#define LOW_MEM` + `#include "batch_learn.c"`）。
- `tools/batch_learn.c` —— 删除 `CROSS_REBUILD_INTERVAL` 的**整段声明**：注释 10 行 + `#ifndef/#define/#else/#define/#endif` 5 行 + 尾随空行，共 **16 行**。该宏既无引用、其唯一假想使用场景（lowmem 变体）也已删除，**连同注释一并清除**，不留误导性历史说明。

**构建（`Makefile` 四处）**
1. 删除 `batch_learn_lowmem` 的注释块 + 二进制规则（原 L144–147，含尾随空行共 5 行）；
2. 从 `TOOL_BINS` 清单移除该条目（**20 → 19**）；
3. 删除 phony 别名 `batch-learn-lowmem:`；
4. 从 `.PHONY` 列表移除 `batch-learn-lowmem`。

**无需改动的地方（说明「为什么不用改」）**
- `TOOL_SRC = $(wildcard tools/*.c demos/*.c)` —— 源文件删除后**自动**不再纳入编译，无需手改源清单。
- `check-tools:` 用 `$(words $(TOOL_BINS))` **动态计数**，无硬编码「20」，自动变 19。
- CI 的 `build-x86_64` / `build-arm` 两处 `make check-tools` 步骤**不用改**。

## 验证

- **三台机器、三个编译器** `make clean && make all`：**0 error / 0 warning**。
- `make check-tools`：✓ 全部 **19** 个工具已产出。
- `batch_learn` 二进制仍在、行为未变；`batch_learn_lowmem` 已不再产生。
- 全仓 `lowmem` 残留扫描：**代码层与构建层零残留**，仅历史文档（`CHANGELOG.md` / `changelogs/075` / `docs/Makefile_decoded.txt` 旧快照）保留记录。

## 诚实边界

- 本次**只删除、不提供替代**。若将来真要支持 Zero 2W（416MB）级设备，必须在 `batch_learn.c` 内**重新设计**重建策略 —— 真实重建点只有 epoch 末与训练收尾两处 —— 而**不是**重新引入这个空壳变体。
- `docs/Makefile_decoded.txt` 里仍留有旧规则快照（`-DLOW_MEM` 写在**链接行**上那段），那是**历史解码产物**，刻意不改；它反而准确记录了 v0.5.31 之前那个「看起来有低内存版、实际不生效」的状态。
- 删除一个变体**不会**降低任何现有功能的内存占用。「低内存」从来不是一个已实现的特性。
