# C语言AI框架 Makefile �?PivotMind
#
# 增量编译: 每个 .c 独立编译�?.o，二进制仅链接所需 .o
# 改一个源文件 �?只重新编译该文件 �?重新链接相关二进�?
# 自动依赖追踪: -MD -MP 生成 .d 文件，头文件变化时自动重编译

# 编译器
CC = gcc
CFLAGS = -pipe -Wall -Wextra -O2 -Iinclude -I. -Ilibs -std=gnu99 -fopenmp -pthread -MD -MP -D_USE_MATH_DEFINES -D_FORTIFY_SOURCE=2 -flto -DHAS_OPENSSL
LDFLAGS = -lm -lssl -lcrypto -lcurl -lz -flto
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -Iinclude -I. -Ilibs -std=gnu99 -fopenmp -pthread -MD -MP -DDEBUG -D_FORTIFY_SOURCE=2
ASAN_CFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -Iinclude -I. -Ilibs -std=gnu99 -fopenmp -pthread -MD -MP -DDEBUG
ASAN_LDFLAGS = -fsanitize=address,undefined -lm -lcurl

# 输出目录
BUILD_DIR = build/bin
OBJ_DIR = build/obj
DEP_DIR = build/dep
$(shell mkdir -p $(BUILD_DIR) $(OBJ_DIR) $(DEP_DIR))

export TMPDIR = /tmp

# 源文件（通配自动发现�?
CORE_SRC = $(wildcard src/*.c)
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
	$(CC) $(CFLAGS) -MF $(DEP_DIR)/$*.d -c $< -o $@

# 工具 .c �?.o
$(OBJ_DIR)/%.o: tools/%.c
	$(CC) $(CFLAGS) -MF $(DEP_DIR)/$*.d -c $< -o $@

# 演示 .c �?.o
$(OBJ_DIR)/%.o: demos/%.c
	$(CC) $(CFLAGS) -MF $(DEP_DIR)/$*.d -c $< -o $@

# 静态库
$(LIB_NAME): $(CORE_OBJ)
	ar rcs $@ $(CORE_OBJ)

# ========== 二进�?==========

$(BUILD_DIR)/digital_life: $(OBJ_DIR)/digital_life.o $(LIB_NAME)
	TMPDIR=/tmp $(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/digital_life.o -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/pivotmind_gateway: $(OBJ_DIR)/pivotmind_gateway.o $(LIB_NAME)
	TMPDIR=/tmp $(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/pivotmind_gateway.o -L. -lpivotmind $(LDFLAGS)

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

$(BUILD_DIR)/test_io: tests/unit/test_io.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_io.c -L. -lpivotmind $(LDFLAGS)

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

$(BUILD_DIR)/test_media_reader: tests/unit/test_media_reader.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_media_reader.c -L. -lpivotmind $(LDFLAGS)

$(BUILD_DIR)/test_visual_cortex: tests/unit/test_visual_cortex.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/unit/test_visual_cortex.c -L. -lpivotmind $(LDFLAGS)

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
test-io: $(BUILD_DIR)/test_io
test-web-fetch: $(BUILD_DIR)/test_web_fetch
test-dialog-unit: $(BUILD_DIR)/test_dialog_unit
test-diffusion-unit: $(BUILD_DIR)/test_diffusion_unit
test-topology-unit: $(BUILD_DIR)/test_topology_unit
test-memory-unit: $(BUILD_DIR)/test_memory_unit
test-learner-unit: $(BUILD_DIR)/test_learner_unit
test-causal-unit: $(BUILD_DIR)/test_causal_unit
test-forgetting-unit: $(BUILD_DIR)/test_forgetting_unit
test-media-reader: $(BUILD_DIR)/test_media_reader        # v0.5
test-visual-cortex: $(BUILD_DIR)/test_visual_cortex       # v0.5
test-integration: $(BUILD_DIR)/test_integration
test-cc: $(BUILD_DIR)/test_cognitive_controller
test-cc-full: $(BUILD_DIR)/test_cognitive_full

# 统一测试运行器
$(BUILD_DIR)/test_runner: tests/test_runner.c $(LIB_NAME)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_runner.c -L. -lpivotmind $(LDFLAGS)

test-runner: $(BUILD_DIR)/test_runner

# 运行所有测试
test: test-tensor test-model test-metrics test-trainer test-chinese test-io test-cc test-web-fetch test-dialog-unit test-diffusion-unit test-topology-unit test-memory-unit test-learner-unit test-causal-unit test-forgetting-unit test-media-reader test-visual-cortex
	@echo "╔══════════════════════════════════════╗"
	@echo "║  所有单元测试已完成                  ║"
	@echo "╚══════════════════════════════════════╝"

.PHONY: all linux debug asan digital-life gateway seed-builder debug-seed test-dialog corpus-train batch-learn batch-learn-lowmem template-build path-analyze compare-templates eval-templates run clean install test test-tensor test-model test-metrics test-trainer test-chinese test-io test-web-fetch test-dialog-unit test-diffusion-unit test-topology-unit test-memory-unit test-learner-unit test-causal-unit test-forgetting-unit test-media-reader test-visual-cortex test-integration test-cc test-cc-full test-runner
