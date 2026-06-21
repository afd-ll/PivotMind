<div align="center">

# 玄枢 PivotMind

### A Brain-Inspired Semantic Association Engine
**Pure C · Zero AI Framework Dependencies · Runs on ARM Embedded Boards**

[English](README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Русский](README.ru.md)

[![Version](https://img.shields.io/badge/version-v0.4.0-blue.svg)](changelogs/)
[![License](https://img.shields.io/badge/license-Apache%202.0-green.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C-99%2B-orange.svg)](https://en.wikipedia.org/wiki/C99)
[![Platform](https://img.shields.io/badge/ARM-RK3399%20%7C%20x86__64-lightgrey.svg)](#running-on)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-success.svg)](#quick-start)
[![CI](https://github.com/afd-ll/PivotMind/actions/workflows/ci.yml/badge.svg)](https://github.com/afd-ll/PivotMind/actions/workflows/ci.yml)

> Intelligence is not a stack of matrix multiplications,
> but ripples of activation spreading through a reasoning network.

</div>

---

## What is PivotMind

PivotMind is a **brain-inspired cognitive engine** built on
[HuarongTopologyNet](#huarongtopologynet) +
[Hebbian Learning](#core-mechanisms) +
[Multi-Layer Diffusion Reasoning](#multi-layer-diffusion-engine).
No Transformers. No embedding vectors. No backpropagation.
Just nodes, edges, activation, and decay — powered by a relentless background clock.

**Current Version: v0.4.0** — 10 brain regions, PFE reasoning, IdeaArena competition, online Hebbian learning.

### HuarongTopologyNet

Each concept is a node. Co-occurrence creates an edge. Edges carry a triple attribute:
**weight × confidence × motivational bias**.
Ten sub-topologies (vocabulary / semantic / emotion / syntax / context / domain / pragmatics / culture / concept / template)
each form an independent reasoning network, interconnected through cross-topology links.
Activation diffuses simultaneously across layers, with competition selecting the winner as output.

### Why This Approach

| Traditional LLM       | PivotMind                                              |
|-----------------------|--------------------------------------------------------|
| Token prediction, stateless | Node activation, continuous internal state       |
| Gradient-based offline batch training | Hebbian online real-time learning          |
| Single embedding space | 10 independent sub-topologies                          |
| Neural network black box | Explicit node-edge paths, fully traceable            |
| Requires GPU + massive VRAM | pthread + OpenMP only, runs on ARM embedded     |
| Inference separate from learning | Conversation IS learning                    |
| No physiological awareness | Interoceptive self-monitoring, 3-tier health response |

---

## Brain Region Architecture

PivotMind models mammalian cortical functional divisions — 10 brain regions, each with dedicated responsibilities, communicating through the Thalamus signal bus.

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

> Detailed changelogs: v0.3.0 → [changelogs/032-v0.3.0-reasoning-architecture.md](changelogs/032-v0.3.0-reasoning-architecture.md) ｜ v0.4.0 → [changelogs/034-v0.4.0-code-simplify-brain-boundary.md](changelogs/034-v0.4.0-code-simplify-brain-boundary.md)

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

|<div align="center">
|
|Maintained by [陈道祥 (afd-ll)](https://github.com/afd-ll)
|
|[⭐ Star this repo](https://github.com/afd-ll/PivotMind) · [Report a bug](https://github.com/afd-ll/PivotMind/issues) · [Read the docs](ARCHITECTURE.md)
|
|</div>
