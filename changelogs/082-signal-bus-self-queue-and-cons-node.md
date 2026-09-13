# 082 — 信号总线第一批接线：丘脑自用信箱 + `CONSOLIDATE_NODE` 投递

> 版本 v0.5.39 · 2026-09-13 · 分支 `feat/signal-bus` · 基点 `e48aa48`（= main = v0.5.37）
> 工作区 `/home/cx/pm-fix`（Pi 3B）· 编译验证：WSL（gcc 15.2.0）+ armbian-1（gcc 13.3.0）

---

## 一、来源与病灶

### 1.1 判据来自老大

> 「**全部补收件人，我就说嘛，地基绝对不稳。**」

### 1.2 全仓三层「拔线」扫描的结果

| 层 | 扫法 | 命中 | 真病 |
|---|---|---|---|
| L1 | `grep -rnE '\(void\)[A-Za-z_]\w*;'`（显式丢弃参数） | 53 处 | **独苗 1 处**（感知区 `(void)throttle;`） |
| L2 | 签名有参数 / 函数体未用（自写脚本 `_scan_l2.py`） | 5 处 | 0（均为接口对齐） |
| L3 | 信号总线比对（定义 vs 发送点 vs 消费点） | **19 种信号** | **大面积空转** |

L3 的结论（确凿，非推断）：

- ✅ **活 1 种**：`FEEDBACK_REPORT`（6 个上报点）
- 💀 **发了被烧 7 种**：`CONSOLIDATE_NODE` / `REASONING_START` / `REASONING_END` / `IDEA_SELECTED` / `VISUAL_FRAME` / `CROSS_MODAL_EDGE` / `MEDIA_FILE_DONE`
- ❌ **从来没人发 10 种**：`HEARTBEAT` / `CIRCADIAN_UPDATE` / `PAUSE` / `RESUME` / `THROTTLE_UPDATE` / `SEARCH_RESULT` / `DIALOG_EVENT` / `SUBGOAL_START` / `SUBGOAL_RESULT` / `IDEA_PROPOSED`
- 🔴 `thalamus_recv_signal()` / `thalamus_has_signal()` —— **全仓只有声明 + 定义，零调用者**

### 1.3 根因：不是「忘了建收件端」，是「丘脑自己没有信箱」

`ThalamusSubsystem` 枚举 10 个脑区（`THAL_PREFRONTAL=0` … `THAL_VISUAL_CORTEX=9`），**没有丘脑自己那一项**。

而 `FEEDBACK_REPORT` 的注释写「广播到丘脑自己（在 tick 中被消费）」⇒ 实现只能**遍历全部 10 个脑区队列**来捞自己的信。副作用：

> **每次丘脑 tick（每 30 拍）把所有人的定向信箱扫荡干净** —— 定向信号在收件方读到之前即被丢弃。
> 这同时解释了「`recv_signal` 全仓零调用」：**收了也没用，下一个 tick 反正会被清空。**

作者的注释也印证了这一点 —— `hippocampus.c:121-130` 原文：

> 主路径：直接调用感知皮层公开 API `perception_consolidate_node()`
> 备选路径：通过丘脑信号总线发送 `CONS_NODE` 信号（解耦）

**「备选路径」从来没通过。**

---

## 二、本版做了什么

范围：**第 0 步（修信箱）+ 甲档第一条（打通 `CONSOLIDATE_NODE`）**，作为后续 16 种信号的样板。

### 2.1 `include/thalamus.h` —— 新增丘脑自用槽

```c
/* v0.5.39: 丘脑自己的信箱槽位 —— 丘脑不属于 10 个脑区，此前没有自己的队列，
 * 只能靠「遍历全部脑区队列」来捞广播给自己的信（FEEDBACK_REPORT，target=-1），
 * 副作用是把各脑区的定向信一并清空 ⇒ 定向信号永远到不了收件方。
 * 此宏即 signal_queues[] 的末槽下标，把「丘脑自己」与「各脑区」在存储上分开。 */
#define THAL_SELF_QUEUE  (THAL_SUBSYSTEM_COUNT)
```

```c
/* ── 信号队列（每个脑区一个 + 末槽 THAL_SELF_QUEUE 为丘脑自用） ── */
struct {
    BrainSignal slots[THAL_SIGNAL_QUEUE_SIZE];
    int head, tail, count;
} signal_queues[THAL_SUBSYSTEM_COUNT + 1];
```

`thalamus_create()` 走 `calloc` ⇒ 新槽自动清零，**无需额外初始化**。

### 2.2 `src/thalamus.c` —— 三处

| # | 函数 | 改前 | 改后 |
|---|---|---|---|
| ① | `thalamus_send_signal()` | `if (target >= 0 && target < THAL_SUBSYSTEM_COUNT)` | `if (target >= 0 && target <= THAL_SELF_QUEUE)` |
| ② | `thalamus_send_feedback()` | `sig.target = -1`（广播 → 10 个队列各 1 条） | `sig.target = THAL_SELF_QUEUE`（只 1 条） |
| ③ | `thalamus_tick()` | `for (r = 0; r < THAL_SUBSYSTEM_COUNT; r++) { while (队列非空) pop; }` —— **扫荡并清空全部脑区队列** | `while (signal_queues[THAL_SELF_QUEUE].count > 0) { ... }` —— **只消费自用槽** |

### 2.3 甲档第一条：`CONSOLIDATE_NODE`（海马体 → 感知区）

新增 `perception_request_concept()`：

```c
/* ⚠️ 只入队、不执行：同步版 perception_consolidate_node() 会跑
 * search_and_learn（HTTP）——那正是 v0.5.8 修掉的「脑干主循环被网络拖死」。 */
int perception_request_concept(Perception* p, int node_id) {
    if (!p || !p->topology || node_id < 0) return 0;
    SubTopology* vocab = master_get_sub_topology_by_type(p->topology, TOPO_VOCABULARY);
    if (!vocab || !vocab->net || node_id >= vocab->net->node_count) return 0;
    ReasoningNode* node = vocab->net->nodes[node_id];
    if (!node || !node->concept || !node->concept[0]) return 0;
    return _perception_enqueue(p, node->concept);
}
```

投递落在 `src/brainstem.c` 的 `brainstem_tick_perception`：

```c
if (p) {
    BrainSignal sigs[4];
    int n = thalamus_recv_signal(th, THAL_PERCEPTION, sigs, 4);   /* ← 全仓第一个 recv 调用点 */
    for (int i = 0; i < n; i++) {
        if (sigs[i].type == THAL_SIG_CONSOLIDATE_NODE) {
            perception_request_concept(p, sigs[i].data.consolidate.node_id);
        }
    }
}
```

**设计要点：快慢分离。** 收信的人只把请求**放进待办队列**，真正的 HTTP 由 perception worker 线程串行跑 —— 主循环永不被网络阻塞。

---

## 三、验证

### 3.1 三层 + 第二编译器

| 层 | 手段 | 结果 |
|---|---|---|
| 编译 | WSL `make clean && make all -j8`（告警按 `[-Wxxx]` 分组统计） | ✅ **0 error / 0 warning** |
| 内存安全 | WSL `make asan-test` | ✅ **15 / 15 PASS** |
| 回归 | WSL `make test`（33 个检查） | ✅ **32 通过 / 0 失败** |
| 功能 | 独立投递验证程序 `vtest.c`（17 条断言） | ✅ **17 / 17 PASS** |
| 第二编译器 | armbian-1（aarch64 / gcc 13.3.0） | ✅ 见 §3.3 |

### 3.2 ★ 投递验证程序的关键设计

**范式：每条断言直接对位一处代码改动，且必须「可区分」——旧实现必 FAIL、新实现必 PASS。**

| # | 断言 | 旧实现 | 新实现 |
|---|---|---|---|
| [2] ★核心★ | 定向信发出后跑一次 `thalamus_tick`，信**仍在** | **0 条** | **1 条** |
| [4] | 反馈只进丘脑自用槽，**10 个脑区队列零污染** | **10 条** | **0 条** |
| [5] | 给两个不同脑区各发一条，tick 后**两条都在** | 0 条 | 2 条 |

**⚠️ 第一版设计错误（已改）：** 原断言「`thalamus_send_feedback(...,7,3,0)` 后 `th->fb_hippo_consolidated == 7`」——**必 FAIL**。因为消费循环把值**累加**进累加器后，紧跟的 sigmoid 块**在同一次 tick 内立刻清零**（`thalamus.c:397`），外部永远观测不到中间值。

**改法**：不看累加器，**直接检视队列里那条信的类型与载荷**（`FEEDBACK_REPORT` / `consolidated=7` / `searched=3`）—— 这才是不可被后续代码抹掉的硬证据。

⇒ **通用纪律（已写进工程纪律档案 §7.9/§7.10）**：
- 凡断言对象有「读完即归零 / 用完即释放」的结构，**断言中间值一律无效**，必须找**清场之后仍存在的证据**（队列内容 / 文件字节 / 调用计数）。
- **不可区分的断言只能证明「代码跑起来了」，证明不了「改动生效了」。**

### 3.3 armbian-1 第二编译器

> 结果见本版 `CHANGELOG.md` 的 Verified 节与本仓 `pm_wip\玄枢-v0539-信号总线第一批闭环.md`。

---

## 四、诚实边界

1. **只做了 1 条链路**（`CONSOLIDATE_NODE`）。甲档其余 6 种（`REASONING_START` / `REASONING_END` / `IDEA_SELECTED` / `VISUAL_FRAME` / `CROSS_MODAL_EDGE` / `MEDIA_FILE_DONE`）**尚未接线**。
2. 🔴 **更正一处口径**：此前说「甲档 7 种**收件人明确**，直接补即可」——**查下来不成立**。除 `CONSOLIDATE_NODE`（收件人 = 感知区，语义明确）外，其余 6 种**全是「状态通知」式广播**（`target = -1`，多数无载荷），**收件人需要逐个判别**；且投出去必须**同时补消费端**，否则定向信会填满 16 格队列后静默丢弃（**比广播更糟**）。⇒ **需老大拍板后才能动手**。
3. **乙档 10 种从来没人发** ⇒ 须先补发件端，且部分「收到后干什么」需要定义语义。
4. **已知遗留**：`thalamus_recv_signal()`（`thalamus.c:176`）/ `thalamus_has_signal()`（`:194`）的 `region` 边界仍是 `>= THAL_SUBSYSTEM_COUNT` 即拒，与 `thalamus_send_signal()` 放宽后的边界**不对称** ⇒ **丘脑读不了自己的信箱**（实测两处对 `THAL_SELF_QUEUE` 均返回 0）。当前**无功能影响**（唯一读自用槽的是 `thalamus_tick` 内部直读数组，不走这两个 API），属语义不对称。
5. **未上线部署**；线上 armbian 8080 实例（`pid 2215289`）与线上数据**未触碰**。
6. **本版不改状态格式**：`Thalamus` 结构**不参与持久化**（全仓唯一引用是 `thalamus_create()` 的 `calloc`），⇒ `STATE_FORMAT_VERSION` 保持 **10** 不变，**无迁移、无回退风险**。

---

## 五、方法论留痕（供复用）

1. **三层拔线扫描法**：L1 显式丢弃 → L2 隐式未用 → L3 信号总线比对（定义 / 发送点 / 消费点）。
2. ⚠️ **L1 一个独苗 ≠ 系统性问题**：我先猜「可能不止一处」，L1 证明猜错；真问题在 L3。**先扫完再下结论。**
3. ⚠️ **一个上报点失效 ≠ 该脑区反馈失效**：`thalamus_send_feedback()` 全仓 **6 个上报点**，`hippocampus.c:142` 传的是真实值 ⇒ **海马体那条刹车是活的**。**先穷举全部上报点再下结论。**
4. **验证「消费」必须在消费者清场之前取样**（§3.2）。
5. **每条断言都要「可区分」**（§3.2）。
6. **工作副本 ≠ 仓库**：`~/pm-53` 是纯工作树，跑 `make test` 时 `check-version` 报「`ARCHITECTURE.md` / `README.md` / `README.zh-CN.md` <缺失>」，**一度被读成代码回归**；补齐文档后立刻 32/0。⇒ **读文件型门禁失败，先查文件在不在，再查代码改没改。**
