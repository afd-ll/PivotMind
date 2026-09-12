# CI 工作流说明（`.github/workflows/ci.yml`）

本目录只有一份工作流：`ci.yml`，workflow 名为 `CI`。

- **触发**：`push` 到 `main` / `master` / `develop`；以及面向 `main` / `master` 的 `pull_request`。
- **徽章 URL**（README.md 与 README.zh-CN.md 顶部各一行，靠 Version 徽章旁边，指向本工作流的运行页）：

  ```
  https://github.com/afd-ll/PivotMind/actions/workflows/ci.yml/badge.svg
  ```

> 徽章里的 workflow 文件名 `ci.yml` 必须与本目录下实际存在的文件名**逐字一致**（如日后改名，徽章与本文档需同步改）。

---

## ⚠️ 边界：CI 不替代真机验证

> **CI 跑的是 x86_64 上的快速门禁，不能替代 G15（x86 真实数据 ASan+UBSan）与 armbian（aarch64 生产）的验证。**

CI 只用仓库自身、不带真实语料，跑的是短时单测。它证的是“能编译、单元门禁不红”，**不**证真实数据下无泄漏，**不**证 aarch64 上的生产行为。G15 与 armbian 上的验证仍须照常执行；CI 绿灯不得被当作它们的替代品。

---

## 三个 job

| job | runner | 干什么 | 预计耗时\* |
|---|---|---|---|
| `build-x86_64` | `ubuntu-latest` | 装 4 个系统依赖 → `make linux` → `make test` → `file build/bin/*` | 约 5–10 分钟 |
| `build-arm` | `ubuntu-latest` | 装 aarch64 交叉工具链 → **从源码**交叉编译 zlib 1.3.1 + OpenSSL 3.0.15 + curl 8.12.1 → `make linux CC=aarch64-linux-gnu-gcc` → `readelf` 校验 ELF 机器类型与 `NEEDED` | 约 15–30 分钟（三者中最重） |
| `asan-ubsan` | `ubuntu-latest` | 装 4 个系统依赖 → `make asan-test`（ASan+UBSan 编译并运行核心单测） | 约 8–15 分钟 |

\* 以上为**量级估算，非 CI 实测值**，仅供排期参考。`build-arm` 最重，因为它要从源码交叉编译三个 C 库；`asan-ubsan` 要在 sanitizer 下重编整个静态库再逐条跑单测（每条 `timeout 120`）。

### 1. `build-x86_64` —— x86_64 快速门禁

步骤：`actions/checkout@v4` → `apt-get install libsqlite3-dev libssl-dev libcurl4-openssl-dev zlib1g-dev` → `make linux` → `make test` → `file build/bin/*`。

跑的是默认出货构建（Makefile 默认 `CFLAGS` 带 `-DHAS_OPENSSL`、`LDFLAGS` 带 `-lssl -lcrypto -lcurl -lz`），外加 `make test` 里的全部单元门禁。这是**最快的一道灯**——它红了，先在这里看。

### 2. `build-arm` —— aarch64 交叉编译门禁

步骤：`actions/checkout@v4` → 装 `gcc-aarch64-linux-gnu` + `libc6-dev-arm64-cross` → 依次从源码构建 ARM64 的 zlib（v1.3.1）、OpenSSL（3.0.15，`no-asm no-shared` 等精简配置）、libcurl（8.12.1，`--host=aarch64-linux-gnu`，去掉 nghttp2/zstd/brotli/ldap）→ 用 `CC=aarch64-linux-gnu-gcc` 显式传入 `CFLAGS`/`LDFLAGS` 跑 `make linux` → 用 `aarch64-linux-gnu-readelf` 校验产物是 aarch64 ELF 且 `NEEDED` 合理。

它证明的是“能在 aarch64 目标上链接出正确 ELF”，**不**跑真机、**不**跑生产负载。armbian 上的生产验证不在此处。

### 3. `asan-ubsan` —— 内存与未定义行为门禁

步骤：`actions/checkout@v4` → 装上述 4 个系统依赖 → `make asan-test`。环境变量：

```
ASAN_OPTIONS: "detect_leaks=1:halt_on_error=1:abort_on_error=1"
UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1"
```

sanitizer 旗标**不在 CI 里内联**，唯一来源是 `Makefile` 的 `ASAN_CFLAGS` / `ASAN_LDFLAGS`（`make asan-test`）——以免出现“CI 绿 ≠ 本地 asan 绿”。`detect_leaks=1` 与 changelogs/069 的“内存错误（含泄漏）与未定义行为直接红”一致。

---

## 为什么 `asan-ubsan` 不能关

这一条不是装饰。**它就是 2026-09-12 那次抓到 `layer_create_simple_rnn` 未初始化 + `model_train_step` 泄漏 + `dialog_input_create` 泄漏的那盏灯**（三处缺陷见提交 `0c7a960`）：

- `layer_create_simple_rnn` 未初始化 `layer->weights` / `layer->bias`，而其余四个 `layer_create_*` 构造器都设了它们，`layer_destroy` 因此解引用了 malloc 填充字节（ASan 报 `tensor=0xbebebebe…`）。
- `model_train_step` 丢弃了 `model_forward` 返回的 tensor——按前向契约该 tensor 归调用方释放，于是每个训练步泄漏一个预测 tensor。
- `dialog_input_create` 未释放 `utf8_tokenize` 分配的临时 token。

据提交 `0c7a960` 的说明，这三处缺陷被 **CI 红灯报了 298 次**。也就是说：**根因不是 CI 报错，是没人看**。把徽章装到 README 顶部，正是为了让这盏灯有地方被看见。

### 🚫 不许通过弱断言/删测试/关 sanitizer 来变绿

**不许通过弱断言、删测试、或关 sanitizer 来让这一条变绿。** 该次修复的验收原文即为：`make test 26/26`（原 25 passed / 1 failed）、`make asan-test` 在 `detect_leaks=1` 下 **0** sanitizer 报告（原 2 次 abort），且 “No check was disabled, no test removed, no assertion relaxed.”——绿必须是**真绿**。

注意 `changelogs/069` 曾记录过一段劣化史：`ASAN_OPTIONS` 里一度写着 `detect_leaks=0`（把 LeakSanitizer 关了），后被改回 `detect_leaks=1`。把 `detect_leaks` 改回 0、放宽断言、或从 `ASAN_TEST_TARGETS` 里摘掉用例，都是关灯，不是修灯。

---

## 依赖清单及依据

CI 三个 job 中，`build-x86_64` 与 `asan-ubsan` 安装同一组系统包（`ci.yml` 的两处 `apt-get install` 行）：

```
libsqlite3-dev libssl-dev libcurl4-openssl-dev zlib1g-dev
```

| 包 | 依据 |
|---|---|
| `libssl-dev` | Makefile 默认 `CFLAGS` 带 `-DHAS_OPENSSL`（`Makefile:19`）、`LDFLAGS` 带 `-lssl -lcrypto`（`Makefile:20`）；OpenSSL 代码路径见 `src/web_fetch.c` 的 `#ifdef HAS_OPENSSL`（`src/web_fetch.c:642`）。 |
| `libcurl4-openssl-dev` | Makefile `LDFLAGS` 的 `-lcurl`（`Makefile:20`）。changelogs/037 记录：该包曾从 install 行缺失，导致 `-lcurl -lz` 链接失败。 |
| `zlib1g-dev` | Makefile `LDFLAGS` 的 `-lz`（`Makefile:20`）。同上，changelogs/037 一并补入。 |
| `libsqlite3-dev` | 提供 `<sqlite3.h>`，供 `USE_SQLITE` 条件编译的 SQLite 路径使用（`src/causal_reasoning.c:14`、`include/dictionary_v2.h:4`）。**现状说明**：当前 Makefile 未定义 `USE_SQLITE`，这些代码默认不参与编译，故此包眼下并非编译硬依赖，属为启用 `USE_SQLITE` 的构建预留。 |

ARM job 的额外依赖是交叉工具链（`gcc-aarch64-linux-gnu`、`libc6-dev-arm64-cross`），以及它**从源码**构建的 zlib / OpenSSL / curl 三件套——不走系统 apt 包。

---

## 本地复现

```bash
make linux          # x86_64 默认构建（build-x86_64 job 的前半）
make test           # 单元门禁（build-x86_64 job 的后半）
make asan-test      # ASan+UBSan 门禁（asan-ubsan job 等价物）
make check-version  # 版本号 SSOT 门禁（已接进 make test）
```

ARM 交叉编译在本地按 `ci.yml` 的 `make linux CC=aarch64-linux-gnu-gcc …` 复现。
