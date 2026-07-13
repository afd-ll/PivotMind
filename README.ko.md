<div align="center">

# 玄枢 PivotMind

### A Brain-Inspired Semantic Association Engine
**순수 C · AI 프레임워크 의존성 제로 · ARM 임베디드 보드에서 실행**

[English](README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Русский](README.ru.md)

[![Version](https://img.shields.io/badge/version-v0.5.5-blue.svg)](changelogs/)
[![License](https://img.shields.io/badge/license-Apache%202.0-green.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C-99%2B-orange.svg)](https://en.wikipedia.org/wiki/C99)
[![Platform](https://img.shields.io/badge/ARM-RK3399%20%7C%20x86__64-lightgrey.svg)](#running-on)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-success.svg)](#quick-start)

> Intelligence is not a stack of matrix multiplications,
> but ripples of activation spreading through a reasoning network.

</div>

---

## PivotMind 소개

PivotMind는 [TraceWisdomNetwork](#tracewisdomnetwork) +
[Hebbian Learning](#core-mechanisms) +
[Multi-Layer Diffusion Reasoning](#multi-layer-diffusion-engine)을 기반으로 한
**뇌에서 영감을 받은 인지 엔진**입니다.
Transformer 없음. 임베딩 벡터 없음. 역전파 없음.
노드, 엣지, 활성화, 감쇠 — 끊임없이 돌아가는 백그라운드 클록에 의해 구동됩니다.

**현재 버전: v0.5.5** — 14개 뇌 영역/서브시스템(전부 구현 완료), 창발적 품사 시스템, 병렬 다중 학습기, PFE 추론, 512차원 특징 벡터.

**코드 규모: 85개 소스 파일(~48,600행 C) + 89개 헤더(~12,600행) + 도구/테스트/데모(~13,000행) = 약 74,000행.**

### TraceWisdomNetwork

각 개념은 노드입니다. 동시출현이 엣지를 생성합니다. 엣지는 세 가지 속성을 갖습니다:
**가중치 × 신뢰도 × 동기적 편향**.
11개 하위 토폴로지(어휘 / 의미 / 감정 / 구문 / 문맥 / 도메인 / 화용 / 문화 / 개념 / 마스터 / 템플릿)
각각이 독립적인 추론 네트워크를 형성하고, O(1) 인접 리스트로 토폴로지 간 링크를 통해 상호 연결됩니다.
활성화는 계층 간 동시에 확산되며, 경쟁을 통해 승자가 출력으로 선택됩니다.

### Why This Approach

| 기존 LLM                       | PivotMind                                                  |
|--------------------------------|------------------------------------------------------------|
| 토큰 예측, 상태 비보존          | 노드 활성화, 지속적 내부 상태                              |
| 경사하강 기반 오프라인 배치 학습 | Hebbian 온라인 + Skip-gram 사전학습                        |
| 단일 임베딩 공간                | 11개 독립 하위 토폴로지 + 512차원 특징 벡터                |
| 신경망 블랙박스                 | 명시적 노드-엣지 경로, 완전 추적 가능                      |
| GPU + 대용량 VRAM 필요          | pthread + OpenMP만, ARM 임베디드에서 실행                   |
| 추론과 학습이 분리              | 대화 자체가 곧 학습                                        |
| 생리적 자각 없음                | 내수용적 자기 모니터링, 3단계 건강 대응                     |
| 학습 후 동결                    | 24/7 지속적 백그라운드 학습                                  |

---

## Brain Region Architecture

PivotMind는 포유류 대뇌 피질의 기능적 분화를 모델링합니다 — 14개 뇌 영역/서브시스템, 각자 전담하는 책임이 있으며, Thalamus 신호 버스를 통해 통신합니다. **13개 영역 모두 완전히 구현되었으며 스텁 코드가 없습니다.**

```
                          ┌──────────────────────┐
                          │   Prefrontal Cortex    │ ← Dialog / Decision Entry
                          │  + Prefrontal Exec PFE │ ← 6-Mode Reasoning Orchestrator
                          └──────────┬───────────┘
                                     │ Signal Bus
        ┌────────┬────────┬─────────┼─────────┬────────┬────────┬────────┐
        ▼        ▼        ▼         ▼         ▼        ▼        ▼        ▼
   ┌────────┐┌──────┐┌──────┐┌──────────┐┌──────┐┌──────┐┌──────┐┌──────────┐
   │Hippo-  ││ DMN  ││Amyg- ││ Perception││Broca ││Cere- ││Brain-││Hypothal- │
   │campus  ││      ││dala  ││  Cortex   ││      ││bellum││stem  ││  amus    │
   │Memory  ││Dream ││Emo-  ││  Web     ││Template││BPTT  ││Circ- ││ Drives   │
   │Consol. ││      ││tion  ││  Search  ││Builder││Tuner ││adian ││          │
   └────────┘└──────┘└──────┘└──────────┘└──────┘└──────┘└──────┘└──────────┘
                                     │
                          ┌──────────┴──────────┐
                          │    Thalamus           │ ← Signal Bus + Resource Gate
                          └─────────────────────┘
```

| Brain Region       | File                       | Lines | Function                                                       |
|--------------------|----------------------------|-------|----------------------------------------------------------------|
| **Prefrontal**     | `prefrontal.c`             | 132   | Dialog generation, diffusion → ACC adaptive gating              |
| **Prefrontal Exec**| `prefrontal_executive.c`   | 1,502 | 6-mode reasoning orchestration, task decomposition             |
| **Hippocampus**    | `hippocampus.c`            | 135   | Memory consolidation, QA replay, perception coupling            |
| **DMN**            | `dmn.c`                    | 46    | Default Mode Network: dream association, idle exploration      |
| **Amygdala**       | `amygdala.c`               | 97    | Emotional valence sampling, explore / exploit balance          |
| **Perception**     | `perception.c`             | 838   | Web search (Sogou+Bing dual provider), article_reader pipeline |
| **Broca's Area**   | `broca.c`                  | 56    | Template auto-building and decay scheduling                    |
| **Cerebellum**     | `cerebellum.c`             | 80    | BPTT fine-tuning, CPU/memory resource protection               |
| **Hypothalamus**   | `hypothalamus.c`           | 149   | 4D drive regulation (curiosity/acquisition/social/comfort)     |
| **Thalamus**       | `thalamus.c`               | 540   | Signal bus, resource gating, inter-region routing, tool slots  |
| **Brainstem**      | `brainstem.c`              | 613   | Circadian heartbeat, activation decay, spontaneous activation  |
| **Cingulate (ACC)**| `cingulate.c`              | 223   | 4D sequence evaluation (semantic + template + emotion + length)|
| **IdeaArena**      | `idea_arena.c`             | 722   | Multi-candidate 5D competition, lateral inhibition, dopamine   |
| **Reticular**      | `reticular.c`              | 133   | Arousal/alertness level regulation                             |

---

## Core Mechanisms

### Multi-Layer Diffusion Engine

Input is tokenized via sliding window, then diffuses simultaneously across layers:

- **Vocabulary** — direct literal matching, fast recall
- **Semantic** — cross-topology association across 11 sub-topologies
- **Template** — syntactic pattern recognition, guiding connector insertion
- **Emotion** — valence × arousal weighting, modulating candidate priority

**v0.5.5 개선**: 기능어 필터링 — `is_function_word()`가 ~130개 중영 기능어를 확인하고, 파이프라인 3단계(활성 세트 업데이트, 가중 점수, 출력)에서 필터링하여 고연결성 기능어가 출력을 장악하는 것을 방지합니다. 측방 억제로 내용어 다양성을 보장합니다.

### Reasoning Orchestration (PFE)

The Prefrontal Executive automatically assesses question complexity and matches one of 6 reasoning modes:

| Mode       | Trigger Keywords        | Strategy                              |
|------------|-------------------------|---------------------------------------|
| DIRECT     | default                 | Single diffusion association          |
| DECOMPOSE  | why / because           | Definition → causality → synthesis     |
| COMPARE    | compare / difference    | Attribute extraction → contrast       |
| HOWTO      | how to                  | Preconditions → step sequence         |
| ABDUCE     | what if / assume        | Baseline → chain reaction             |
| ANALOGY    | analogy / similar       | Structural mapping                    |

Subgoals are recursively decomposed (configurable depth). Conflict detection + IdeaArena 5D competition
selects optimal paths, producing explainable reasoning chains. Strategy weights support EMA self-learning with persistence.

### Emergent POS System **NEW v0.4.3** *(자세한 내용은 영문/중문 버전 참조)*

Abandons hardcoded POS dictionaries. Humans provide only 3-5 seed anchor words per class (~50 words total). The system initializes anchor centroids from seed words' 512-dim Hebbian feature vectors. At runtime, new words are classified via cosine similarity to the nearest centroid, which is EMA micro-tuned. When the unclassified pool exceeds 10 words, a new word class emerges through greedy clustering.

### Interoceptive Self-Monitoring

Continuously monitors RSS memory, connection growth rate, and reasoning latency with 3-tier response:

| Level         | Condition   | Action                                            |
|---------------|-------------|---------------------------------------------------|
| 🟢 GREEN      | Normal      | Normal operation                                  |
| 🟡 YELLOW     | Warning     | Log alert + raise learning threshold              |
| 🔴 RED        | Critical    | Emergency save + bulk prune weak edges            |

---

## Learning System *(자세한 내용은 영문/중문 버전 참조)*

### Pretraining System

Skip-gram / CBOW word embedding pretraining (1,624 lines): dynamic window size, negative sampling, momentum acceleration, gradient clipping, learning rate scheduling.

### Learner Matrix

| Learner | Method | Description |
|---------|--------|-------------|
| **Autonomic** | Hebbian online | Co-occurrence reinforcement, 16-shard concurrent updates |
| **Active** | 24/7 background | Auto-acquires new knowledge, analyzes concept relationships |
| **Self** | Curiosity-driven | Curiosity sampling → deep walk → knowledge review → self-correction |
| **BPTT** | Temporal backprop | RNN + Linear layers, Adam optimizer (lr=0.001) |

### Catastrophic Forgetting Prevention

Based on EWC (Elastic Weight Consolidation): Fisher information matrix marks parameter importance, selectively protecting prior knowledge.

---

## Neural Network Subsystem *(자세한 내용은 영문/중문 버전 참조)*

**Tensor**: Multi-dim tensor create/destroy/broadcast/clone/view (889 lines)  
**Layers**: 8 types (LINEAR/RELU/SIGMOID/TANH/SOFTMAX/DROPOUT/EMBEDDING/SIMPLE_RNN)  
**LSTM** (713 lines): Full LSTM, bidirectional, layer normalization  
**GRU** (621 lines): Full GRU, update/reset gates, bidirectional  
**Model**: Multi-layer stacking, forward pass, MSE loss, serialization  
**Trainer**: Mini-batch training, learning rate scheduling  
**Optimizer**: SGD / Adam (β1=0.9, β2=0.999) / RMSprop  
**Quantization**: FP16 / INT8 / INT4 / INT2 precision reduction  
**Pruning**: MAGNITUDE / RANDOM / GRADIENT / STRUCTURED strategies  
**Attention**: Bahdanau / Luong / Self-Attention / Multi-Head Attention

---

## Quick Start

### Build

```bash
# Requires GCC + pthread + OpenMP (needs libcurl + openssl, otherwise zero dependencies)
make all

# ARM cross-compilation
make CC=aarch64-linux-gnu-gcc all

# Debug build (with ASAN address/UB detection)
make asan

# Run all unit tests
make test
```

### Run

```bash
# Interactive gateway (recommended)
./build/bin/pivotmind_gateway

# CLI interactive mode
./build/bin/digital_life
```

The gateway listens on `:8080` by default, with an HTML dashboard (auto-refreshing JS) at `/`.

### API Examples

```bash
# Ask a question
curl -X POST http://localhost:8080/chat \
  -H "Content-Type: application/json" \
  -d '{"query":"What is consciousness?"}'

# Feed learning material
curl -X POST http://localhost:8080/learn \
  -H "Content-Type: application/json" \
  -d '{"text":"Consciousness is the subjective experience produced by neural networks."}'

# Check status
curl http://localhost:8080/status

# Health check
curl http://localhost:8080/health
```

### Build Targets

| Command | Description |
|---------|-------------|
| `make all` | Build all targets |
| `make gateway` | Build HTTP gateway only |
| `make digital-life` | Build CLI interactive version |
| `make seed-builder` | Build seed topology tool |
| `make debug-seed` | Build debug seed tool |
| `make batch-learn` | Batch training tool |
| `make corpus-train` | Corpus training tool |
| `make template-build` | Template construction tool |
| `make test-dialog` | Dialog testing tool |
| `make clean` | Clean build artifacts |
| `make test` | Run all unit tests |

---

## Project Structure

```
pivotmind/
├── src/               # 85 core source files (~48,600 lines C)
├── include/           # 89 header files (~12,600 lines)
├── demos/             # Gateway and interactive entry
├── tools/             # 57 tools (training/debugging/data processing/corpus DL)
├── tests/             # Unit tests + integration tests + fixtures
├── scripts/           # Automation scripts (feeding, knowledge DL, etc.)
├── changelogs/        # 44 version changelogs (000-043)
├── docs/              # Architecture documentation and diagrams
├── data/              # Runtime data (hermes knowledge base 25MB, etc.)
└── libs/              # Third-party libraries
```

---

## Version History

| Version    | Highlights                                                                          |
|------------|-------------------------------------------------------------------------------------|
| v0.1.x     | Basic walk reasoning, competitive queue, state persistence                          |
| v0.2.x     | Multi-layer diffusion, Hippocampus / DMN / Perception, interoceptive monitoring     |
| **v0.3.0** | Prefrontal Executive (6-mode reasoning), IdeaArena 5D, strategy weight self-learning |
| **v0.4.0** | Code simplification, brain boundary fixes, Broca upgrade, Hypothalamus/Thalamus/Brainstem |
| **v0.4.1** | Web fetch refactor (libcurl engine), Bing/Bing News providers, news timer           |
| **v0.4.2** | Comprehensive realloc dangling pointer fix (15+ sites), 4-round memory safety audit |
| **v0.4.3** | **Emergent POS** — seed anchors + 512-dim feature clustering, grammar emerges from data |
| **v0.5.5** | Diffusion function word filter (~130 words), cross-layer index fix, double-free race fix |

> Detailed changelogs: see [`changelogs/`](changelogs/) directory

---

## Known Limitations

- **Generation fluency** — Associative path output is less natural than LLM generated text (actively iterating)
- **No GPU acceleration** — Pure CPU + pthread + OpenMP
- **Binary state files** — Not cross-architecture compatible (x86_64 and ARM; JSON/MessagePack format planned)
- **Single-node only** — No distributed multi-node topology support yet

---

## Roadmap

- [ ] FPGA deployment (ultimate goal: hardware-level neuromorphic computing)
- [ ] Distributed multi-node topology (cross-device activation propagation)
- [ ] Visual / auditory multimodal input interfaces
- [ ] JSON/MessagePack text format persistence (cross-architecture compatibility)

---

## Contributing

Issues and Pull Requests are welcome. For major changes, please open an issue first to discuss what you would like to change.

---

## License

[Apache License 2.0](LICENSE)

---

<a name="running-on"></a>
*Currently running on **EAIDK-610** (RK3399 ARM Cortex-A72, 3.8GB RAM).*
*Goal: A self-sustaining distributed cognitive engine deployable on embedded hardware.*

<div align="center">

메인테이너：[陈道祥 (afd-ll)](https://github.com/afd-ll)

[⭐ Star this repo](https://github.com/afd-ll/PivotMind) · [Report a bug](https://github.com/afd-ll/PivotMind/issues) · [Read the docs](ARCHITECTURE.md)

</div>
