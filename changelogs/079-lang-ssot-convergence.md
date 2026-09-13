# 079 — 语种判定收敛为单一真值源（多语种分离·第 1 步）

- **版本**：v0.5.35（v0.5.34 → v0.5.35）
- **日期**：2026-09-13
- **工作区**：`/home/cx/pm-fix`（Pi 3B），分支 `feat/paths-callsite-migration`；`main` 未动
- **来源**：老大「**接下来就是做分开语种了**」→ 追问方向后裁决「**先分离多语种，专项专做再说**」
- **前置**：[078 — 资产路径并入 SSOT + 制表框按显示宽度对齐（含超宽截断）+ 加载计数纠偏](078-asset-ssot-frame-width-load-count.md)
- **提交**：`f861729`（SSOT 主体 + 9 处调用点收敛）/ `6cb34df`（契约单测 + Makefile 接入 + 版本锚点）/ `ac5f3b9`（`-Wcomment` 新警告清零）

---

## 零、缘起：动工前先盘点，发现「语种」在本仓有九个答案

老大拍板下一程走「多语种分离」。分离的第一步本想直接动数据结构，
但按惯例先扫了一遍全仓 —— **结果不支持直接动手**：

> 语种判定散落在 **9 处**，口径分成 **三档**，互不相同，且**各有错判**。

同一个词，在不同子系统里会被判成不同语种。**在这样的地基上做「分离」，
分离出来的边界本身就是错的。** 所以第 1 步只做一件事：

> **让「语种是什么」在全仓只有一个答案。**

这也是本版**刻意不碰**节点数据结构、跨语种边、状态格式的原因 —— 那些属于
「专项」（见第七节 D1/D2/D3），要在真值源立住之后再谈。

---

## 一、三档口径，各自的错判

| 档 | 判据 | 出现处 | 错判 |
|----|------|--------|------|
| ① 宽档 | `(unsigned char)c & 0x80` | `utf8_tokenizer.c` 的 `is_chinese()`、`diffusion.c` 的 `NODE_IS_CJK`、`gateway_handlers.c` 的语种兜底 | **任何非 ASCII 都算「中文」**：法文 é、德文 ü、日文假名、韩文谚文、emoji、CJK 标点全中招 |
| ② 中档 | `strlen(s)==3`（+ 首字节 E0-EF） | `autonomic_learner.c` 的 `cc_is_cjk_char()`、`compound_promote.c` 的 `is_cjk_char()` | **「3 字节就算汉字」**：CJK 标点 `。`(E3 80 82)、平假名 `あ`(E3 81 82)、谚文，全是 3 字节，全被误当汉字 |
| ③ 严档 | 3 字节且码点 ∈ 0x4E00..0x9FFF | `chinese.c` 的 `is_chinese_char()` | 只认基本区，**丢掉扩展 A**（3400–4DBF） |

另有 3 处是**反方向**的 ASCII 判定（`cognitive_controller.c` 的 `is_ascii_word()`、
`feed_cli.c` 的 `is_ascii_token()`、`article_reader.c` 的内联 `text[0] < 0x80`），
与上面三档共同构成「同一维度、九种写法」的乱局。

---

## 二、收敛：`include/lang.h` + `src/lang.c`（唯一定义源）

新增一份 **语种 SSOT**，口径统一为 **码点级**（先解码、再按 Unicode 区块归属），
设计思路与显示宽度 SSOT（`src/ui.c` 的 `ui_disp_width`）同源：**一个维度一个真值源**。

```c
typedef enum {
    PM_LANG_UNKNOWN = 0,   /* 空串/纯数字/纯标点/纯符号 */
    PM_LANG_ZH      = 1,   /* CJK 统一表意：基本区 4E00-9FFF + 扩展 A 3400-4DBF */
    PM_LANG_EN      = 2,   /* 拉丁：基本 + 扩展 A/B（é ü ñ ç…） */
    PM_LANG_JA      = 3,   /* 平假名/片假名 3040-30FF（+ 半角片假名 FF66-FF9D） */
    PM_LANG_KO      = 4,   /* 谚文音节 AC00-D7AF + 字母 1100-11FF */
    PM_LANG_OTHER   = 5    /* 希腊/西里尔/希伯来/阿拉伯/天城文/emoji */
} PmLang;
#define PM_LANG_COUNT 6
```

对外 API（**新代码禁止再写裸判据**）：

| API | 语义 | 承接的旧档位 |
|-----|------|-------------|
| `pm_utf8_decode(s, &cp)` | **全仓唯一**的 UTF-8 码点解码（返回字节数，非法/截断⇒1 且 cp=0） | 取代散落的移位拼码点 |
| `pm_lang_of_cp(cp)` | 码点 → 语种 | —— |
| `pm_lang_of(s)` | **首码点**判语种（旧「看首字节」的正确版本） | ① / ② |
| `pm_lang_of_text(s)` | 整串**主导语种**（按码点投票，平票取先出现） | 新增能力 |
| `pm_zh_ratio_permille(s)` | 中文码点占**非 ASCII 码点**的千分比 | 新增能力 |
| `pm_has_zh(s)` | 是否**存在**中文码点（≠ 主导语种） | 新增能力 |
| `pm_is_nonascii(s)` | 首字节带高位（`& 0x80`）——**字节级粗判，不含语种语义** | ①（保留语义、显式命名） |
| `pm_is_zh_char(s)` | 首码点落在 CJK 表意区（**排除 CJK 标点/假名/谚文**） | ② / ③ |
| `pm_is_ascii_text(s)` | 全部字节 < 0x80（空串 ⇒ 0） | `is_ascii_word` / `is_ascii_token` |
| `pm_lang_name(lang)` | 稳定短名：`zh/en/ja/ko/other/unknown`（对外协议，勿用枚举数值） | 新增能力 |

关键设计点：**保留旧函数名作薄转发**，调用点零改动即可切到新口径
（例如 `utf8_tokenizer.c` 的 `is_chinese()` 内部改为 `return pm_is_nonascii(p);`）——
这样「口径收敛」与「调用点重构」解耦，本版只做前者。

---

## 三、9 处调用点全部改走 SSOT（10 文件）

| 文件 | 旧实现 | 新实现 |
|------|--------|--------|
| `src/utf8_tokenizer.c` | `is_chinese()`：`& 0x80` | `pm_is_nonascii()` |
| `src/autonomic_learner.c` | `cc_is_cjk_char()`：`strlen==3` | `pm_is_zh_char()` |
| `src/cognitive_controller.c` | `is_ascii_word()` | `pm_is_ascii_text()` |
| `tools/feed_cli.c` | `is_ascii_token()` | `pm_is_ascii_text()` |
| `tools/compound_promote.c` | `is_cjk_char()`：`strlen==3` | `pm_is_zh_char()` |
| `src/article_reader.c` | 内联 `text[0] < 0x80` | `pm_is_ascii_text()` |
| `src/diffusion.c` | `NODE_IS_CJK` 宏 + `lang_dom` 统计 | `pm_lang_of(...) == PM_LANG_ZH` + `switch(pm_lang_of)` |
| `demos/gateway_handlers.c` | 语种一致性兜底 | `pm_has_zh(msg) && !pm_has_zh(response)` |
| `src/chinese.c` | `is_chinese_char()`（**无调用方的死代码**，一并归正） | `pm_is_zh_char()` |

`include/lang.h` / `src/lang.c` 为新增；`Makefile` 的 `CORE_SRC = $(wildcard src/*.c)`
会自动纳入 `src/lang.c`，**无需改 Makefile 即可参与编译**（测试注册另行处理，见第四节）。

---

## 四、契约单测：把 SSOT 的边界锁死（第 28 支）

新增 `tests/unit/test_lang_unit.c`，**9 组 / 13 用例**：

- **解码**：1/2/3/4 字节序列；非法/截断/空串/NULL 不崩溃、码点归零。
- **判定边界**：CJK 基本区两端 + 扩展 A 两端（`4E00`/`9FFF`/`3400`/`4DBF`），
  以及 `4DFF`/`A000` 必须**不是**中文；日/韩/英/其它；数字/标点/空白 ⇒ UNKNOWN。
- **★ 回归锁（本版的重点Case）**：CJK 标点「**。**」(U+3002) 与平假名「**あ**」(U+3042)
  都是 **3 字节** —— 旧 `strlen(s)==3` 口径把它们当汉字。这两条一旦变红，
  说明「字节数 = 语种」的错误口径又回来了。
- 主导语种投票、千分比/存在性、三个谓词、6 个名称标签逐字断言且互不相同。

**Makefile 接入 6 处**（全部为追加，零删除）：`ASAN_TEST_BINS` / 编译规则 /
别名 `test-lang-unit`（第 28 支）/ `TEST_BINS` / `test:` 前置依赖 / `.PHONY`。

---

## 五、新警告清零

首次三机编译时 `lang.h` 触发一条**新警告**：

```
include/lang.h:28:46: warning: '/*' within comment [-Wcomment]
```

原因是头注释里写了字面量路径 `src/*.c`（其中的 `/*` 落在块注释内部）。
改为不提该字面量（「CORE_SRC 用 `src/` 通配全部 `.c`」）后 **`-Wcomment` 归零**。
（`include/lang.h` 被 10+ 个 TU 包含，这条警告会在每个 TU 各响一次 —— 必须清。）

---

## 六、可见产出：`state_dump` 新增「语种分布」节

`tools/state_dump.c` 增加一节，遍历全部子拓扑的概念节点，**按 `pm_lang_of` 统计分布**：

```
--- 语种分布 (按概念节点, 口径=pm_lang_of) ---
  unknown       112  (  2.9%)
  zh           3116  ( 79.4%)
  en            694  ( 17.7%)
  ja              0  (  0.0%)
  ko              0  (  0.0%)
  other           0  (  0.0%)
  合计       3922
```

**这是本版唯一的"看得见"的产出** —— 做「分离」之前，先看清各语种到底占多少。
上表是**只读复制**线上 state（5.3 MB，源文件 md5 前后均为 `31377ef0…`、
**零写入**）后用 v0.5.35 的 `state_dump` 读出来的**真实系统分布**：

- 总节点 **3923**、总连接 **3635**、子拓扑 **12**。
- 分布 **合计 3922**，比总节点少 1 —— 有 1 个节点 `concept` 为空串被跳过（口径如此，非 bug）。
- **`en 694` 不是「中文被错判成英文」**：汉字区 `4E00–9FFF` 是精确范围，中文不会落到 EN。
  这 694 个几乎全是**结构性元节点的 ASCII 命名** —— 领域拓扑（930）、语法拓扑（15）、
  语用拓扑（5）、文化拓扑（4）、上下文拓扑（4）、模板拓扑（8）合计约 966 个元节点里，
  相当一部分用 `TECH` / `CN_CULTURE` / `ASK` / `SVOC` 这类英文类目码命名；
  词汇/语义/概念三层（1805+455+697）则中文占绝对多数。**线上语料实质是单语中文。**

沙箱 A/B（10 词 jieba 词典 + 双语 QA）：681 节点 / `zh 360` `en 281` `unknown 40` —— 口径在两处一致工作。

---

## 七、诚实边界（留给「专项」轮的待决策项）

本版**只做真值源**，以下**均未动**，属「专项专做」范畴：

- **D1 英文语料来源**：现有语料 100% 中文（含 `/home/cx/corpus-rescue/本地书库/` 578M）。
  没有英文语料 ⇒ 现在只能做**「纯化」**（把误判剔掉），做不了**「双语能力」**。
- **D2 跨语种语义层**：仓里对「跨语种」本身就有**三个互相矛盾的答案**：
  ① 旧 `LANG_BOOST_CROSS 0.4f`（`diffusion.c`：跨语种=弱但允许）；
  ② v0.6 补丁 `lang_dom` + ~5 处硬过滤 + 网关语种兜底（`diffusion.c` / `gateway_handlers.c`：输出层**阻断**）；
  ③ 注释目标「禁止跨语种边」（`feed_cli.c` / `CHANGELOG.md`：建边层**禁止**）。
  三者语义不同、层次不同。**先不动**，等专项轮统一定调。
- **D3 状态格式 9 → 10**：`ReasoningNode` **没有 `lang` 字段**；12 个 `TopologyType`
  全是功能维度、无语言维度；中文词与英文词混在同一张词汇拓扑里。
  要真正「分语种」，迟早要给节点打 `lang` 标或分表 —— 那是一次**状态格式升级**，
  必须单独一版、带迁移脚本，**不在本版**。
- **口径细化**：`语种分布` 目前统计**所有**子拓扑的节点，因此结构性元节点（英文类目码）
  会计入 `en`。若要把该指标用于「语种分离」的量化依据，应细化为**只统计词汇/概念层**。
- **`pm_is_nonascii` 是字节级粗判**、**不含语种语义**，保留它是为承接旧档位语义；
  能用 `pm_lang_of` 的地方不要用它。
- `src/ui.c` 仍有一份独立的 `static ui_utf8_decode()`（显示宽度用），**本版未合并**；
  `lang.h` 注释已标注「待后续复用本函数」。

---

## 八、Verified

**三机（Pi 按现行铁律不编译）**：

| 机器 | 架构 / 编译器 | `make clean && make all` | check-tools | check-version | `make test` |
|------|--------------|--------------------------|-------------|---------------|-------------|
| WSL | x86_64 / gcc 15.2.0 | **0 error / 0 warning** | ✓ 19/19 | PASS | **28 通过 / 0 失败**（27→28） |
| armbian-1 | aarch64 / gcc 13.3.0 | **0 error / 0 warning** | ✓ 19/19 | PASS | **28 通过 / 0 失败** |
| Pi 3B | ——（只编辑/提交/打包） | —— | —— | —— | —— |

- 新单测 `test_lang_unit` 在两机**直跑均 RC=0**（13 用例全过）；`make test` 汇总 **28 / 0**。
- `-Wcomment` 与 `warning:` 在两机均 **0 行**。
- **线上实例与线上数据全程未动**：armbian `pid 2215289` 存活、仅监听 `127.0.0.1:8080`；
  线上 state 仅**只读复制**一份做分布测量，源文件 md5 前后一致。
- 分支 `feat/paths-callsite-migration` 三处（Pi 工作区 / `origin` / 私有 forge）一致，
  tip = `ac5f3b9`；bundle `pm-v0535b.bundle`（14014422 B）经字节校验 + md5 校验一致。
