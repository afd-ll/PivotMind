<div align="center">

# 玄枢 PivotMind

### A Brain-Region-Structured Cognitive Engine in Pure C

**No transformer runtime · No pretrained embeddings · Designed for continuous operation on ARM boards**

[English](README.md) · [简体中文](README.zh-CN.md)

[![Version](https://img.shields.io/badge/version-v0.5.23-blue.svg)](changelogs/)
[![License](https://img.shields.io/badge/license-Apache%202.0-green.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C-99%2B-orange.svg)](https://en.wikipedia.org/wiki/C99)
[![Platform](https://img.shields.io/badge/ARM-RK3399%20%7C%20x86__64-lightgrey.svg)](#quick-start)

</div>

---

## What This Is

PivotMind is a cognitive engine written in C. It does not use transformer inference, pretrained word embeddings, or an AI framework. Its basic learning loop is explicit: concepts are represented as nodes, co-occurrence creates weighted edges, activation diffuses through several topologies, and candidate sequences compete for output.

The project is designed around continuous online learning on modest hardware. Corpus input is the primary learning signal; the generated output is not treated as an independent source of truth.

**Current version: v0.5.23.** This is a research project in the toy stage, not a replacement for a large language model or a production conversational system.

## Architecture

### Engineering Modules Named After Brain Regions

The brain-region names describe engineering responsibilities. They are an organizational analogy, not a claim that the software is a biological brain simulation. Each name maps to a concrete C module and its runtime role.

<p align="center"><img src="diagrams/brain-regions.png" alt="PivotMind module architecture" width="780"/></p>

| Module | File | Responsibility |
|---|---|---|
| Prefrontal | `prefrontal.c` | Dialog generation and adaptive gating |
| Prefrontal Executive | `prefrontal_executive.c` | Six-mode reasoning, task decomposition, conflict detection |
| Hippocampus | `hippocampus.c` | Memory consolidation, replay, perception coupling |
| DMN | `dmn.c` | Idle association and exploration |
| Amygdala | `amygdala.c` | Valence sampling and explore/exploit balance |
| Perception | `perception.c` | Search and article-reading pipeline |
| Broca's Area | `broca.c` | Template construction and decay scheduling |
| Cerebellum | `cerebellum.c` | Training-related functions and resource protection |
| Hypothalamus | `hypothalamus.c` | Four-dimensional drive regulation |
| Thalamus | `thalamus.c` | Signal bus, routing, resource gating, tool slots |
| Brainstem | `brainstem.c` | Heartbeat, decay, and spontaneous activation |
| Cingulate (ACC) | `cingulate.c` | Sequence evaluation across semantic, template, valence, and length signals |
| IdeaArena | `idea_arena.c` | Multi-candidate competition and lateral inhibition |
| Reticular | `reticular.c` | Arousal and alertness regulation |
| Visual Cortex | `visual_cortex.c` | Frame, subtitle, and cross-modal input processing |

### TraceWisdom Network: 12 Topologies

PivotMind uses 12 topology spaces:

`vocabulary / semantic / emotion / syntax / context / domain / pragmatics / culture / concept / template / visual / master`

Nodes and edges are stored by topology. Cross-topology propagation uses adjacency indexing. The network is not a lookup table: an input activates a region of the network, and the diffusion engine supplies candidates to the competition stage.

### Difference from a Conventional LLM

| Conventional LLM | PivotMind |
|---|---|
| Token prediction through a pretrained parameter set | Node activation over an explicit, mutable network |
| Primarily offline gradient training | Online co-occurrence learning from corpus input |
| One dominant representation space | Multiple topology spaces with separate roles |
| Stateless request-level inference | Persistent state and continuous background activity |

## Core Mechanisms

### Hebbian Co-occurrence Learning

Co-occurring symbols strengthen their connection. A simplified update is:

```text
w(i, j, t + 1) = w(i, j, t) + eta * cooccur(i, j)
```

The implementation contains separate character/word processing and promotion paths. New structure is expected to be supported by corpus statistics rather than inserted as a hand-written answer table.

### Diffusion and Competition

The diffusion engine is the output path. Activation decays and propagates across relevant topologies; candidate sequences then compete through semantic, template, emotion/valence, length, and reward-related signals. The implementation is in `src/` and `idea_arena.c`, not in a remote model call.

### Emergent POS and Template Structure

Part-of-speech evidence is accumulated by the feed pipeline. POS clustering and template construction are experimental mechanisms: they are being measured against the quality of generated structure and are not yet a complete grammar system.

### Persistence and Runtime Safety

- Atomic state persistence using temporary files and rename
- A systemd-managed gateway with HTTP endpoints
- Memory protection and restart/load verification in the deployment environment
- Explicit concurrency fixes around learning queues, snapshots, and topology access

## Current Runtime Snapshot

Measured on the EAIDK-610 board on 2026-08-17. These values are a snapshot of one reset/rebuild baseline, not a benchmark promise.

| Metric | Value |
|---|---|
| Version | `0.5.23` |
| Runtime status | `running`; `/health` returned `ok` |
| Nodes | `17,105` |
| Topologies | `12` |
| State file | about `42.6 MiB` |
| Gateway RSS | about `257 MiB` at inspection time |
| Hardware | RK3399, 4 GB RAM, Armbian ARM64 |

The previous large state was intentionally retired during the v0.5.21 reset baseline. Older measurements such as 380,000+ nodes and 623 MB RSS belong to a different state and should not be compared directly with this snapshot.

## Quick Start

### Build

```bash
# Requires GCC, pthread, OpenMP, libcurl, OpenSSL, and zlib
# On a 4 GB ARM board, use a bounded build rather than nproc-wide parallelism.
make -j1 gateway

# ARM cross-compilation
make CROSS_COMPILE=aarch64-linux-gnu- gateway

# Debug build
make DEBUG=1 gateway

# Build the test targets
make test
```

### Run

```bash
# HTTP gateway on port 8080
./build/bin/pivotmind_gateway 8080

# CLI mode
./build/bin/pivotmind_cli
```

### API Examples

```bash
# Generate a response
curl -X POST http://localhost:8080/chat \
  -H "Content-Type: application/json" \
  -d '{"msg":"什么是政治？"}'

# Feed external learning material
curl -X POST http://localhost:8080/learn \
  -H "Content-Type: application/json" \
  -d '{"msg":"你的学习文本"}'

# Inspect runtime state
curl http://localhost:8080/status
curl http://localhost:8080/health
```

`/learn` is the main learning path. The quality of the resulting network depends on the corpus and on the mechanisms currently under evaluation.

## Project Structure

```text
src/            Engine implementation
include/        Public headers
demos/          Gateway and CLI entrypoints
tools/          State and analysis utilities
tests/          Unit and regression tests
changelogs/     Version history
diagrams/       Architecture diagrams
```

## Known Limitations

- **Generated language is not grammatical by default.** The current output is still largely an activation/proximity result; word-layer assembly is an active development area.
- **Abstract concepts are incomplete.** The syntax and concept topologies are still accumulating structure and do not provide human-level abstraction.
- **Corpus bias matters.** The learned network reflects the material it receives; political and historical text can dominate if modern spoken-language material is absent.
- **External validation is required.** Internal generation is used for diagnostics and selected reinforcement, not as an unverified replacement for corpus evidence.
- **The project is in the toy stage.** The architecture is being tested through implementation and measurement; capabilities should not be inferred from module names.

## Roadmap

- Stabilize the post-reset distribution and external-support measurements
- Revive and evaluate the sequence assembler against held-out material
- Continue POS and syntax-topology accumulation
- Add stronger support checks before treating generated structure as reliable
- Explore embodied/action-observation learning after the core runtime is stable

## Contributing

This is a personal research project. Issues and pull requests are welcome. Major architectural decisions are documented and owned by the author; see [CONTRIBUTING.md](CONTRIBUTING.md) when proposing changes.

## License

[Apache 2.0](LICENSE)
