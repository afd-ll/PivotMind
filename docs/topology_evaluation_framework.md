# 玄枢拓扑训练评估指标体系 — 完整设计

## 概述

玄枢 (PivotMind) 是纯 C 认知 AI 框架，核心推理机制是**多拓扑走边**（贪心/跨拓扑），
不是神经网络前馈。因此评估体系不能依赖"损失函数"，而应从拓扑结构、走边行为、
记忆巩固三个维度综合判断。本设计遵循**最小 C 代码改动原则**，优先复用已有结构
（`MasterTopology`、`SubTopology`、`ReasoningNode`、`EdgeWeightDual`）。

---

## 一、训练时评估指标（Train-time Metrics）

### T1. 边增长率 (Edge Growth Rate)
```c
// 定义: 每轮训练新增边数 / 已有边数
// 用途: 判断拓扑是否在合理扩张
float topo_edge_growth_rate(MasterTopology* master);
```
- 健康范围：node_count 扩容期 5%-20%/epoch，稳定期 <5%/epoch
- 若 <0.5%/epoch → 拓扑可能僵化（learn nothing）
- 若 >30%/epoch → 过度连接，需检查是否记忆而非理解

### T2. 置信度分布熵 (Confidence Distribution Entropy)
```c
// 定义: 所有边置信度的分布信息熵
// H = -Σ(p_i * log(p_i))  其中 p_i = 置信度在区间 i 的比例
float topo_confidence_entropy(MasterTopology* master, int bins);
```
- 健康范围：0.6~0.9（正态分布在 [0.3, 0.8] 区间）
- 熵太低 (<0.3) → 置信度极化（全高或全低），需检查过/欠拟合
- 概念：**拓扑困惑度 (Topological Perplexity)** = 2^H (类比 NLP perplexity)

### T3. 节点覆盖度 (Node Coverage)
```c
// 定义: 在最近 N 轮训练中被激活的节点占比
float topo_node_coverage(MasterTopology* master, int recent_epochs);
```
- 健康范围：70%-95%
- 若 <50% → 大量节点"僵尸化"，从未使用
- 若 =100% → 可能过度泛化，所有节点都弱连接

### T4. 边密度 (Edge Density)
```c
// 定义: 实际边数 / 完全图边数 = total_edges / (n*(n-1)/2)
float topo_edge_density(MasterTopology* master, int topo_id);
```
- 稀疏拓扑：0.01%-0.1%（知识图谱典型值）
- 若 >1% → 边过多，信息冗余
- 若 <0.001% → 严重稀疏，推理可能断裂

### T5. 同现置信度涨落 (Co-occurrence Confidence Delta)
```c
// 定义: batch_learn 前后同一节点对置信度的变化量
// 每批 QA 对完成后计算
float topo_conf_delta_avg(MasterTopology* master, const QA_Pair* batch, int batch_size);
```
- 每 QA 对应该产生置信度上升（>0 增量）
- 若大批 QA 对产生 0 增量 → 拓扑已饱和或学习失效

---

## 二、推理时评估指标（Inference-time Metrics）

### I1. 路径步长分布 (Path Step Distribution)
```c
// 定义: 走边路径长度的统计分布 (均值 / 中位数 / 标准差)
PathStepStats topo_path_step_stats(MasterTopology* master, int num_trials);
```
- 对话推理典型值：3-8 步
- 均值 <2 → 推理太浅，只在相邻节点跳
- 均值 >12 → 推理发散，可能陷入循环或噪音路径
- 标准差大 >5 → 路径长度不稳定，推理质量波动大

### I2. 跳转成功率 (Jump Success Rate) — 核心指标！
```c
// 定义: 成功激活到目标节点的走边次数 / 总尝试次数
float topo_jump_success_rate(MasterTopology* master);
```
- 使用的是 MasterTopology 已有的 `successful_inferences / total_inferences`
- 健康范围：60%-90%
- <40% → 拓扑学习不足，边置信度过低
- >95% → 可能过度拟合，每次都能跳到"标准答案"

### I3. 联想多样性 (Associative Diversity)
```c
// 定义: 同一输入触发不同推理路径的比率
// 对同一输入做 N 次走边，统计路径的 Jaccard 距离
float topo_associative_diversity(MasterTopology* master, 
                                  const char* input, int num_samples);
```
- **拓扑联想多样度 (Topological Associative Entropy, TAE)**
  - 公式: TAE = -Σ(p_i * log(p_i)), p_i = 某节点出现在路径中的频率
- 健康范围：0.4-0.8（取决于问题确定性）
- 事实性问题应低（0.1-0.3），创造性问题应高（0.5-0.8）
- TAE=0 → 完全确定路径（可能死记硬背）
- TAE>0.9 → 路径随机发散

### I4. 语义连贯性评分 (Semantic Coherence Score)
```c
// 定义: 路径中相邻概念的字面重叠度 / 特征向量余弦相似度
float topo_semantic_coherence(int* path, int path_len, SubTopology* sub);
```
- 使用已有的 `calculate_semantic_similarity` 或 `cosine_similarity`
- 逐对检查相邻节点：score = avg(cosine_sim(path[i], path[i+1]))
- <0.1 → 推理跳跃不合理（可能是噪音干扰）
- >0.7 → 推理过于保守（只跳语义极近的词）

### I5. 跨拓扑跳转比 (Cross-topo Jump Ratio)
```c
// 定义: 跨拓扑跳转次数 / 总跳转次数
float topo_cross_ratio(int* path_topos, int path_len);
```
- 健康范围 5%-25%
- =0% → 各拓扑独立运作，没有跨域联想
- >50% → 跨拓扑过于频繁，可能语义不稳

### I6. 推理稳定度 (Inference Stability)
```c
// 定义: 对同一输入的多次推理，路径的重复率
float topo_inference_stability(MasterTopology* master, 
                                const char* input, int num_repeats);
```
- 用编辑距离或最长公共子序列(LCS)衡量路径相似度
- 高稳定度（>80%重复）→ 知识可靠
- 低稳定度（<30%重复）→ 知识碎片化

---

## 三、长期评估指标（Long-term Metrics）

### L1. 遗忘率 (Forgetting Rate)
```c
// 定义: 每轮遗忘清理掉的边占全部边的比例
float topo_forgetting_rate(PruneStats* before, PruneStats* after);
```
- 健康范围：1%-10%/轮
- =0% → 遗忘机制无效，拓扑持续膨胀
- >20% → 遗忘过于激进，可能损失有效知识

### L2. 知识泛化度 (Knowledge Generalization) — 防过拟合核心
```c
// 定义: 训练集 vs 验证集（未见QA对）的路径成功率差值
// acc_train - acc_val 越大表示过拟合越严重
float topo_knowledge_generalization(MasterTopology* master,
                                     QA_Pair* train_set, int train_n,
                                     QA_Pair* val_set, int val_n);
```
- 过拟合检测阈值：
  - acc_diff < 5% → 泛化良好
  - 5% < acc_diff < 15% → 轻微过拟合
  - acc_diff > 15% → 严重过拟合，机械记忆
- 实现方式：对验证集走边，看能否从问句概念走到答句概念

### L3. 记忆迁移效率 (Memory Transfer Efficiency)
```c
// 定义: 临时记忆升迁为永久记忆的比率
float topo_memory_promotion_rate(MemorySystem* memory, int since_hours);
```
- 利用已有的三级记忆系统
- 健康范围：5%-15%/天
- 过低 <2% → 记忆巩固不足，学到的东西都没存住
- 过高 >30% → 任何信息都变永久记忆，缺少筛选

### L4. 知识域间干扰 (Inter-domain Interference)
```c
// 定义: 使用已有 catastrophic_forgetting 的 compute_domain_interference
float topo_domain_interference(MasterTopology* master,
                                KnowledgeDomain* d1, KnowledgeDomain* d2);
```
- 健康范围：<0.3
- >0.5 → 跨域干扰严重，不同知识之间产生混乱

---

## 四、在线运行时评估指标（Online / Dialogue Runtime）

### R1. 每轮对话学习效率 (Per-turn Learning Efficiency)
```c
// 定义: 单轮对话后置信度变化量 / 新创建的边数
// 越高说明对话学习"性价比"越好
float online_learning_efficiency(AutonomicState* state_before,
                                  AutonomicState* state_after);
```
- 在 `autonomic_learn_from_dialog` 调用前/后快照
- edge_delta > 0 且 conf_delta > 0 → 正常
- edge_delta > 0 但 conf_delta ≈ 0 → 创建了边但没学到（低质量交互）

### R2. 响应相关度 (Response Relevance)
```c
// 定义: 生成的回复路径中，与用户输入共现的概念占比
float online_response_relevance(int* path, int path_len,
                                 const char* user_input, SubTopology* sub);
```
- 实时计算对话中的推理路径
- >0.6 → 回答相关
- <0.3 → 离题（hallucination）

### R3. 探索 vs 利用比率 (Explore-Exploit Ratio)
```c
// 定义: 走边时选择新路径 vs 已有路径的概率比
// 利用 CognitiveState 中的 explore_rate
float online_explore_exploit_ratio(CognitiveState* state);
```
- 直接复用 `compute_explore_exploit_balance`
- 系统刚启动时 explore > 0.5
- 成熟系统 explore 应趋于 0.1-0.3

### R4. 知识质量评分实时跟踪 (Knowledge Quality Tracking)
```c
// 定义: 对话过程中 knowledge_quality 的时间序列
// 每次推理后记录
KnowledgeQualityTrack online_kq_track(MasterTopology* master);
```
- 记录每个推理的:
  - path_length
  - avg_confidence_in_path
  - cross_topo_count
  - successful_inference (bool)
- 滑动窗口（最近 N 轮）计算综合质量

### R5. 拓扑健康度仪表盘 (Topology Health Dashboard)
```c
// 定义: 综合上述指标，给出 0-100 的健康分
float topo_health_score(MasterTopology* master);
```
- 加权公式（建议权重）:
  - 跳转成功率 25%
  - 置信度分布熵 20%
  - 路径步长合理性 15%
  - 节点覆盖度 15%
  - 遗忘率 10%
  - 跨拓扑比 10%
  - 联想多样性 5%
- 等级: >80=健康, 60-80=注意, 40-60=警告, <40=危险

---

## 五、实现方案与 C 代码改动

### 总体架构

```
src/topo_eval.c          ← 新增: 所有拓扑评估指标实现
include/topo_eval.h      ← 新增: 头文件
src/metrics.c            ← 复用现有实现（不修改）
tests/unit/test_topo_eval.c  ← 新增: 单元测试
```

### 新增文件清单

| 文件 | 用途 | 预估行数 |
|------|------|---------|
| `include/topo_eval.h` | 拓扑评估指标头文件 | ~200 |
| `src/topo_eval.c` | 所有指标实现 | ~1200 |
| `tests/unit/test_topo_eval.c` | 单元测试 | ~400 |

### 对已有代码零改动

本设计**完全不修改**已有文件：

- `multi_topology.h/c` — 数据完全从已有结构读取
- `metrics.h/c` — 复用 Tensor 基础计算函数
- `autonomic_learner.c` — 运行时调用新增评估接口即可
- `catastrophic_forgetting.c` — 复用其 FisherInfo / domain_interference
- `memory_system.c` — 复用其 consolidation 统计

### 最小改动方案（可选，若需集成到现有工具）

仅需在 `batch_learn.c` 末尾加一行：
```c
print_topo_evaluation(master);  // 打印完整评估报告
```
在 `master_save_state` 中无侵入地保存评估快照。

---

## 六、关键问题的回答

### Q1: 拓扑系统没有统一的"损失"概念如何处理？
**答**：构建指标体系从四个维度综合判断——结构健康度（T1-T4）、推理质量（I1-I6）、
长期稳定性（L1-L4）、在线响应（R1-R5）。综合健康分替代 loss 作为单值监控指标。

### Q2: 拓扑困惑度或联想多样性？
**答**：定义了两种：
- **拓扑困惑度 (Topological Perplexity)** = 2^(置信度分布熵)。量化拓扑的"知识确定度"。
- **拓扑联想熵 (Topological Associative Entropy, TAE)** = 路径多样性的信息熵。
  量化同一输入的不同走边路径多样性。

### Q3: 在线运行时如何评估学习效果？
**答**：每轮对话前后快照 AutonomicState，计算 `online_learning_efficiency`。
结合 R1-R5 的实时流式指标。关键是**滑动窗口统计**，避免单次噪声。

### Q4: 如何检测过拟合？
**答**：核心方法是 L2（知识泛化度）——对比训练集与验证集上的跳转成功率差值。
辅以 T2（置信度分布熵——过拟合时熵极低，所有边置信度趋近 1.0）
和 I6（推理稳定度——过拟合时每次走完全相同的路径）。
