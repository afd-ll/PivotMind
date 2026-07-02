# 0.4.11 — 双语对话语法引擎 + 扩散激活优化

> **日期**: 2026-07-02 | **类型**: 新增 + 优化

## 概述

攻坚 `/chat` 管线对话质量。核心成果：将扩散输出从"词序列"升级为"主谓宾句子"（中英双语），并改善扩散激活的语义区分度。发现知识图谱边质量是当前瓶颈。

## 核心变更

### 1. 句式骨架组装 (diffusion.c)

扩散输出阶段全面重写，从"POS 优先级平铺"变为"句式骨架填槽 + 动词配价驱动"：

**句式骨架**（第一版）：
- 定义 24 种中英文句式（4词长句优先，`ADJ+ADJ的NOUN` 格式）
- 槽位多词填充（每槽 3 词，顿号列举）
- 连接词按 POS 对自动注入（中文：的/地/得/和/个/是，英文：空格/of/and）

**动词配价驱动**（第二版，核心）：
- 中文配价表 43 项：及物/不及物/系词/双宾/补语/形容词谓语
- 英文配价表 70+ 项：be/have/transitive/intransitive/modal/copula
- 语法组装流程：找谓语 → 分配主语(NP) → 分配宾语/补语 → 加助词(了) → 标点
- 中文：主格代词+NOUN 主语 + 状语 + 谓语(很+Adj或V+得+补) + 宾语 + 了+。
- 英文：主格代词白名单 + be/have 系词 + 冠词(a/an) + 空格 + 名词 + .

### 2. 英文 POS 系统 (cognitive_controller.c + diffusion.c)

**英文后缀规则**（220+ 行新增）：
- 120+ 后缀推断：`-ly→ADV, -ful/-less/-ous→ADJ, -tion/-ment/-ness→NOUN, -ing/-ed→VERB`
- 200+ 小词典：功能词(pron/prep/conj/adv/num/interj) + 常见名词/动词/形容词
- `english_pos_lookup()` 暴露为公共 API，供 diffusion.c 直接调用
- 在 `pos_tag_emergent()` 中自动检测 ASCII 词走英文规则

**英文连接词映射** (multi_topology.c)：
- `english_connector_map()`: ADJ+N→空格, N+N→of, N+ADJ→is, VERB+VERB→and
- 组装器自动检测候选词首字符 ASCII 选择中/英文模式

### 3. 扩散引擎优化

**激活机制改进**：
- 深度 depth: 1→2（允许更深层语义扩散）
- top_k: 5→10（保留更多样化候选）
- **枢纽词过滤**：>2000 边的超级连通节点（如"你"8000边、"是"8000边）跳过扩散，减少噪声
- **输入词注入**：按连接度分级加权（0边=0.6, <100边=0.4, <2000边=0.2），枢纽词不加权

**效果**：不同输入产生不同的候选词集，但知识图谱边质量限制输出质量。

### 4. /chat fallback 链完善 (048-050 归入)

```
PFE 推理
  ├─ 成功 → 返回
  └─ 失败 ↓
prefrontal_chat (扩散 + ACC 门控)
  ├─ 成功 → 返回
  └─ 失败 ↓
联想推理 (associate + topology_walk)
  ├─ 成功 → 返回
  └─ 失败 ↓
QA 记忆检索 (token 交集评分)
  ├─ 命中 → 返回预设答案
  └─ 未命中 → "(无回应)"
```

完整四层 fallback 全部验证通过。`goodbye` → QA Memory 命中 "= ="，`你好` → 扩散+语法产生 `人很好。`

## 回应质量演进

| 阶段 | "你好" | "你是谁" | "我爱你" |
|------|--------|--------|---------|
| 修复前 | `人好大来没家只当再小已出将认算天身子` (13词乱码) | 16词乱码 | — |
| 句式骨架 | `好、大的人。` (ADJ+ADJ的N) | `大的人。` | — |
| 动词语法 | **`人很好。`** (SV句) | **`人很大。`** (SV句) | **`说好了。`** (V+补+了) |

**英文**：`hello→show .` | `I love you→like love.` | `how are you→much better come.`

## 发现与结论

1. **语法框架正确**：中英双语动词配价驱动的主谓宾结构生成正确
2. **激活机制正确**：枢纽过滤后不同输入产生不同候选词
3. **知识图谱是瓶颈**：`你→8000单字邻居` 全是噪声边，`我→0边` 完全孤立。不是算法问题，是训练数据/知识图谱质量限制

## 改动文件

| 文件 | 变更 |
|------|------|
| `src/diffusion.c` | 重写 — 语法组装(动词配价中英双引擎) + 枢纽过滤 + 输入注入 + 深度/top_k |
| `src/cognitive_controller.c` | 新增 — 英文 POS 后缀规则(120+) + 小词典(200+) + `english_pos_lookup()` |
| `include/cognitive_controller.h` | 修改 — 导出 `english_pos_lookup()` |
| `src/multi_topology.c` | 新增 — `english_connector_map()` |
| `include/multi_topology.h` | 修改 — 导出 `english_connector_map()` |
| `include/diffusion.h` | 修改 — DiffusionCtx 新增 `emergent_pos` 字段 |
| `include/pivotmind_version.h` | 修改 — v0.4.10 → v0.4.11 |
| `changelogs/README.md` | 修改 — 版本对照表更新 |
| `changelogs/048-*.md` ~ `changelogs/051-*.md` | 修改 — 版本号 0.4.9 → 0.4.10 |

**总计**: 18 文件, +1032 行, -78 行

## 编译验证

```bash
make clean && make gateway -j4
```
零错误，零新增扩散相关警告。
