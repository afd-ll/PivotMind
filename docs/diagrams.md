# PivotMind Architecture Diagrams

> All diagrams use Mermaid syntax. Rendered natively on GitHub.

## 1. Brain Region Architecture

```mermaid
graph TB
    PF["🧠 Prefrontal Cortex<br/>Dialog / Decision"]
    PFE["🎯 PFE<br/>6-Mode Reasoning"]
    HC["📚 Hippocampus<br/>Memory Consolidation"]
    DMN["💭 DMN<br/>Dream / Idle"]
    AMY["😊 Amygdala<br/>Emotion"]
    PERC["🔍 Perception<br/>Web Search"]
    BROCA["📝 Broca<br/>Template Builder"]
    CB["⚖️ Cerebellum<br/>BPTT / Protect"]
    BS["⏰ Brainstem<br/>Circadian Clock"]
    HYPO["🔥 Hypothalamus<br/>4D Drives"]
    ACC["✅ ACC<br/>4D Evaluation"]
    ARENA["🏟️ IdeaArena<br/>Candidate Competition"]
    RET["⚡ Reticular<br/>Arousal"]
    VC["👁️ VisualCortex v0.5<br/>Multi-Modal Pipeline"]

    TH["📡 Thalamus<br/>Signal Bus + Resource Gate"]

    PF --> TH
    PFE --> TH
    HC --> TH
    DMN --> TH
    AMY --> TH
    PERC --> TH
    BROCA --> TH
    CB --> TH
    BS --> TH
    HYPO --> TH
    ACC --> TH
    ARENA --> TH
    RET --> TH
    VC --> TH

    TH --> PF
    TH --> HC
    TH --> PERC
    TH --> VC

    style VC fill:#4fc3f7,stroke:#0288d1,color:#000
    style TH fill:#ffcc80,stroke:#ef6c00,color:#000
```

## 2. Topology Layer Architecture

```mermaid
graph TD
    VOCAB["词汇拓扑 (0)"] --- SEM["语义拓扑 (1)"]
    VOCAB --- EMOTION["情绪拓扑 (2)"]
    VOCAB --- SYNTAX["语法拓扑 (3)"]
    SEM --- DOMAIN["领域拓扑 (5)"]
    SEM --- VISUAL["视觉拓扑 (11) v0.5<br/>跨模态锚定"]
    EMOTION --- CULTURE["文化拓扑 (7)"]
    SYNTAX --- PRAGMA["语用拓扑 (6)"]
    SYNTAX --- TEMPLATE["模板拓扑 (10)"]
    CONTEXT["上下文拓扑 (4)"] --- CULTURE
    CONTEXT --- CONCEPT["概念拓扑 (8)"]

    VOCAB --- VISUAL

    style VISUAL fill:#4fc3f7,stroke:#0288d1,color:#000
```

## 3. Multi-Modal Pipeline

### Pipeline A: Subtitle (MediaReader)

```mermaid
flowchart LR
    A["🎬 Video File"] --> B["ffprobe<br/>detect subtitle track"]
    B --> C["ffmpeg<br/>extract SRT/VTT/ASS"]
    C --> D["SRT Parser<br/>strip HTML + timestamps"]
    D --> E["article_process_line()<br/>PMI word discovery"]
    E --> F["article_flush()<br/>create vocab nodes + edges"]
    F --> G["🧠 Topology Network"]
```

### Pipeline B: Visual Cortex (Cross-Modal Alignment)

```mermaid
flowchart TB
    V["🎬 Video File"]
    V --> FK["ffprobe<br/>keyframes + scene detection"]
    V --> SUBTITLE["ffmpeg<br/>extract SRT subtitles"]

    FK --> FRAMES["VisualFrame[]<br/>512-dim features<br/>Phase 1: zero-vector placeholder"]
    SUBTITLE --> TOKENS["TimedToken[]<br/>word-level timestamps"]

    FRAMES --> ALIGN["Time-window Alignment<br/>token ± 2000ms ↔ frames"]
    TOKENS --> ALIGN

    ALIGN --> COOC["Co-occurrence Counting<br/>hash table"]
    COOC --> THRESHOLD{"count ≥ min_cooccurrence?"}
    THRESHOLD -->|Yes| EDGE["master_add_cross_link()<br/>vocab 'apple' ↔ visual 'apple_visual'<br/>relation: 'visual_anchor'"]
    THRESHOLD -->|No| SKIP["skip"]

    EDGE --> SIG["THAL_SIG_CROSS_MODAL_EDGE"]
```

## 4. Task Queue Model

```mermaid
sequenceDiagram
    participant GW as Gateway
    participant Q as Task Queue (Ring Buffer 128)
    participant BS as Brainstem
    participant TH as Thalamus
    participant VC as VisualCortex

    GW->>Q: enqueue(filepath, mode)
    Note over GW,Q: Non-blocking, async

    loop Every 1s tick
        BS->>TH: get_throttle(THAL_VISUAL_CORTEX)
        TH-->>BS: throttle value

        alt throttle < 0.1
            BS-->>VC: skip (idle cooldown)
        else throttle >= 0.1
            BS->>VC: visual_cortex_tick(throttle)
            VC->>Q: dequeue 1 task
            Q-->>VC: (filepath, mode)
            VC->>VC: extract frames + SRT + align
            VC->>TH: THAL_SIG_VISUAL_FRAME
            VC->>TH: THAL_SIG_CROSS_MODAL_EDGE
        end
    end
```

## 5. Cross-Modal Alignment Detail

```mermaid
flowchart LR
    subgraph INPUT["Input"]
        WORD["Word: 苹果<br/>ts: 1500ms"]
        F1["Frame 01<br/>ts: 500ms"]
        F2["Frame 02<br/>ts: 1200ms"]
        F3["Frame 03<br/>ts: 2000ms"]
        F4["Frame 04<br/>ts: 3500ms"]
    end

    subgraph WINDOW["Alignment Window ±2000ms"]
        M["Wait..."]
    end

    subgraph RESULT["Result"]
        CNT["Co-occurrence: 3<br/>≥ min_cooccurrence(2)"]
        EDGE["Cross-Topo Edge<br/>vocab'苹果' ↔ visual'苹果_视觉'"]
    end

    WORD --> WINDOW
    F2 --> WINDOW
    F3 --> WINDOW
    F1 -- "out of window" --> X1["✗"]
    F4 -- "out of window" --> X2["✗"]
    WINDOW --> CNT
    CNT --> EDGE
```

## 6. Data Flow: Input to Topology

```mermaid
flowchart TD
    subgraph INPUTS["Input Sources"]
        CHAT["💬 /chat<br/>text conversation"]
        LEARN["📖 /learn<br/>feed text"]
        MEDIA["🎬 /media/feed<br/>video/audio"]
        WEB["🌐 Perception<br/>web search"]
    end

    subgraph PIPELINES["Processing Pipelines"]
        TOKEN["tokenize<br/>sliding window"]
        PMI["article_reader<br/>PMI word discovery"]
        SRT["media_reader<br/>SRT extraction"]
        VISUAL["visual_cortex<br/>frame + align"]
    end

    subgraph TOPOLOGY["Topology Network"]
        VOCAB_TOP["词汇拓扑"]
        SEM_TOP["语义拓扑"]
        VIS_TOP["视觉拓扑 v0.5"]
        TEMPLATE_TOP["模板拓扑"]
        CROSS["Cross-Topology Links<br/>O(1) adjacency index"]
    end

    CHAT --> TOKEN
    LEARN --> PMI
    WEB --> PMI
    MEDIA --> SRT
    MEDIA --> VISUAL

    TOKEN --> VOCAB_TOP
    PMI --> VOCAB_TOP
    SRT --> VOCAB_TOP
    VISUAL --> VIS_TOP
    VISUAL --> CROSS
    VOCAB_TOP <--> CROSS
    VIS_TOP <--> CROSS
    SEM_TOP <--> CROSS
    TEMPLATE_TOP <--> CROSS
```

## 7. Brainstem Tick Loop

```mermaid
flowchart TD
    TICK["Brainstem tick (1s)"]
    TICK --> DECAY["decay activations × 0.97"]
    DECAY --> SPON{"spontaneous<br/>activation?<br/>(0.01% chance)"}
    SPON -->|Yes| ACTIVATE["activate random node"]
    SPON -->|No| CIRCADIAN["update circadian rhythm"]

    CIRCADIAN --> CEREBELLUM["cerebellum_tick<br/>CPU/memory protect"]
    CEREBELLUM --> THALAMUS["thalamus_tick<br/>recompute throttles"]

    THALAMUS --> HYPO{"tick % 300 == 0?"}
    HYPO -->|Yes| HYPOTHALAMUS["hypothalamus_tick<br/>drive regulation"]
    HYPO -->|No| PERCEPTION["brainstem_tick_perception"]

    HYPOTHALAMUS --> PERCEPTION

    PERCEPTION --> PERC_TICK["perception_tick<br/>+ hourly news search"]
    PERC_TICK --> AMYG_TICK["amygdala_tick<br/>emotion sampling"]
    AMYG_TICK --> VC_TICK["visual_cortex_tick v0.5<br/>dequeue + process media"]

    VC_TICK --> CONSOLIDATE{"tick % N == 0?"}
    CONSOLIDATE -->|Yes| HIPPO["hippocampus_consolidate<br/>+ DMN dream"]
    CONSOLIDATE -->|No| SYNAPSE{"tick % 600 == 0?"}

    HIPPO --> SYNAPSE
    SYNAPSE -->|Yes| SCALE["synapse scaling<br/>log(edge_count) adjustment"]
    SYNAPSE -->|No| SAVE{"tick % 1800 == 0?"}

    SCALE --> SAVE
    SAVE -->|Yes| PERSIST["master_save_state<br/>+ save_features"]
    SAVE -->|No| DONE["sleep 1s → next tick"]
    PERSIST --> DONE
```
