# C语言AI框架 Makefile — PivotMind

# 编译器
CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude -I. -Ilibs -std=gnu99 -fopenmp
LDFLAGS = -lm -pthread

# 输出目录
BUILD_DIR = build/bin
$(shell mkdir -p $(BUILD_DIR))

# 所有源文件
ALL_SRC = \
	src/tensor.c \
	src/tensor_pool.c \
	src/matrix_ops.c \
	src/gradient_ops.c \
	src/layer.c \
	src/layer_rnn.c \
	src/layer_rnn_backward.c \
	src/layer_lstm.c \
	src/layer_gru.c \
	src/model.c \
	src/optimizer.c \
	src/trainer.c \
	src/scheduler.c \
	src/chinese.c \
	src/vocab.c \
	src/pretrain.c \
	src/generative_model.c \
	src/seq2seq_train.c \
	src/sequence_generation.c \
	src/model_io.c \
	src/error.c \
	src/attention.c \
	src/quantization.c \
	src/pruning.c \
	src/metrics.c \
	src/context.c \
	src/memory_system.c \
	src/huarong_topology.c \
	src/string_pool.c \
	src/multi_topology.c \
	src/node_hash.c \
	src/associative_reasoning.c \
	src/utf8_tokenizer.c \
	src/dialog_system.c \
	src/active_learner.c \
	src/autonomic_learner.c \
	src/node_importance.c \
	src/topology_growth.c \
	src/catastrophic_forgetting.c \
	src/memory_consolidation.c \
	src/concept_abstraction.c \
	src/causal_reasoning.c \
	src/cognitive_params.c \
	src/cognitive_controller.c \
	src/memory_arena.c \
	src/ui.c \
	src/concept_processor.c \
	src/thread_pool.c \
	src/topo_snapshot.c

# 静态库
LIB_OBJ = $(ALL_SRC:.c=.o)
LIB_NAME = libpivotmind.a

# ========== 通用编译规则 ==========
BUILD_BIN = $(BUILD_DIR)/digital_life $(BUILD_DIR)/seed_builder $(BUILD_DIR)/debug_seed

# 单个.c -> .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 链接任意主程序
define binary_rule
$(BUILD_DIR)/$(1): $(2) $$(ALL_SRC)
	$$(CC) $$(CFLAGS) -D_USE_MATH_DEFINES -pthread -o $$@ $(2) $$(ALL_SRC) -lm
endef

$(eval $(call binary_rule,digital_life,demos/digital_life.c))
$(eval $(call binary_rule,seed_builder,tools/seed_builder.c))
$(eval $(call binary_rule,debug_seed,tools/debug_seed.c))
$(eval $(call binary_rule,seq2seq_trainer,tools/seq2seq_trainer.c))

# ========== 构建目标 ==========

# 默认 (跳过 clean 以加快开发迭代; 用 make clean 显式清理)
all: lib digital-life seed-builder debug-seed

# Linux 一键构建全部 (清理后)
linux: clean
	$(MAKE) all

# 构建静态库
lib: $(LIB_OBJ)
	ar rcs $(LIB_NAME) $(LIB_OBJ)

# 各个可执行文件
digital-life: $(BUILD_DIR)/digital_life
seed-builder: $(BUILD_DIR)/seed_builder
debug-seed: $(BUILD_DIR)/debug_seed

# 运行
run: $(BUILD_DIR)/digital_life
	./$(BUILD_DIR)/digital_life

# 清理
clean:
	rm -rf build
	rm -f $(LIB_OBJ) $(LIB_NAME)
	rm -f *.exe

# 安装
install:
	mkdir -p /usr/local/include/pivotmind
	cp include/*.h /usr/local/include/pivotmind/
	cp $(LIB_NAME) /usr/local/lib/

.PHONY: all linux lib digital-life seed-builder debug-seed run clean install
