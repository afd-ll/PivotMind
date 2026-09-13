# 078 — 资产路径并入 SSOT + 制表框按显示宽度对齐（含超宽截断）+ 加载计数纠偏

- **版本**：v0.5.34（v0.5.33 → v0.5.34）
- **日期**：2026-09-13
- **工作区**：`/home/cx/pm-fix`（Pi 3B），分支 `feat/paths-callsite-migration`；`main` 未动
- **来源**：老大「**可以，都一起抓了做了吧**」—— 把 v0.5.33 收尾时 CLI/网关端到端验证挖出的 5 处缺陷一次性收口
- **前置**：[077 — 旧扁平布局 fail-loud 门 + 长跑脚本隔离洞 + `snprintf` 字面量尺寸体检](077-legacy-layout-gate-and-longrun-isolation.md)

---

## 零、缘起：一句「你验证了 CLI 包装了吗？」带出的五处缺陷

v0.5.33 发完之后问了一句「你验证了 CLI 包装了吗？也就是 pivotmind gateway 这些指令」。
于是补了一轮**端到端**验证：12 项端点 / 鉴权 / 退出码检查**全过**，
但顺手挖出 5 处与「能不能用」直接相关的问题：

| # | 级别 | 问题 | 本版处理 |
|---|------|------|---------|
| P1 | 高 | 8 处资产仍用【相对 CWD 的裸路径】 | 并入路径 SSOT（新增资产表 + `pm_asset()`） |
| P1-B | 高 | 网关 `--train-mode` 与资产不是同一个根 | 统一走 `pm_asset()` |
| P2 | 中 | `quick_chat` 缺状态文件时静默以空脑启动 | 显式提示 + 退出码语义对齐 |
| P3 | 中 | 制表框按字节数补空格 ⇒ CJK 右边框全错位 | 立显示宽度感知的框 API，17 处站点迁移 |
| P3 | 中 | `state_dump` 计数自相矛盾（顶部 N 节点 vs 详情 0） | 注册 12 个子拓扑 + 丢弃/加载分开计数 |

---

## 一、P1：资产相对路径没并入路径 SSOT（8 处调用点）

**现象**：路径 SSOT（v0.5.30）收编的是 **12 个状态文件**，资产不在其中。
于是这几个字面量散落在调用点：

```
data/jieba_dict.txt            data/hermes_knowledge_base.json
data/knowledge_base.json       corpus/xiaohuangji_pipe.txt
```

它们是**相对 CWD 的裸路径** ⇒ 从任意目录启动工具/网关，资产全部找不到，
而且找到失败后**静默降级**（词典没了就退化成逐字分词，语料没了就报一句"无法打开"）。

**修法**：在 `pivotmind_paths` 里增设**资产表** —— 刻意与 12 个状态文件**分开**，
避免污染 v0.5.33 刚落地的「旧扁平布局审计」（审计的对象是状态文件，不是资产）：

```c
/* include/pivotmind_paths.h */
#define PM_ASSET_JIEBA_DICT   (1u << 0)   /* data/jieba_dict.txt              */
#define PM_ASSET_QA_CORPUS    (1u << 1)   /* data/hermes_knowledge_base.json  */
#define PM_ASSET_KB           (1u << 2)   /* data/knowledge_base.json         */
#define PM_ASSET_XIAOHUANGJI  (1u << 3)   /* corpus/xiaohuangji_pipe.txt      */
#define PM_ASSET_COUNT        4
const char *pm_asset(unsigned which);     /* 非法 which ⇒ NULL */
```

`src/pivotmind_paths.c` 侧是一张 `g_asset_rel[4]` + `g_assets[4][PM_PATH_MAX]`，
在 `pm_home()` 解析完成后按同一套 `pm_join` 规则落位。

**调用点全部改走 `pm_asset()`**：`quick_chat` / `feed_cli` / `batch_learn` /
`hebbian_pretrain` / `corpus_train` / `seed_builder` / `demos/gateway_system` /
`demos/pivotmind_gateway`。

`tools/corpus_train.c` 另有一处更隐蔽的：

```c
#define CORPUS_DIR "~/本地书库"      /* ← C 里 ~ 从不展开，这是永远找不到的路径 */
```
改为 `pm_dir(PM_DIR_CORPUS)`，`QA_PATH` 改为 `pm_asset(PM_ASSET_QA_CORPUS)`。

---

## 二、P1-B：网关 `--train-mode` 的双根

`demos/pivotmind_gateway.c` 的 `--train-mode` 默认语料写死
`"data/hermes_knowledge_base.json"` —— 与 P1 的资产根**不是同一个根**：
一个走 SSOT、一个走 CWD。统一为 `pm_asset(PM_ASSET_QA_CORPUS)`。

---

## 三、P2：`quick_chat` 的「缺文件」与「文件损坏」语义

**现象**：状态文件**缺失**时静默以空脑启动、**RC=0**；文件**损坏**时 RC=1。
同一类失败两种语义，且缺文件时**一句话都不说** —— 看起来就像「升级后失忆」。

**修法**（两处）：
1. 缺文件时打印 `提示: 状态文件不存在（…）—— 将以【空脑】启动（首次运行属正常）`；
2. 词典缺失也在 stderr 明确告警（`[quick_chat] ⚠ 词典不存在: …（逐字模式，分词质量会下降）`），
   不再 `fopen` 失败后静默跳过。

**但第一版只修了一半**：源码注释写着「缺 ⇒ 正常路径（RC=0）」，
可紧跟着的 `if (loaded <= 10) { printf("× 状态加载异常"); return 1; }`
把「文件不在」和「文件在却几乎没加载出东西」判成了同一件事 ——
先提示「属正常」，再按异常退出。这个问题是 **WSL 终验时用退出码实测**才暴露的
（见第五节的 `① 文件缺失 RC=0`），第一/二批补丁里都没发现。

最终三态语义：

| 状态 | 提示 | RC |
|---|---|---|
| 文件**不在** | 「将以【空脑】启动（首次运行属正常）」+「不按加载异常处理」 | **0** |
| 文件在但加载 ≤10 节点 | 「× 状态加载异常」 | 1 |
| 文件**损坏** | `multi_topology` fail-loud（fmt_ver 非法/过高，附诊断） | 1 |

---

## 四、P3：制表框按【字节数】补空格 ⇒ CJK 右边框全错位

**现象**：全仓 14 个文件里的框，都是用 `strlen`（**字节数**）补空格。
CJK 占 2 个显示列却只算 1 ⇒ **右边框一律错位**。
实测：42 条框内容行里 **31 条错位**。而且标题里带版本号（`v%s`）⇒ 手改空格**必然复发**。

**修法**：在 `ui.c` / `ui.h` 立一套**显示宽度感知**的框 API（不再逐点手改空格）：

| API | 职责 |
|---|---|
| `int ui_disp_width(const char*)` | UTF-8 字符串的显示列宽：EAW `W`/`F` = 2 列，组合符/零宽 = 0 列，其余 = 1 列（与 `wcwidth` 的 EAW 口径一致；框线 `U+2500-257F` 等 Ambiguous 按 1 列） |
| `FILE* ui_frame_stream(FILE*)` | 切换框输出流并返回**之前的流**，便于成对恢复（给 `fprintf(stderr,…)` 的报告用） |
| `ui_frame_begin/sep/sep_label/row/end(int/label/fmt,…)` | 开框 / 分隔 / 带居中标签的分隔 / 一行 / 闭框 |
| `ui_frame_title(int inner_w, const char* fmt, ...)` | 一行式标题框；`inner_w <= 0` ⇒ 自动（内容宽 + 4，下限 44） |

**17 处站点全部迁移**（覆盖 14 个文件），包括两处块内含 `for`/`if` 的复合框：
`template_builder.c` 的 POS 槽位化诊断报告（走 stderr，用 `ui_frame_stream(stderr)` 成对切换）
与 `batch_learn.c` 的完成汇总框。
顺带修掉 `ui_box_start()` 的同类缺陷（横线数也按字节数算 ⇒ CJK 标题的横线少一截）。

### 4.1 超宽：右边框被推走 —— 治根，而不是只治这批数据

WSL 上用一个**显示宽度检查器**（把连续的、含 `╔╠╚║` 的行聚成"框块"，
要求块内每行的显示列宽相同）跑全工具启动框矩阵，抓到两处**真实超宽**：

```
✗ compound_promote  49 vs 45   ← 标题里 阈值=3.0 上限=500 两位以上数就撑破
✗ batch_learn       47 vs 45   ← 「权重饱和: 0% | 孤立节点: 69% | 平均度: 6.6」三个数同占一行
```

注意这**不是排版错，是内容按显示宽度算确实超过了设计宽度**。
而原实现的「超宽不截断、留一格再收边」正好把右边框推走 —— 那正是本版要治的病。

改为：超宽时按【显示列】在**码点边界**截断，末位打 `U+2026 …`。
两个都不选：**不**整行溢出（推走边框），**也不**静默丢数据（省略号就是「这里被切了」的凭证）。
配套把 UTF-8 解码抽成 `static ui_utf8_decode()`，`ui_disp_width` 复用它（解码口径收成一处）。

另外两处内容层面的修（不是 API 的问题）：
- `compound_promote` 标题框改**自动宽度** —— 标题本来就该随内容定宽；
- `batch_learn` 完成框的「权重饱和」行**拆成两行**（三个百分数同占一行，43 列必然不够）。

### 4.2 手工框清零

迁移后，**生产代码里手写制表框清零**。残留 6 个只在 `tests/` 下（测试脚手架，另行处理）。

---

## 五、P3：`state_dump` 的加载计数

**现象**：`tools/state_dump.c` 只调 `master_topology_create(0)` —— **子拓扑 0 个**。
而加载循环（`multi_topology.c`）是按 `topo_type` 找目标拓扑注册项的：

```c
if (!target_topo) { loaded_nodes++; ... }   /* ← 找不到 ⇒ 节点被【丢弃】，却计成【已加载】 */
```

于是每个节点都落进这个分支：**节点全被丢弃，计数却全是"成功"**。
报告就自相矛盾了：顶部打印 `N 节点`，详情却是 `总节点数: 0 / 子拓扑数: 0`。

**修法两条**：
1. `state_dump` 按**标准全集**注册 12 个子拓扑（`TOPO_VOCABULARY`..`TOPO_VISUAL`，0..11），
   注册失败**硬失败**（不再带着空注册表往下跑）；
2. `multi_topology` 的加载循环把「丢弃」与「加载」**分开计数**：
   `skipped_unknown_topo` + 按类型分桶 `skipped_by_type[256]`，收尾打 WARN 明细。

**这条修完立刻在别的工具上抓到真问题**：`build_cross_links` 注册的拓扑不全，
加载时真的丢了节点，现在会明确说出来：

```
[WARN] [状态持久化] 加载期丢弃 1 条节点记录：其 topo_type 在目标 master 中【未注册】
       （类型:计次 = 3:1）—— 这些节点【未进入大脑】，故不计入已加载数。
       请核对调用方的 master_add_sub_topology() 清单是否覆盖该文件里的全部拓扑。
```

---

## 六、验证

### 6.1 三机编译（`make clean && make all`）

| 机器 | 架构 | 编译器 | 结果 |
|---|---|---|---|
| WSL Ubuntu | x86_64 | gcc 15.2.0 | **0 error / 0 warning** |
| armbian-1（RK3399） | aarch64 | gcc 13.3.0 | **0 error / 0 warning** |
| Pi 3B（开发主机） | aarch64 | — | 按现行铁律【不在 Pi 上编译】，只做编辑 / 提交 / 打包 |

- `make check-tools`：**✓ 19/19** 工具产出（WSL 与 armbian 均通过）。
- `make check-version`：**PASS**（版本号 SSOT + 3 份活文档 / 7 处锚点一致）。

### 6.2 显示宽度检查器（本次新增的验证手段）

把连续的、含 `╔╠╚║` 的行聚成"框块"，要求块内每行的**终端显示列宽**相同
（EAW `W`/`F` 记 2 列）。全工具启动框矩阵（CWD=`/`，数据根=沙箱）：

| | 框块 | 错位 |
|---|---|---|
| 修前（wip5） | 14 | **2** |
| 修后（wip6） | 14 | **0** |

### 6.3 资产 SSOT 权威性（决定性 A/B）

在外来 CWD 里放一份**同名诱饵** `data/jieba_dict.txt`（只有 1 条词），从该 CWD 启动：

```
[词典] 从 /home/cx/pm-0534-sb/data/jieba_dict.txt 加载 10 条词
词典已加载: /home/cx/pm-0534-sb/data/jieba_dict.txt
```

加载的是**数据根**里的 10 条，不是 CWD 里的 1 条 ⇒ **SSOT 是权威，CWD 不再参与**。
负对照（数据根指向空目录）则明确告警「词典不存在…（逐字模式，分词质量会下降）」。

### 6.4 `quick_chat` 三态退出码（实测）

```
① 文件缺失 RC=0   + 「将以【空脑】启动（首次运行属正常）」+「不按加载异常处理」
② 文件损坏 RC=1   + multi_topology fail-loud（fmt_ver 越界诊断）
③ 文件在但≤10节点 RC=1 + 「× 状态加载异常」
```

### 6.5 `state_dump` 计数

沙箱态（`batch_learn` 现场产出）：
```
  子拓扑数:     12        ← 修前恒为 0
  总节点数:     555
  总连接数:     500
```
「顶部 N 节点 vs 详情 0」的矛盾消失。

---

## 七、诚实边界与后续

1. **`ui_frame_row` 超宽契约变了**：由「不截断、推走右边框」改为「按显示列在码点边界截断 + `…`」。
   这意味着**超宽内容会被截**（有明示）。取舍理由：右边框对齐优先于完整打印；
   看到 `…` 就知道该去加宽框或拆行。若某处确实需要完整内容，应改框宽而不是指望 API 溢出。
2. **新 API 有两条路径只有编译期覆盖、没有运行时覆盖**：
   - `ui_frame_stream()` 切流（stderr）目前唯一使用者是 `template_builder.c` 的
     `template_diagnose_pos_coherence()` —— 而该函数**全仓没有调用点**；
   - `ui_print_header()` 同样**全仓没有调用点**（只有定义）。
   ⇒ 它们的改动属**编译期一致性 + 为将来铺路**，本版无法给出运行时证据。不装作验过。
3. **`state_dump` 的历史输出不可信**：此前它注册 0 个子拓扑，节点全被丢弃而计数为"成功"，
   所以「总节点数」一直是假的。**本次之前的任何 `state_dump` 报告都应作废重跑。**
4. **新挖出的既有缺陷（只让它可见，本版未修）**：
   `batch_learn → build_cross_links → state_dump` 这条链上，`state_dump` 加载时报
   `跨链引用越界丢弃 250/750 (33.3%)`，日志自己给了成因：
   「加载期 id 整体重排（种子副本位移），cross_links 仍用文件原始 id」
   与「跨代 id 空间 / 剪枝重编号未同步」。
   ⇒ 跨链 id 空间在「加载→重存」循环里不稳，需要**单独一轮**治理，不夹带在本版。
5. **`compound_promote` 标题框宽度不再固定**：改自动宽度后随 `阈值/上限` 的位数变化（本例 52）。
6. **`tests/` 下 6 个手写框未迁移**（测试脚手架），`tests/scratch/` 里的 legacy 更不在范围。
7. **全仓行尾政策缺失**：155 个受跟踪的 `*.c/*.h/*.md` 是 **CRLF**，且没有 `.gitattributes`。
   本版打补丁时踩到过坑 —— Python 用 `open(..., "w")` 写回会把 CRLF 规整成 LF，
   两个文件（`src/topo_eval.c`、`src/template_builder.c`）因此产生 **2075 行伪改动**；
   已 `git checkout` 回滚并改用**保行尾的二进制读写**重打。
   **行尾混用本身是技术债**，建议单独立项（加 `.gitattributes` + 一次全仓规整），
   本版刻意不夹带（否则 diff 没法评审）。
8. **部署阻塞仍未解除**（沿用 077）：线上真实数据是 `fmt_ver=9` + 旧扁平布局，
   直接部署新二进制仍会**启动即拒绝**。正确顺序仍是：先跑 `migrate-home-layout.sh` 搬数据，
   再处理 fmt_ver 闸门。本版没碰状态格式闸门。
