# C语言AI框架 Makefile �?PivotMind
#
# 增量编译: 每个 .c 独立编译�?.o，二进制仅链接所需 .o
# 改一个源文件 �?只重新编译该文件 �?重新链接相关二进�?
# 自动依赖追踪: -MD -MP 生成 .d 文件，头文件变化时自动重编译

# 编译器 — ccache 加速（未安装时自动回退到 gcc）
CCACHE := $(shell which ccache 2>/dev/null)
ifeq ($(CCACHE),)
  CC = gcc
else
  CC = ccache gcc
endif

# 并行编译 — 默认 2 路（低内存板防 OOM），命令行 make -jN 优先
JOBS ?= 2
MAKEFLAGS += -j$(JOBS)

# ========== 路径 SSOT：平台化缺省数据根（PM_HOME_DEFAULT）====================
# 语义：pm_home() 第 3 级（$PIVOTMIND_HOME、$HOME/pivotmind 都不成立时）的**编译期缺省位置**。
# 🔴 默认值里【绝不许出现 $HOME】：$HOME 是【构建期】变量，写进去等于把构建机的家目录钉进
#    二进制 —— CI/打包/换用户全错（作者定稿，别顺手“优化”）。
# 判定：$(PREFIX) 含 com.termux ⇒ Termux/Android；否则 uname -s 含 Android ⇒ 同样；
#       其余 Linux ⇒ /var/lib/pivotmind。
# 覆盖：make PM_HOME_DEFAULT=/some/where ...（命令行/环境变量优先，?= 不覆盖已定义值）
UNAME_S := $(shell uname -s)
ifeq ($(strip $(findstring com.termux,$(PREFIX))$(findstring Android,$(UNAME_S))),)
  PM_HOME_DEFAULT ?= /var/lib/pivotmind
else
  PM_HOME_DEFAULT ?= /data/data/com.termux/files/usr/var/pivotmind
endif
# 传【裸 token】而不是字符串字面量：引号交给 C 的字符串化（PM_STR）。
# 理由：make 的引号在不同配方里被 shell 剥的层数不同（普通构建 vs `make CFLAGS="$(ASAN_CFLAGS)"`
# 的嵌套 make），写成字面量必然在某一层被剥掉 ⇒ 编译期报 "expected expression before '/' "。
PM_HOME_CFLAGS = -DPM_HOME_DEFAULT_PATH=$(PM_HOME_DEFAULT)
# 硬门（主门）：PM_HOME_DEFAULT 必须是绝对路径（不许相对、不许 $HOME）。
# 依据：C 里字符串字面量的下标不是整型常量表达式（那是 C++ 的规则）
# ⇒ 编译期断言只能落在构建系统这一层（C 侧另有一道 GCC __attribute__((error)) 的次门）。
ifeq ($(filter /%,$(PM_HOME_DEFAULT)),)
$(error PM_HOME_DEFAULT 必须是绝对路径（当前值: '$(PM_HOME_DEFAULT)'）)
endif

# ⛔ 下面这一行是**原样保留**的原 CFLAGS 赋值行（它不是被替换掉的，
#    追加 PM_HOME_DEFAULT 一律走下面的 `CFLAGS +=`，绝不动这一行 —— 本仓在册坑：
#    覆盖式改 CFLAGS 会把 -Iinclude / -MD -MP 一起冲掉）。
CFLAGS = -pipe -Wall -Wextra -O2 -Iinclude -Iinclude/nn -Isrc/nn -I. -Ilibs -std=gnu99 -fopenmp -pthread -MD -MP -D_USE_MATH_DEFINES -D_FORTIFY_SOURCE=2 -flto=auto -DHAS_OPENSSL
LDFLAGS = -lm -lssl -lcrypto -lcurl -lz -flto=auto
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -Iinclude -Iinclude/nn -Isrc/nn -I. -Ilibs -std=gnu99 -fopenmp -pthread -MD -MP -DDEBUG -D_FORTIFY_SOURCE=2
# ASan 旗标唯一权威来源（CI 不得再内联一份，见 ci.yml asan-ubsan job）。
# 与默认出货构建（本文件 :19 CFLAGS 带 -DHAS_OPENSSL、:20 LDFLAGS 带 -lssl -lcrypto -lz）
# 对齐，否则本地 `make asan` 覆盖不到 web_fetch.c 的 OpenSSL 代码路径。
ASAN_CFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -Iinclude -Iinclude/nn -Isrc/nn -I. -Ilibs -std=gnu99 -fopenmp -pthread -MD -MP -DDEBUG -DHAS_OPENSSL
ASAN_LDFLAGS = -fsanitize=address,undefined -lm -lcurl -lssl -lcrypto -lz
# 路径 SSOT 缺省值：默认 / DEBUG / ASAN 三套旗标同源（追加式，见上方平台化定义）——
# 任何一套漏掉 PM_HOME_DEFAULT，都会让那一套构建出来的 pivotmind_paths 少了第 3 级。
CFLAGS += $(PM_HOME_CFLAGS)
DEBUG_CFLAGS += $(PM_HOME_CFLAGS)
ASAN_CFLAGS += $(PM_HOME_CFLAGS)

# 依赖生成收成一处（-MF 脐支点）：默认/DEBUG/ASAN 的 CFLAGS 均已带 -MD -MP，
# 此时 DEPFLAGS 只补 -MF，编译命令行与改动前逐字一致；
# 若调用者覆盖 CFLAGS 却漏了 -MD -MP，这里自动补上，
# 避免 cc1: error: to generate dependencies you must specify either '-M' or '-MM'。
DEPFLAGS = $(if $(findstring -MD,$(CFLAGS)),,-MD -MP )-MF $(DEP_DIR)/$*.d

# 输出目录
BUILD_DIR = build/bin
OBJ_DIR = build/obj
DEP_DIR = build/dep
$(shell mkdir -p $(BUILD_DIR) $(OBJ_DIR) $(DEP_DIR))

export TMPDIR = /tmp

# 源文件（通配自动发现�?
# ⇒ 新增 src/pivotmind_paths.c（路径 SSOT）由 $(wildcard src/*.c) 自动进入
#    CORE_SRC/CORE_OBJ，**无需手工登记**；核验：make -n libpivotmind.a | grep pivotmind_paths
CORE_SRC = $(wildcard src/*.c) $(wildcard src/nn/*.c)
TOOL_SRC = $(wildcard tools/*.c demos/*.c)

# 所�?.o 文件（映射到 obj/ 目录�?
CORE_OBJ = $(patsubst src/%.c, $(OBJ_DIR)/%.o, $(CORE_SRC))
TOOL_OBJ = $(patsubst tools/%.c, $(OBJ_DIR)/%.o, $(filter tools/%.c, $(TOOL_SRC)))
TOOL_OBJ += $(patsubst demos/%.c, $(OBJ_DIR)/%.o, $(filter demos/%.c, $(TOOL_SRC)))

# 依赖文件
CORE_DEP = $(patsubst src/%.c, $(DEP_DIR)/%.d, $(CORE_SRC))
TOOL_DEP = $(patsubst tools/%.c, $(DEP_DIR)/%.d, $(filter tools/%.c, $(TOOL_SRC)))
TOOL_DEP += $(patsubst demos/%.c, $(DEP_DIR)/%.d, $(filter demos/%.c, $(TOOL_SRC)))

# 包含自动生成的依赖文�?
-include $(CORE_DEP) $(TOOL_DEP)

# 静态库
LIB_NAME = libpivotmind.a

# ========== 编译规则 ==========

# 核心�?.c �?.o（依赖文件写�?dep/ 目录�?
$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(dir $(OBJ_DIR)/$*) $(dir $(DEP_DIR)/$*)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# 工具 .c �?.o
$(OBJ_DIR)/%.o: tools/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# 演示 .c �?.o
$(OBJ_DIR)/%.o: demos/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# 静态库
$(LIB_NAME): $(CORE_OBJ)
	ar rcs $@ $(CORE_OBJ)

# ========== 二进�?==========

$(BUILD_DIR)/digital_life: $(OBJ_DIR)/digital_life.o $(LIB_NAME)
	TMPDIR=/tmp $(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/digital_life.o -L. -lpivotmind $(LDFLAGS)

# v0.5.25 P2-6: gateway 已按模块拆分（gateway_http/system/learn/handlers + 主文件）
GATEWAY_OBJ = $(OBJ_DIR)/pivotmind_gateway.o $(OBJ_DIR)/gateway_http.o $(OBJ_DIR)/gateway_system.o $(OBJ_DIR)/gateway_learn.o $(OBJ_DIR)/gateway_handlers.o
$(BUILD_DIR)/pivotmind_gateway: $(GATEWAY_OBJ) $(LIB_NAME)
	TMPDIR=/tmp $(CC) $(CFLAGS) -o $@ $(GATEWAY_OBJ) -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/seed_builder: $(OBJ_DIR)/seed_builder.o $(LIB_NAME)
	TMPDIR=/tmp $(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/seed_builder.o -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/debug_seed: $(OBJ_DIR)/debug_seed.o $(LIB_NAME)
	TMPDIR=/tmp $(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/debug_seed.o -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_dialog: $(OBJ_DIR)/test_dialog.o $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/test_dialog.o -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/corpus_train: $(OBJ_DIR)/corpus_train.o $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/corpus_train.o -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/batch_learn: $(OBJ_DIR)/batch_learn.o $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/batch_learn.o -L. -lpivotmind $(LDFLAGS)

# 字符共现建边工具（从文本文件在现有拓扑上建边）
$(BUILD_DIR)/edge_builder: $(OBJ_DIR)/edge_builder.o $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/edge_builder.o -L. -lpivotmind $(LDFLAGS)

# 低内存版（禁掉周期性跨拓扑重建，适合 Zero 2W �?512MB 以下设备�?
$(BUILD_DIR)/batch_learn_lowmem: $(OBJ_DIR)/batch_learn.o $(LIB_NAME)
	$(CC) $(CFLAGS) -DLOW_MEM -o $@ $(OBJ_DIR)/batch_learn.o -L. -lpivotmind $(LDFLAGS)

# ·������ / ģ�幹������
$(BUILD_DIR)/template_build: $(OBJ_DIR)/template_build.o $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/template_build.o -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/path_analyze: $(OBJ_DIR)/path_analyze.o $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/path_analyze.o -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/compare_templates: $(OBJ_DIR)/compare_templates.o $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/compare_templates.o -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/eval_templates: $(OBJ_DIR)/eval_templates.o $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/eval_templates.o -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/qa_crawler: $(OBJ_DIR)/qa_crawler.o $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/qa_crawler.o -L. -lpivotmind $(LDFLAGS)

# ========== 构建目标 ==========

# 默认 (跳过 clean)
all: $(LIB_NAME) seed-builder debug-seed gateway

# Linux 一键构建全�?
linux: clean
	$(MAKE) all

# Debug 构建
debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" all

# AddressSanitizer 构建
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(ASAN_CFLAGS)" LDFLAGS="$(ASAN_LDFLAGS)" all

# ASan/UBSan 下运行核心单测（不依赖网络/终端的子集）。
# 泄漏检测：CI 通过 ASAN_OPTIONS=detect_leaks=1 打开（本配方不写死，保持本地默认）；
# Linux 下 ASan 默认 detect_leaks=1，故本地 `make asan-test` 同样会查泄漏。
ASAN_TEST_TARGETS = test-tensor test-tensor-broadcast test-model test-metrics test-memory-unit test-topology-unit test-dialog-unit test-learner-unit test-causal-unit test-forgetting-unit test-paths-unit
ASAN_TEST_BINS = $(BUILD_DIR)/test_tensor $(BUILD_DIR)/test_tensor_broadcast $(BUILD_DIR)/test_model $(BUILD_DIR)/test_metrics $(BUILD_DIR)/test_memory_unit $(BUILD_DIR)/test_topology_unit $(BUILD_DIR)/test_dialog_unit $(BUILD_DIR)/test_learner_unit $(BUILD_DIR)/test_causal_unit $(BUILD_DIR)/test_forgetting_unit $(BUILD_DIR)/test_paths_unit

asan-test:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(ASAN_CFLAGS)" LDFLAGS="$(ASAN_LDFLAGS)" $(ASAN_TEST_TARGETS)
	@echo ""
	@echo "== 运行 ASan/UBSan 单测 =="
	@FAILED=0; \
	for t in $(ASAN_TEST_BINS); do \
		name=$$(basename $$t); \
		if [ -x "$$t" ] && timeout 120 $$t > /dev/null 2>&1; then \
			echo "  PASS  $$name"; \
		else \
			echo "  FAIL  $$name"; FAILED=$$((FAILED+1)); \
		fi; \
	done; \
	echo ""; \
	[ $$FAILED -eq 0 ]

# 各个可执行文�?
digital-life: $(BUILD_DIR)/digital_life
gateway: $(BUILD_DIR)/pivotmind_gateway
seed-builder: $(BUILD_DIR)/seed_builder
debug-seed: $(BUILD_DIR)/debug_seed
test-dialog: $(BUILD_DIR)/test_dialog
corpus-train: $(BUILD_DIR)/corpus_train
batch-learn: $(BUILD_DIR)/batch_learn
batch-learn-lowmem: $(BUILD_DIR)/batch_learn_lowmem
template-build: $(BUILD_DIR)/template_build
path-analyze: $(BUILD_DIR)/path_analyze
compare-templates: $(BUILD_DIR)/compare_templates
eval-templates: $(BUILD_DIR)/eval_templates
qa-crawler: $(BUILD_DIR)/qa_crawler

edge-builder: $(BUILD_DIR)/edge_builder

# 运行
run: $(BUILD_DIR)/digital_life
	./$(BUILD_DIR)/digital_life

# 清理
clean:
	rm -rf build
	rm -f $(LIB_NAME)
	rm -f *.exe

# 安装
install:
	mkdir -p /usr/local/include/pivotmind
	cp include/*.h /usr/local/include/pivotmind/
	cp $(LIB_NAME) /usr/local/lib/

# ========== 单元测试 ==========

$(BUILD_DIR)/test_tensor: tests/unit/test_tensor.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_tensor.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_model: tests/unit/test_model.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_model.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_metrics: tests/unit/test_metrics.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_metrics.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_trainer: tests/unit/test_trainer.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_trainer.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_chinese: tests/unit/test_chinese.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_chinese.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_tensor_broadcast: tests/unit/test_tensor_broadcast.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_tensor_broadcast.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_web_fetch: tests/unit/test_web_fetch.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_web_fetch.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_dialog_unit: tests/unit/test_dialog.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_dialog.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_diffusion_unit: tests/unit/test_diffusion.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_diffusion.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_topology_unit: tests/unit/test_topology.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_topology.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_memory_unit: tests/unit/test_memory.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_memory.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_learner_unit: tests/unit/test_learner.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_learner.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_causal_unit: tests/unit/test_causal.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_causal.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_forgetting_unit: tests/unit/test_forgetting.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_forgetting.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_paths_unit: tests/unit/test_paths_unit.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_paths_unit.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_media_reader: tests/unit/test_media_reader.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_media_reader.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_visual_cortex: tests/unit/test_visual_cortex.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_visual_cortex.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_pure: tests/unit/test_pure.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_pure.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_search: tests/unit/test_search.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_search.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_pfe_unit: tests/test_pfe_unit.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_pfe_unit.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_regression: tests/unit/test_regression.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_regression.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_semantic_growth: tests/unit/test_semantic_growth.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_semantic_growth.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_integration: tests/integration/test_integration.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/integration/test_integration.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_cognitive_controller: tests/test_cognitive_controller.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_cognitive_controller.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_cognitive_full: tests/test_cognitive_full.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_cognitive_full.c -L. -lpivotmind $(LDFLAGS)

# 测试目标
test-tensor: $(BUILD_DIR)/test_tensor
test-model: $(BUILD_DIR)/test_model
test-metrics: $(BUILD_DIR)/test_metrics
test-trainer: $(BUILD_DIR)/test_trainer
test-chinese: $(BUILD_DIR)/test_chinese
test-tensor-broadcast: $(BUILD_DIR)/test_tensor_broadcast
test-web-fetch: $(BUILD_DIR)/test_web_fetch
test-dialog-unit: $(BUILD_DIR)/test_dialog_unit
test-diffusion-unit: $(BUILD_DIR)/test_diffusion_unit
test-topology-unit: $(BUILD_DIR)/test_topology_unit
test-memory-unit: $(BUILD_DIR)/test_memory_unit
test-learner-unit: $(BUILD_DIR)/test_learner_unit
test-causal-unit: $(BUILD_DIR)/test_causal_unit
test-forgetting-unit: $(BUILD_DIR)/test_forgetting_unit
test-paths-unit: $(BUILD_DIR)/test_paths_unit                # 路径 SSOT 契约单测（第 27 支）
test-media-reader: $(BUILD_DIR)/test_media_reader        # v0.5
test-visual-cortex: $(BUILD_DIR)/test_visual_cortex       # v0.5
test-pure: $(BUILD_DIR)/test_pure                           # 纯函数单元测试
test-search: $(BUILD_DIR)/test_search                        # 多引擎搜索测试
test-pfe-unit: $(BUILD_DIR)/test_pfe_unit                     # 前额叶执行器测试
test-regression: $(BUILD_DIR)/test_regression                    # 回归测试 (bug修复验证)
test-semantic-growth: $(BUILD_DIR)/test_semantic_growth           # 语义生长回归测试
test-integration: $(BUILD_DIR)/test_integration
test-cc: $(BUILD_DIR)/test_cognitive_controller
test-cc-full: $(BUILD_DIR)/test_cognitive_full

# 统一测试运行器
$(BUILD_DIR)/test_runner: tests/test_runner.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_runner.c -L. -lpivotmind $(LDFLAGS)

test-runner: $(BUILD_DIR)/test_runner

# 运行所有测试（编译 + 执行 + 汇总）
# 每个测试二进制退出码 0=通过, 非0=失败
# 说明（P2-6）：TEST_BINS 是 test: 的执行清单，必须与 test: 的构建前置逐项一致；
# TEST_FAST_BINS 刻意是它的子集（排除 test_chinese 控制台 smoke、test_trainer、
# test_web_fetch、test_tensor、test_cc、test_tensor_broadcast、test_semantic_growth、
# test_integration 等较慢/依赖终端或网络的项）。两列表口径显式维护，禁止有“定义了却没人跑”的目标。
TEST_BINS = $(BUILD_DIR)/test_tensor $(BUILD_DIR)/test_tensor_broadcast $(BUILD_DIR)/test_model $(BUILD_DIR)/test_metrics $(BUILD_DIR)/test_trainer $(BUILD_DIR)/test_chinese $(BUILD_DIR)/test_web_fetch $(BUILD_DIR)/test_dialog_unit $(BUILD_DIR)/test_diffusion_unit $(BUILD_DIR)/test_topology_unit $(BUILD_DIR)/test_memory_unit $(BUILD_DIR)/test_learner_unit $(BUILD_DIR)/test_causal_unit $(BUILD_DIR)/test_forgetting_unit $(BUILD_DIR)/test_media_reader $(BUILD_DIR)/test_visual_cortex $(BUILD_DIR)/test_pure $(BUILD_DIR)/test_search $(BUILD_DIR)/test_pfe_unit $(BUILD_DIR)/test_regression $(BUILD_DIR)/test_semantic_growth $(BUILD_DIR)/test_integration $(BUILD_DIR)/test_cognitive_controller $(BUILD_DIR)/test_cognitive_full $(BUILD_DIR)/test_paths_unit
TEST_FAST_BINS = $(BUILD_DIR)/test_model $(BUILD_DIR)/test_metrics $(BUILD_DIR)/test_visual_cortex $(BUILD_DIR)/test_dialog_unit $(BUILD_DIR)/test_diffusion_unit $(BUILD_DIR)/test_topology_unit $(BUILD_DIR)/test_memory_unit $(BUILD_DIR)/test_learner_unit $(BUILD_DIR)/test_causal_unit $(BUILD_DIR)/test_tensor_broadcast $(BUILD_DIR)/test_forgetting_unit $(BUILD_DIR)/test_media_reader $(BUILD_DIR)/test_pure $(BUILD_DIR)/test_search $(BUILD_DIR)/test_pfe_unit $(BUILD_DIR)/test_regression

test: test-cc-full test-tensor test-tensor-broadcast test-model test-metrics test-trainer test-chinese test-web-fetch test-dialog-unit test-diffusion-unit test-topology-unit test-memory-unit test-learner-unit test-causal-unit test-forgetting-unit test-media-reader test-visual-cortex test-pure test-search test-pfe-unit test-regression test-semantic-growth test-integration test-cc test-paths-unit
	@echo ""
	@echo "╔══════════════════════════════════════╗"
	@echo "║  运行单元测试...                     ║"
	@echo "╚══════════════════════════════════════╝"
	@echo ""
	@PASSED=0; FAILED=0; \
	echo "── 锁纪律静态检查 check-locks（秒级，规则见 src/funcword.c:479）──"; \
	if python3 tests/tools/check_lock_discipline.py; then \
		echo "  PASS  check-locks"; PASSED=$$((PASSED+1)); \
	else \
		echo "  FAIL  check-locks"; FAILED=$$((FAILED+1)); \
	fi; \
	echo "── 版本号一致性 check-version（秒级；真值源 include/pivotmind_version.h）──"; \
	if python3 tools/check_version_consistency.py; then \
		echo "  PASS  check-version"; PASSED=$$((PASSED+1)); \
	else \
		echo "  FAIL  check-version"; FAILED=$$((FAILED+1)); \
	fi; \
	for t in $(TEST_BINS); do \
		name=$$(basename $$t); \
		if [ -x "$$t" ]; then \
			if timeout 60 $$t > /dev/null 2>&1; then \
				echo "  PASS  $$name"; PASSED=$$((PASSED+1)); \
			else \
				echo "  FAIL  $$name"; FAILED=$$((FAILED+1)); \
			fi; \
		else \
			echo "  MISS  $$name (not built)"; FAILED=$$((FAILED+1)); \
		fi; \
	done; \
	echo ""; \
	echo "╔══════════════════════════════════════╗"; \
	printf "║  结果: %2d 通过, %2d 失败             ║\n" $$PASSED $$FAILED; \
	echo "╚══════════════════════════════════════╝"; \
	[ $$FAILED -eq 0 ]

# 快速测试（跳过慢速/网络测试）
test-fast: test-model test-metrics test-visual-cortex test-dialog-unit test-diffusion-unit test-topology-unit test-memory-unit test-learner-unit test-causal-unit test-tensor-broadcast test-forgetting-unit test-media-reader test-pure test-search test-pfe-unit test-regression
	@echo ""
	@echo "╔══════════════════════════════════════╗"
	@echo "║  运行快速测试...                     ║"
	@echo "╚══════════════════════════════════════╝"
	@echo ""
	@PASSED=0; FAILED=0; \
	for t in $(TEST_FAST_BINS); do \
		name=$$(basename $$t); \
		if [ -x "$$t" ]; then \
			if timeout 30 $$t > /dev/null 2>&1; then \
				echo "  PASS  $$name"; PASSED=$$((PASSED+1)); \
			else \
				echo "  FAIL  $$name"; FAILED=$$((FAILED+1)); \
			fi; \
		else \
			echo "  MISS  $$name (not built)"; FAILED=$$((FAILED+1)); \
		fi; \
	done; \
	echo ""; \
	echo "╔══════════════════════════════════════╗"; \
	printf "║  结果: %2d 通过, %2d 失败             ║\n" $$PASSED $$FAILED; \
	echo "╚══════════════════════════════════════╝"; \
	[ $$FAILED -eq 0 ]

# ========== 锁纪律静态检查（秒级；已接进 test:）============================
# 查「持 master 读锁期间是否调用了会取 master 写锁的函数」。
# glibc 的 pthread_rwlock_t 不支持同线程「读→写」升级 ⇒ 永久自死锁；它是纯自
# 死锁、无数据竞争，TSan/Helgrind 不会报，只会跟着一起挂住。规则出处是仓库自己的
# 纪律 src/funcword.c:「持读锁期间内部不得调用抢 master 写锁的函数」。
# 历史：修复前 src/brainstem.c:453 恰好 1 处命中（读锁在 :396），修复后 0 命中。
# 手工跑：make check-locks   或   python3 tests/tools/check_lock_discipline.py
check-locks:
	@python3 tests/tools/check_lock_discipline.py

# ========== 版本号单一真值源（SSOT；check-version 已接进 test:）============================
# 真值源唯一：include/pivotmind_version.h（PIVOTMIND_VERSION + MAJOR/MINOR/PATCH）。
# 活文档 README.md / README.zh-CN.md / ARCHITECTURE.md 里「声明当前版本」的 4 种锚点
# （shields.io badge URL、正文当前版本句、指标表版本行、架构文档抬头）一律由生成器改写，不得手写。
# ⛔ 历史不改：changelogs/** 与 CHANGELOG.md 的历史节不在扫描面内（陈旧断言只允许追加带日期的更正注记）。
# 手工跑：make check-version   或   python3 tools/check_version_consistency.py
#         make sync-version    或   python3 tools/sync_version_docs.py
sync-version:
	@python3 tools/sync_version_docs.py

check-version:
	@python3 tools/check_version_consistency.py

# ========== 长跑监护（opt-in，约 15 分钟；**刻意不进 test/test-fast**）=======
# 治「分钟级才现形的死」：那处自死锁只在 tick%600==0（约 11 分钟）才第一次执行到，
# 秒级单测结构上抓不住，所以必须长跑。断言 tick 越过 600 并持续增长到 >=900。
# ⚠️ 本目标会把 pivotmind_gateway **跑起来**，因此只能在允许运行产物的机器上跑；
#    脚本自带内存守卫，在受限验证机（Pi，<2G）上会直接拒绝执行。
#      make longrun GATEWAY=~/pm-lockguard/bin/pivotmind_gateway
#      make longrun GATEWAY=<gw> LONGRUN_ARGS="--port 8421 --minutes 17 --tick-target 900"
#    等价直跑： tests/longrun/run_longrun_guard.sh --bin <gw>
LONGRUN_ARGS ?=
longrun:
	@test -n "$(GATEWAY)" || { echo "用法: make longrun GATEWAY=<pivotmind_gateway 路径> [LONGRUN_ARGS=...]"; exit 2; }
	@tests/longrun/run_longrun_guard.sh --bin "$(GATEWAY)" $(LONGRUN_ARGS)

# ========== R3 · G-T1 批次契约探针（round 3 门禁接线）==========
# 交付物 tools/probe_batch_contract.c（方案附录 A）只编 src/thread_pool.c，
# **不进 libpivotmind.a**，不参与 all / test / asan-test。
# 🔴 构建坑（本仓已踩）：Makefile:61 的编译配方硬编码了 -MF，而 -MD -MP 在 CFLAGS 里；
#    任何 CFLAGS 覆盖都必须原样带上 -MD -MP，否则
#    cc1: error: to generate dependencies you must specify either '-M' or '-MM' 全盘失败。
#    本目标**刻意不覆盖 CFLAGS**，只用显式旗标且不生成 .d，从构造上规避该坑。
# 干跑（只打印命令、不构建）：make -n probe-batch-contract
# 实测（G-T1）：bash tests/round3/run_g_t1_probe.sh
PROBE_BATCH_CONTRACT = $(BUILD_DIR)/probe_batch_contract

$(PROBE_BATCH_CONTRACT): tools/probe_batch_contract.c src/thread_pool.c include/thread_pool.h
	$(CC) -O1 -g -pthread -fopenmp -Iinclude -o $@ tools/probe_batch_contract.c src/thread_pool.c -lm -lcurl -lssl -lcrypto -lz

probe-batch-contract: $(PROBE_BATCH_CONTRACT)

.PHONY: all linux debug asan asan-test digital-life gateway seed-builder debug-seed test-dialog corpus-train batch-learn batch-learn-lowmem template-build path-analyze compare-templates eval-templates qa-crawler run clean install test test-fast test-tensor test-model test-metrics test-trainer test-chinese test-tensor-broadcast test-web-fetch test-dialog-unit test-diffusion-unit test-topology-unit test-memory-unit test-learner-unit test-causal-unit test-forgetting-unit test-media-reader test-visual-cortex test-pure test-search test-pfe-unit test-regression test-semantic-growth test-integration test-cc test-cc-full test-runner probe-batch-contract check-locks sync-version check-version longrun test-paths-unit
