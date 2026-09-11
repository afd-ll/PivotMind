# 负控反证（§4.2.3）——"门禁有没有牙"的自证

> 🔴 **执行者不是 R3**：这些负控要**改 `src/**`**，那是 R1/R2 的领地（R3 的唯一所有者不含 `src/**`）。
> 本文件只**写命令、不执行**，交给验证阶段由对应所有者做。
> 依据：`falsification.md` M4 的教训 —— **没有 mutation 自证的门禁等于没门禁**（V1）。

---

## NC-1（对 R3-1 批次完成语义）——G-T1 必须立刻变红

把 `pool->tasks_left = count;` 改回"投给 `num_workers` 计数的趟数账"，
或在屏障里把判据改回 `workers_done`。做完**必须**观察到：

1. G-T1 立刻出现 `*** CONTRACT VIOLATED ***`（`tasks_completed_at_return < count`）；
2. ASan 立刻报 `heap-use-after-free src/dialog_system.c:149`
   （既有证据 `uaf-dialog-topoworker.md §③` 已证明该判定有效）。

```bash
# 示例（改前必须 git stash/备份；取证后立即复原）——下面的行号以 §1.5 定稿为准
# grep -n "tasks_left" src/thread_pool.c
# 复原后： bash tests/round3/run_g_t1_probe.sh  应重新全 HOLDS
```

## NC-2..NC-4（对 R3-2 三处锁）——TSan ① 必须对应地非 0

**逐处**注释掉加锁（保留其它两处），每处单独取证一次：

| 编号 | 站点 | 期望 TSan 报出的行号（与 §3 列的行号一一对应） |
|---|---|---|
| NC-2 | `master_activate_node`（`multi_topology.c:783-829`） | `multi_topology.c:(804\|810\|811\|813\|824\|825\|826)` |
| NC-3 | `master_propagate_activation`（`multi_topology.c:888-962`） | `multi_topology.c:917` |
| NC-4 | `boost_connection_weighted` **读侧**（`autonomic_learner.c:476-571`） | `autonomic_learner.c` |

```bash
# 每处做一次：注释 -> 重跑 G-T2 -> 断言「本批站点计数」>=1 -> 复原
# 判据命令（§4.2.2 ①，把期望 0 反过来读）：
grep -E 'SUMMARY: ThreadSanitizer' artifacts/tsan_fix_r3_ncNN.txt \
 | grep -E 'multi_topology\.c:(804|810|811|813|824|825|826|917)|autonomic_learner\.c' | wc -l   # 期望 >=1
```

> **这三条负控是"锁没白加"的唯一证明**（§4.2.3 原文）。

---

## NC 取证留档

各次负控的证据放 `tests/round3/artifacts/`，命名 `tsan_fix_r3_ncNN.txt`，
并在 `artifacts/README.md` 里补一行「NC 编号 → 站点 → ① 计数 → 复原确认」。
