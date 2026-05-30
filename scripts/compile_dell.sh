#!/usr/bin/bash
export PATH=/mingw64/bin:/usr/bin:/bin
cd /d/PivotMind_src || exit 1

mkdir -p build/obj build/bin

SRCS="
src/tensor.c src/tensor_pool.c src/matrix_ops.c src/gradient_ops.c
src/layer.c src/layer_rnn.c src/layer_rnn_backward.c src/layer_lstm.c
src/layer_gru.c src/model.c src/optimizer.c src/trainer.c src/scheduler.c
src/chinese.c src/vocab.c src/pretrain.c src/generative_model.c src/model_io.c
src/error.c src/attention.c src/quantization.c src/pruning.c src/metrics.c
src/context.c src/memory_system.c src/huarong_topology.c src/string_pool.c
src/multi_topology.c src/node_hash.c src/associative_reasoning.c
src/utf8_tokenizer.c src/dialog_system.c src/active_learner.c
src/autonomic_learner.c src/node_importance.c src/topology_growth.c
src/catastrophic_forgetting.c src/memory_consolidation.c
src/concept_abstraction.c src/causal_reasoning.c src/cognitive_params.c
src/cognitive_controller.c src/memory_arena.c src/ui.c
src/concept_processor.c src/thread_pool.c src/topo_snapshot.c
src/feature_io.c src/cross_edge_io.c src/topo_eval.c
"

for f in $SRCS; do
    base=$(basename "$f" .c)
    echo "  CC $base"
    gcc -Wall -Wextra -O2 -Iinclude -I. -Ilibs -std=gnu99 -fopenmp -c "$f" -o "build/obj/$base.o" 2>&1 || exit 1
done

echo "Building static library..."
ar rcs libpivotmind.a build/obj/*.o

echo "Linking batch_learn..."
gcc -std=gnu99 -O2 -Iinclude -I. -Ilibs -D_USE_MATH_DEFINES -pthread -fopenmp \
    -o build/bin/batch_learn tools/batch_learn.c -L. -lpivotmind -lm -pthread 2>&1

echo "DONE: build/bin/batch_learn ready"
ls -la build/bin/batch_learn 2>/dev/null || echo "BUILD FAILED"
