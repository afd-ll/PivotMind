# 090 — v0.6.5：heat 跨会话持久化（状态格式 v11）

> 版本 **v0.6.5** · 2026-09-16 · 分支 `feat/v065-heat-persist`（基点 `b862d4b` = v0.6.4）
> 工作区 `/home/cx/pm-fix`（Pi 3B）· 编译验证：armbian-1（gcc 13.3.0）+ WSL（gcc 15.2.0）

---

## 一、来源

工作台 **BG-01**（原 A23）。0 级读码摸底已定性两问：

1. 「heat 是否常量」→ **是**，且原因不是「接近常量」而是**压根不落盘**（详见 §三）；
2. 「`sem_` 节点名泄漏 + 乱码」→ 泄漏当前未复现（输出端已有 7+ 处过滤），
   真缺陷是种子词 `馴﹔` 里的标点 `﹔`(U+FE54) —— `is_punctuation` 漏 U+FE50–FE6F，已**另开 BG-14**。

老大 2026-09-16 拍板 **A（heat 落盘）**。

## 二、核心变更（`7752e7c`，1 文件 +34 / -1）

全部在 `src/multi_topology.c`：

| 位置 | 改动 |
|---|---|
| `STATE_FORMAT_VERSION` | `10` → `11` + 版本注释（布局 / 兼容 / 行为影响 / 回退） |
| `SaveNodeSnap` | 加 `float heat` 字段 |
| `master_capture_batch` | 深拷贝 `ns->heat = node->heat` |
| `master_serialize_batch`（流式批写端） | 节点记录尾部加写 4 字节 |
| `master_save_state_locked`（锁内回退写端） | 同上（**两处写端同布局**） |
| `master_load_state` Pass 1 | `fmt_ver>=11` 读 + 夹取 `[0.05, 1.0]`（NaN/越界回落 1.0） |
| `master_load_state` Pass 2 | `fmt_ver>=11` SKIP 4 字节 |

**节点记录尾部布局**：`dist_sig[26] + dist_sig_count(int) + lang(uint8) + heat(float)`

## 三、为什么落盘（原病灶，0 级读码结论）

- 创建默认 `node->heat = 1.0f`（`src/huarong_topology.c:71`）；
- 运行期 heat 只在走边路径**单向衰减**：`multi_topology.c` stepped ×decay / winner ×0.995 / selected ×decay，
  floor 0.05，**无回升路径**；
- 存盘时 heat **不在节点段**（两处写端都只写 activation / features / edges / dist_sig / lang）⇒ 整体丢弃；
- ⇒ 每次 `master_load_state` 后全节点回创建默认 `1.0` —— A22 dump 的「3894/3894 全 > 0.5」**就是创建默认值**。

## 四、行为变化（如实登记，不夸大）

- 加载后**不再「全员满热」**：长期不活跃节点的 heat 停在 floor 0.05 并被带进下一次会话，
  打分 `score *= (0.05 + 0.95*heat)` ≈ **×0.0975** ⇒ 跨会话抑制（这正是「跨会话热度」的语义）。
- 与 **VF-02（节点 confidence 恒 0.5）** 的区别：confidence 是**派生量、压根没接线**；
  heat 是**真实运行变量**，本次只是让它跨会话 —— 不同根因，不合并处理。

## 五、兼容与回退

- `fmt_ver<=10`（v2..v10）文件无 heat 段 ⇒ 读端不读，保持创建默认 1.0，**零迁移、无格式风险**。
- ⚠️ **用 v11 存盘后回退不可逆**：旧二进制（认 ≤10）会被本二进制闸门**显式拒绝**
  （拒绝加载并 `return -1`）。线上换版前必须备份状态文件。
- ⚠️ 附注（实测发现）：gateway **不检查 `master_load_state` 的返回值** ——
  闸门拒绝后进程**照常继续启动（空状态）**，退出码不受影响（沙箱实测 RC 仍是 timeout 的 124）
  ⇒ 部署验收**不能只看退出码**，必须 grep 日志里的「拒绝加载」。（已单独登记）

## 六、验证读数

| 项 | 读数 |
|---|---|
| armbian 全量真编 | `CCACHE_DISABLE=1 make CC=gcc -j2 all` RC=0、141 gcc 步、0「Nothing to be done」、**0 告警** |
| WSL 全量 | `make CC=gcc -j20 all` RC=0、141 gcc 步、0 告警 |
| 单测 / 工具 | `make test-fast` **16 通过 / 0 失败**；`check-tools` **19/19** |

沙箱链条（armbian，线上状态**只读副本**，`fmt_ver=10` / 5,511,680 B / md5 `693a8040…`；
`PIVOTMIND_HOME` + cwd 双重隔离，端口 8791–8794）：

| 步 | 动作 | 读数 |
|---|---|---|
| A | 旧二进制 v0.6.4 读 v10 | 4383 节点 / 19489 链接 → 存盘仍写 **10** |
| B | 新二进制读 v10（兼容） | 4433 节点 / 19359 链接 → 两次 `/chat` 正常应答 → 存盘写 **11** ✅ |
| C | 新二进制读回 v11（闭环） | 4453 节点 / 19686 链接 → `/chat` 正常 → 再存盘仍 **11** ✅ |
| D | 头改 `fmt_ver=99` | 显式拒绝加载 ✅ |

## 七、未覆盖 / 遗留（不派活，仅登记）

- heat **数值本身**没有可观测入口（`a22_stats` 加一行 heat 直方图即可，可选）。
- 本轮只到「分支 + 三机编译验证 + 推送」，**部署未执行** —— 上线需先备份线上状态文件（回退不可逆）。
