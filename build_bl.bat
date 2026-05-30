@echo off
cd /d D:\work\玄枢-pivotmind
mkdir build\obj build\dep build\bin 2>nul
echo === Compiling core sources ===
for %%f in (src\*.c) do (
    echo %%f
    gcc -Wall -Wextra -O2 -Iinclude -I. -Ilibs -std=gnu99 -fopenmp -pthread -MD -MP -D_USE_MATH_DEFINES -MFbuild\dep\%%~nf.d -c %%f -o build\obj\%%~nf.o
    if errorlevel 1 echo FAILED: %%f & exit /b 1
)
echo === Compiling batch_learn.c ===
gcc -Wall -Wextra -O2 -Iinclude -I. -Ilibs -std=gnu99 -fopenmp -pthread -MD -MP -D_USE_MATH_DEFINES -MFbuild\dep\batch_learn.d -c tools\batch_learn.c -o build\obj\batch_learn.o
if errorlevel 1 echo FAILED: batch_learn.c & exit /b 1
echo === Creating static library ===
ar rcs libpivotmind.a build\obj\active_learner.o build\obj\associative_reasoning.o build\obj\attention.o build\obj\autonomic_learner.o build\obj\background_clock.o build\obj\bptt_learner.o build\obj\catastrophic_forgetting.o build\obj\causal_reasoning.o build\obj\chinese.o build\obj\cognitive_controller.o build\obj\cognitive_params.o build\obj\concept_abstraction.o build\obj\concept_processor.o build\obj\context.o build\obj\cross_edge_io.o build\obj\dialog_generate.o build\obj\dialog_system.o build\obj\enhanced_generator.o build\obj\error.o build\obj\feature_io.o build\obj\feature_learn.o build\obj\feature_pretrain.o build\obj\generative_model.o build\obj\gradient_ops.o build\obj\huarong_topology.o build\obj\layer.o build\obj\layer_gru.o build\obj\layer_lstm.o build\obj\layer_rnn.o build\obj\layer_rnn_backward.o build\obj\matrix_ops.o build\obj\memory_arena.o build\obj\memory_consolidation.o build\obj\memory_system.o build\obj\metrics.o build\obj\model.o build\obj\model_io.o build\obj\multi_topology.o build\obj\network_tool.o build\obj\node_hash.o build\obj\node_importance.o build\obj\optimizer.o build\obj\path_encoding.o build\obj\pretrain.o build\obj\pruning.o build\obj\quantization.o build\obj\scheduler.o build\obj\string_pool.o build\obj\template_builder.o build\obj\tensor.o build\obj\tensor_pool.o build\obj\thread_pool.o build\obj\topology_growth.o build\obj\topo_eval.o build\obj\topo_snapshot.o build\obj\trainer.o build\obj\ui.o build\obj\utf8_tokenizer.o build\obj\vocab.o
if errorlevel 1 echo FAILED: ar & exit /b 1
echo === Linking batch_learn.exe ===
gcc -Wall -Wextra -O2 -Iinclude -I. -Ilibs -std=gnu99 -fopenmp -pthread -MD -MP -D_USE_MATH_DEFINES -o build\bin\batch_learn.exe build\obj\batch_learn.o -L. -lpivotmind -lm
if errorlevel 1 echo FAILED: link & exit /b 1
echo === DONE ===
dir build\bin\batch_learn.exe
