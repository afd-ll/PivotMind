# v0.5.31 — 工具层并入构建 + 三编译器告警清零

> 来源：**老大一句「工具要做好，然后要跑那些检测，通常能抓出来很多东西的」**。做完发现两件事：
> ① **`tools/` 层长期处于「零自动编译」状态** —— 23 个 `.c` 里 11 个**根本没有二进制规则**、
> 9 个虽有规则却**不在 `all:` 里**，`make linux`（CI 用的就是它）只编 4 个二进制；
> ② 拿 3 个不同版本的 GCC 交叉扫一遍，**真抓出了 4 类实打实的 bug**（见 Fixed）。
> 权威工作区 `/home/cx/pm-fix`（Pi 3B），分支 `feat/paths-callsite-migration`，`main` 未动。

## 一句话结论

工具不是「写好了没人编」，就是「编了但没进默认构建」。这一版把它们**全部接进 `all:`**，
并在 CI 两个 job 各加一道 `make check-tools` 门禁（清单与 `all` 同源，禁止手抄），
然后把 3 个编译器扫出来的告警全部清零。

---

## Added

### 构建系统：工具终于进了构建
- **`TOOL_BINS`（20 个工具二进制）**：新增单一清单变量，`all:` 与 `tools:` **共用同一份**。
  收录口径 = `tools/*.c` 中所有「有 `main` 或可独立成二进制」者；唯一例外 `probe_batch_contract`
  （它有自己的独立门禁，且按设计不进 `all` / `test` / `asan-test`）。
- **`tools:` 聚合目标**：显式构建全部工具。
- **`check-tools:` 门禁目标**：用 `TOOL_BINS` 断言 20 个二进制**全部已产出**，不触发构建。
  写死在 Makefile 里、CI 只调不抄 —— 手抄清单必然与 Makefile 漂移。
- **11 条缺失的二进制规则 + 11 个 phony 别名**：`batch_test` / `build_cross_links` /
  `compound_promote` / `debug_load` / `feed_cli` / `hebbian_pretrain` / `merge_state` /
  `quick_chat` / `reader` / `seed_teacher` / `state_dump` —— 这些此前**只有源文件、没有任何构建入口**。

### CI
- `build-x86_64` 与 `build-arm` 两个 job 各新增一步 `make check-tools`（全工具门禁）。
  **前置依赖**：`make linux` 已经把工具编出来（它们在 `all:` 里了），所以这一步是**纯断言**、秒级。

## Changed

### `all:` 并入全部工具
- `all: $(LIB_NAME) seed-builder debug-seed gateway digital-life $(TOOL_BINS)`
- ⚠️ 代价与边界：`all:` 变重。**内存受限设备不要跑 `make all`**（Pi 3B 905Mi + LTO）。
  受限机器请用定向目标：`make gateway` / `make seed-builder` / `make <工具名>`。
  这条已写进 Makefile 注释，避免下一个人踩。

### `batch_learn_lowmem` 规则修正（旧规则是**错的**）
```make
# 旧（错）：宏写在链接行 —— 对已编译好的 .o 完全无效，而它链接的正是没定义宏的那个 .o
$(BUILD_DIR)/batch_learn_lowmem: $(OBJ_DIR)/batch_learn.o $(LIB_NAME)
	$(CC) $(CFLAGS) -DLOW_MEM -o $@ $(OBJ_DIR)/batch_learn.o ...
# 新：编译自己的翻译单元（tools/batch_learn_lowmem.c 里 #define LOW_MEM + #include batch_learn.c）
$(BUILD_DIR)/batch_learn_lowmem: $(OBJ_DIR)/batch_learn_lowmem.o $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/batch_learn_lowmem.o ...
```

### Makefile 注释乱码修复
- 仓库里的 `Makefile` 有 **14 处 U+FFFD 替换字符**（注释中文早被破坏），本次一并复原，残留 0 处。
  纯注释改动，不涉及任何构建语义。

## Fixed（3 个编译器交叉扫出来的真问题）

| # | 位置 | 性质 | 说明 |
|---|------|------|------|
| 1 | `tools/quick_chat.c:35` | **编译错误（GCC 14+）** | `master->ext_dict = d;` 缺 `(struct ExternalDict*)` 强转。三个同名站点（`batch_learn.c:342` / `feed_cli.c:213` / `multi_topology.c:73`）早已有强转，**独此漏网**。GCC 13 只 warning，**GCC 14 起 `-Wincompatible-pointer-types` 升为 error** ⇒ 该工具在新编译器上根本编不过。 |
| 2 | `tools/reader.c` `is_sent_end()` | **静默架构 bug** | `char` 在 x86_64 上是**有符号**的（−128..127），所以 `p[1] == 0xBC`（188）**恒假**；中文句末标点判定在 x86_64 上完全失效。ARM 上 `char` 无符号，所以此前「看起来正常」。改走 `const unsigned char*`。 |
| 3 | `batch_learn.c` / `build_cross_links.c` / `hebbian_pretrain.c` | **缓冲截断** | `char bak[520]; snprintf(bak, 519, "%s.bak", path);`，而 `path` 上限 `PM_PATH_MAX=4096` ⇒ 路径偏长时**备份文件名被静默截断**（备份落到错的名字上）。三处统一改为 `PM_PATH_MAX + 后缀` 并用 `sizeof`。由 GCC 14.2 的 `-Wformat-truncation` 抓出 —— **GCC 15.2 不报**。 |
| 4 | `tools/merge_state.c` | **静默吞错 + 计数错乱** | ① 14 处 `fread` 返回值未检查 ⇒ 新增 `rd_or_die()`，状态文件被截断时**显式报错退出**，不再把残缺/未初始化缓冲区当数据静默合并。② 17 处 `int` → `uint32_t`（`total_nodes/edges_a/b`、`n_merged`、循环变量、`str_count_a/b`）+ 4 处 `printf %d`→`%u`。 |
| 5 | `tools/hebbian_pretrain.c` | 健壮性 | `fread` 返回值检查 + `ftell` 负值与 `malloc` 失败守卫。 |
| 6 | `demos/digital_life.c` | 告警 | `system("clear"/"cls")` 返回值。**注意**：`(void)system(...)` **不能**抑制 GCC 的 `-Wunused-result`（已实测），必须把返回值赋给变量。 |
| 7 | 若干 | 告警 | `seed_teacher.c` `int i`→`size_t`；`state_dump.c` 误导缩进；`build_cross_links.c` 删未用常量；`corpus_train.c` `char filepath[1024]`→`PM_PATH_MAX`；`batch_learn.c` `char path[512]`→`PM_PATH_MAX` + `sizeof`；`batch_learn.c` `snprintf(...,63,...)`→`sizeof`。 |

## Verified（证据）

三台机器、三个编译器版本，`make clean && make all` 全量构建 —— **0 error / 0 warning**：

| 机器 | 架构 | gcc | 方式 | 结果 |
|------|------|-----|------|------|
| WSL/G15 | x86_64 | **15.2.0** | `make clean && make -j8 all` | 24 二进制 / **0E 0W** |
| astar728-1 | armv7l (Pi 3B) | **14.2.0** | 12 个改动文件逐个 `gcc -c`（不做 LTO，905Mi 限制） | **0E 0W** |
| armbian-1 | aarch64 (RK3399) | **13.3.0** | 干净克隆 + `make clean && make -j4 all` | 24 二进制（20 工具齐全）/ **0E 0W** |

- `make check-tools` ⇒ `✓ 全部 20 个工具已产出`。
- 行尾：`Makefile` 保持 **CRLF**（551 对），工具源全部 **LF**；改造后 `U+FFFD` 残留 **0**。
- armbian 用的是从 bundle **全新克隆**的干净树（`~/pm-s31tools` @ `cbca59e` + 本次 12 文件），
  未触碰线上那台已有他人未提交改动的 `~/pm-step3`。

## Known Issues / 待老大决策

1. **`CROSS_REBUILD_INTERVAL` 是死宏** —— `tools/batch_learn.c` 里除两行 `#define` 自身外**零引用**。
   真实重建发生在「每个 epoch 结束」与「训练收尾」，也就是说「每 5000 条 QA 重建一次」的机制
   早被重构掉了、宏是遗留垃圾。**后果：`batch_learn_lowmem` 与 `batch_learn` 产出的二进制逐字节相同**
   （md5 一致，已实测），所谓的「低内存版」当前**没有任何实际差异**。
   已在源码处标注 `[待决策]`；处置方案（① 直接删掉这个变体 ② 把 `LOW_MEM` 接回重建判定）
   **涉及训练内存语义，未擅自决断**。
2. `demos/gateway_handlers.c:618`、`demos/pivotmind_gateway.c:236` 的 `snprintf(x, N, ...)` 用了
   字面量尺寸（当前与缓冲同值，**安全**但非 `sizeof`）。**未改** —— 涉及线上部署二进制，单独评估。
3. **CI 只在 `main`/`master`/`develop` 上触发** ⇒ `feat/*` 分支不跑 CI，这也是本轮问题能长期潜伏的原因之一。
   本次加的 `check-tools` 门禁要等合入 `main` 才真正生效。

## 方法教训（值得进纪律）

- **`gcc -fsyntax-only` 会漏报 `-Wunused-result`** —— 第一轮用它扫出「23/23 全零告警」，真实 `-c` 编译后
  才发现 16 个。**编译检测必须真实编译**，图快用 `-fsyntax-only` 等于自欺。
- **`(void)expr` 不能抑制 GCC `-Wunused-result`**（GCC 文档行为，已用最小样例实测确认）；必须赋值给变量。
- **单编译器不足以定案**：`reader.c` 的有符号 char 要 x86_64 才暴露，`bak` 截断要 GCC 14.2 才报，
  `quick_chat` 的强转要 GCC 14+ 才升级为 error。三个版本交叉扫，才算「跑过检测」。
