<div align="center">

# 玄枢 PivotMind

### A Brain-Inspired Semantic Association Engine
**순수 C · AI 프레임워크 의존성 제로 · ARM 임베디드 보드에서 실행**

[English](README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Русский](README.ru.md)

[![Version](https://img.shields.io/badge/version-v0.4.0-blue.svg)](changelogs/)
[![License](https://img.shields.io/badge/license-Apache%202.0-green.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C-99%2B-orange.svg)](https://en.wikipedia.org/wiki/C99)
[![Platform](https://img.shields.io/badge/ARM-RK3399%20%7C%20x86__64-lightgrey.svg)](#running-on)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-success.svg)](#quick-start)

> Intelligence is not a stack of matrix multiplications,
> but ripples of activation spreading through a reasoning network.

</div>

---

## What is PivotMind

PivotMind는 [HuarongTopologyNet](#huarongtopologynet) +
[Hebbian Learning](#core-mechanisms) +
[Multi-Layer Diffusion Reasoning](#multi-layer-diffusion-engine)을 기반으로 한
**뇌에서 영감을 받은 인지 엔진**입니다.
Transformer 없음. 임베딩 벡터 없음. 역전파 없음.
노드, 엣지, 활성화, 감쇠 — 끊임없이 돌아가는 백그라운드 클록에 의해 구동됩니다.

**현재 버전: v0.4.0** — 10개 뇌 영역, PFE 추론, IdeaArena 경쟁, 온라인 Hebbian 학습.

### HuarongTopologyNet

각 개념은 노드입니다. 공기(co-occurrence)가 엣지를 생성합니다. 엣지는 세 가지 속성을 갖습니다:
**가중치 × 신뢰도 × 동기적 편향**.
10개의 하위 토폴로지(어휘 / 의미 / 감정 / 구문 / 문맥 / 도메인 / 화용 / 문화 / 개념 / 템플릿)
각각이 독립적인 추론 네트워크를 형성하고, 토폴로지 간 링크를 통해 상호 연결됩니다.
활성화는 계층 간 동시에 확산되며, 경쟁을 통해 승자가 출력으로 선택됩니다.

### Why This Approach

| 기존 LLM                       | PivotMind                                                  |
|--------------------------------|------------------------------------------------------------|
| 토큰 예측, 상태 비보존          | 노드 활성화, 지속적 내부 상태                              |
| 경사하강 기반 오프라인 배치 학습 | Hebbian 온라인 실시간 학습                                 |
| 단일 임베딩 공간                | 10개 독립 하위 토폴로지                                    |
| 신경망 블랙박스                 | 명시적 노드-엣지 경로, 완전 추적 가능                      |
| GPU + 대용량 VRAM 필요          | pthread + OpenMP만, ARM 임베디드에서 실행                   |
| 추론과 학습이 분리              | 대화 자체가 곧 학습                                        |
| 생리적 자각 없음                | 내수용적 자기 모니터링, 3단계 건강 대응                     |

---

## Brain Region Architecture

PivotMind는 포유류 대뇌 피질의 기능적 분화를 모델링합니다 — 10개 뇌 영역, 각자 전담하는 책임이 있으며, Thalamus 신호 버스를 통해 통신합니다.

```
                          ┌──────────────────────┐
                          │   Prefrontal Cortex    │ ← Dialog / Decision Entry
                          │  + Prefrontal Exec PFE │ ← Reasoning Orchestrator
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

| Brain Region       | File                       | Function                                                       |
|--------------------|----------------------------|----------------------------------------------------------------|
| Prefrontal         | `prefrontal.c`             | Dialog generation, diffusion → ACC evaluation                  |
| Prefrontal Exec    | `prefrontal_executive.c`   | 6-mode reasoning orchestration, task decomposition             |
| Hippocampus        | `hippocampus.c`            | Memory consolidation, QA replay, perception coupling            |
| DMN                | `dmn.c`                    | Default Mode Network: dream association, idle exploration      |
| Amygdala           | `amygdala.c`               | Emotional valence sampling, explore / exploit balance          |
| Perception Cortex  | `perception.c`             | Web search, article_reader semantic pipeline                   |
| Broca's Area       | `broca.c`                  | Template auto-building and decay scheduling                    |
| Cerebellum         | `cerebellum.c`             | BPTT fine-tuning, hardware resource protection                 |
| Hypothalamus       | `hypothalamus.c`           | Drive dynamics regulation, circadian coupling                  |
| Thalamus           | `thalamus.c`               | Signal bus, resource gating, inter-region routing              |
| Brainstem          | `brainstem.c`              | Circadian heartbeat, activation decay, spontaneous activation  |
| Cingulate (ACC)    | `cingulate.c`              | 4D sequence evaluation (semantic + template + emotion + length) |
| IdeaArena          | `idea_arena.c`             | Multi-candidate 5D competitive selection                       |

---

## Core Mechanisms

### Multi-Layer Diffusion Engine

Input is tokenized via sliding window, then diffuses simultaneously across layers:

- **Vocabulary** — direct literal matching, fast recall
- **Semantic** — cross-topology association across 10 sub-topologies
- **Template** — syntactic pattern recognition, guiding connector insertion
- **Emotion** — valence × arousal weighting, modulating candidate priority

Lateral inhibition ensures output diversity.

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

Subgoals are recursively decomposed (configurable depth). Conflict detection + IdeaArena competition
selects optimal paths, producing explainable reasoning chains.

### Interoceptive Self-Monitoring

Continuously monitors RSS memory, connection growth rate, and reasoning latency with 3-tier response:

| Level         | Condition   | Action                                            |
|---------------|-------------|---------------------------------------------------|
| 🟢 GREEN      | Normal      | Normal operation                                  |
| 🟡 YELLOW     | Warning     | Log alert + raise learning threshold              |
| 🔴 RED        | Critical    | Emergency save + bulk prune weak edges            |

---

## Quick Start

### Build

```bash
# Requires GCC + pthread + OpenMP (no other dependencies)
make all

# ARM cross-compilation
make CC=aarch64-linux-gnu-gcc all
```

### Run

```bash
# Interactive gateway (recommended)
./build/bin/pivotmind_gateway

# CLI interactive mode
./build/bin/digital_life
```

The gateway listens on `:8080` by default.

### API Examples

```bash
# Ask a question
curl -X POST http://localhost:8080/ask \
  -H "Content-Type: application/json" \
  -d '{"query":"What is consciousness?"}'

# Feed learning material
curl -X POST http://localhost:8080/learn \
  -H "Content-Type: application/json" \
  -d '{"text":"Consciousness is the subjective experience produced by neural networks in the brain."}'

# Check status
curl http://localhost:8080/status
```

### Build Targets

| Command              | Description                                |
|----------------------|--------------------------------------------|
| `make all`           | Build all targets                          |
| `make gateway`       | Build HTTP gateway only                    |
| `make digital-life`  | Build CLI interactive version              |
| `make seed-builder`  | Build seed topology tool                   |
| `make batch-learn`   | Batch training tool                        |
| `make clean`         | Clean build artifacts                      |

---

## Project Structure

```
pivotmind/
├── src/               # 82 core source files
├── include/           # 86 header files
├── demos/             # Gateway and interactive entry
├── tools/             # Training / debugging / data processing tools
├── tests/             # Unit tests (23 PFE tests, 100% pass)
├── scripts/           # Automation scripts
├── changelogs/        # Version changelogs
├── docs/              # Architecture docs and diagrams
└── archived/          # Historical version archives
```

---

## Version History

| Version    | Highlights                                                                          |
|------------|-------------------------------------------------------------------------------------|
| v0.1.x     | Basic walk reasoning, competitive queue, state persistence                          |
| v0.2.x     | Multi-layer diffusion, Hippocampus / DMN / Perception, interoceptive monitoring     |
| **v0.3.0** | Prefrontal Executive (6-mode reasoning), IdeaArena 5D, strategy weight self-learning |
| **v0.4.0** | Code simplification (~200 lines), 11 unified lookups, Broca upgrade, Hypothalamus new region |

> Detailed changelogs: see [`changelogs/`](changelogs/) directory

---

## Known Limitations

- **Chinese-first** — Character-level tokenization naturally suits Chinese; English / mixed input experience is limited
- **Response fluency** — Associative path output is less natural than LLM-generated text (actively iterating)
- **No GPU acceleration** — Pure CPU + pthread + OpenMP
- **Binary state files** — Not cross-architecture (x86_64 and ARM are incompatible)

---

## Roadmap

- [ ] FPGA deployment (ultimate goal: hardware-level neuromorphic computing)
- [ ] Distributed multi-node topology (cross-device activation propagation)
- [ ] Visual / auditory multimodal input interfaces
- [ ] Scheduled auto-save

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

[⭐ Star this repo](https://github.com/afd-ll/PivotMind) · [Report a bug](https://github.com/afd-ll/PivotMind/issues) · [Read the docs](ARCHITECTURE.md)

</div>
