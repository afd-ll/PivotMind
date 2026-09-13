# deploy/ —— 部署与运维

## 一、数据根与目录布局（v0.5.30 起）

数据/日志/会话/运行的落点全部由**路径 SSOT**（`include/pivotmind_paths.h` + `src/pivotmind_paths.c`）
决定，调用点一律取 `pm_home()` / `pm_dir()` / `pm_file()`，**不再看当前工作目录**。

数据根解析顺序（命中即止，解析一次并缓存）：

1. `$PIVOTMIND_HOME` —— 必须**非空、非全空白、绝对路径、不含 `~`、不是 `/`**
2. `$HOME/pivotmind`
3. `PM_HOME_DEFAULT` —— 编译期固定绝对路径，缺省 `/var/lib/pivotmind`（由 `Makefile` 平台化提供）

目录布局：

| 宏 | 路径 |
|---|---|
| `PM_DIR_HOME` | `<home>` |
| `PM_DIR_DATA` | `<home>/data` |
| `PM_DIR_LOG` | `<home>/log` |
| `PM_DIR_SESSION` | `<home>/session` |
| `PM_DIR_RUN` | `<home>/run` |
| `PM_DIR_CORPUS` | `<home>/corpus`（书库，默认自动建） |

12 个**数据文件**一律落在 `<home>/data/<name>`：

```
pivotmind_state.dat       主状态（脑干 / 健康监控 / TrainMode / 网关共同落点）
brain_state.dat           冻结节点缓存（退出即删）
features.bin              语义特征
cross_edges.bin           跨拓扑边备份
memory_seed.dat           记忆种子（有 D1 防覆盖门卫）
emergent_pos.bin          涌现词性锚点（带维度头 v2）
pivotmind_config.json     运行配置
intent_base.bin           意图基座
pfe_strategy.bin          PFE 策略
pfe_workspace.bin         PFE 工作区
pretrain_embeddings.bin   预训练嵌入
gw_token                  网关凭据（0600；原为编译期宏 GW_TOKEN_FILE）
```

`gw_token` 的路径现在等于 `pm_file(PM_FILE_TOKEN)` = `<home>/data/gw_token`。
**任何脚本/服务里写死的旧路径（如 `/home/cx/pivotmind/gw_token`）都已失效**，必须改。

## 二、⚠ 从 v0.5.28 及更早版本原地升级：旧扁平布局

v0.5.30 之前，上述文件直接放在 `<home>/` **根目录**（扁平布局）。旧部署换上新二进制后，
引擎在 `<home>/data/` 找不到 `pivotmind_state.dat`，会**新建空状态并正常启动、不报错**
—— 表现为「**升级后玄枢失忆**」。

v0.5.33 为此加了 **fail-loud 门**（`pm_legacy_layout_guard()`）：

| 场景 | 行为 |
|---|---|
| 无旧布局残留 | 静默（不打印） |
| 有残留，且 SSOT 主状态已就位 | `WARN` 列清单（纯残留，当前被忽略），继续运行 |
| 有残留，且 SSOT 主状态缺失 | `ERROR` 列清单；`gateway` **拒绝启动**（`digital_life` / `batch_learn` / `quick_chat` 只告警） |

**升级步骤（推荐路径）**：

```bash
# ① 先看计划（默认演练，不改动任何文件）
deploy/migrate-home-layout.sh --home /home/cx/pivotmind

# ② 停掉在用实例（脚本会拒绝在实例运行时动手）
sudo systemctl stop pivotmind

# ③ 真正搬（只搬不删、绝不覆盖）
deploy/migrate-home-layout.sh --home /home/cx/pivotmind --yes

# ④ 起实例
sudo systemctl start pivotmind
```

脚本语义：对每个登记文件，`<home>/<name>` 存在而 `<home>/data/<name>` 不存在 ⇒ 同盘 `mv`
（原子改名）；两边都在 ⇒ **跳过并告警**，交人工裁决。**不删任何数据**，回滚只需把文件搬回去。

**逃生开关**：确实想以空脑启动（例如故意重新训练），

```bash
export PIVOTMIND_ALLOW_LEGACY_LAYOUT=1     # 显式放行，日志仍会 WARN
```

## 三、systemd 服务

⚠️ **两回事，别混**：

| | 是什么 | 在哪 |
|---|---|---|
| **线上真身** | 正在跑的 unit，**权威** | `/etc/systemd/system/pivotmind-gateway.service`（**root-only 0600**） |
| `deploy/pivotmind.service` | **通用部署模板**，仅作新机起点 | 本目录 |

线上真身 2026-09-13 实取核对（`systemctl show` 可读，cx 用户 `systemctl cat` 会 Permission denied）：

```ini
[Service]
ExecStart=/bin/bash -c 'exec /home/cx/pivotmind/build/bin/pivotmind_gateway 8080 >> /tmp/pv62.log 2>&1'
WorkingDirectory=/home/cx/pivotmind
User=cx
MemoryMax=2726297600            # 2600 MiB
Restart=on-failure
# drop-in：pivotmind-gateway.service.d/jemalloc.conf
#   Environment=LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2
```

状态：`active` / **`disabled`**（**未开开机自启** —— 重启机器后不会自己起来）。

🔴 **数据根靠 `WorkingDirectory`（cwd）解析，线上<ins>不设</ins> `PIVOTMIND_HOME`。**
（`pm_home()` 的第二顺位就是 `$HOME`，而 unit 的 `User=cx` ⇒ `$HOME=/home/cx`。）
模板里那句「必须显式设置 `PIVOTMIND_HOME`」是**早期写法的遗留**，**与线上相反**，别再照抄。

**给新机部署**：从模板起步时，先在**非 8080 端口**做一次落点复检（cwd = 数据根、不设
`PIVOTMIND_HOME`、带 jemalloc），确认读写真的落在预期目录，再起服务。

**改线上 unit**：直接改 `/etc/systemd/system/pivotmind-gateway.service`（需 root），
改完 `systemctl daemon-reload`。**改错文件的典型后果**：以为改了配置、其实动的是仓库里
那份不起作用的模板。

## 四、长跑 / 压测脚本的隔离铁律

`tests/longrun/run_longrun_guard.sh` 的 `--data DIR` 会被 **`export PIVOTMIND_HOME="$DATA"`**。

原因：数据落点由 `pm_home()` 决定，**argv 只喂 `chdir()`**（语料相对路径用）。
v0.5.33 之前该脚本只把 `$DATA` 当 argv 传，却没设 `PIVOTMIND_HOME` ⇒ 引擎实际读写
`$HOME/pivotmind`（= 线上数据目录），脚本自以为的「沙箱」是**假的**。

> **铁律**：任何声称在沙箱里跑引擎的脚本，都必须自己 `export PIVOTMIND_HOME=<沙箱>`，
> 且 `PIVOTMIND_HOME` 必须与 argv 里的数据目录一致。改完先用
> `PIVOTMIND_HOME=/tmp/x <bin> --help` 之类确认落点，再放开跑。
