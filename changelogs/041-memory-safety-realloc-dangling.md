# 041 — realloc 悬空指针全面修复

## 版本
`0.4.2`

## 问题
ASAN 连续报告多起 heap-use-after-free，崩溃栈均指向 `article_flush` → `_ar_build_topo` → `huarong_net_find_concept` → `strcmp`。经三轮深入排查，根因为 **realloc 搬迁底层数组后，上层缓存的裸指针未刷新**，以及 **结构体直接存储外部裸指针而非拷贝** 两类模式缺陷。

## 修复

### 第一轮：`article_flush` 三字扩展首字索引悬空

**提交**：`79576f3`, `f0d6392`

| 文件 | 问题 |
|------|------|
| `src/article_reader.c` | `first_char_map` 缓存 `WordEntry**` 指向 `ar->words`，`_ar_find_or_add_word` 内 `realloc(ar->words)` 搬迁后全部悬空 |

**修复**：
- `CharWordList` 从 `WordEntry**` 改为 `int*`，存索引而非指针
- `w2` 每次通过 `&ar->words[widx2]` 实时取地址
- `w1` 在内层循环中也改为 `ar->words[wi].text`（前次修复遗漏）

### 第二轮：`is_punctuation` 传 char 值当指针

**提交**：`ab36b56`

| 文件 | 问题 |
|------|------|
| `src/article_reader.c:278` | `is_punctuation(const char*)` 接收指针，但传入 `*p`（单个 char 值），被隐式转换为非法低地址指针 |

### 第三轮：`concept_hash` 存裸指针

**提交**：`57040df`

| 文件 | 问题 |
|------|------|
| `src/huarong_topology.c:219` | `net->concept_hash[h].name = name` 存裸指针，`_ar_build_topo` 传入 `we->text` 在 `ar->words` 内，realloc 后悬空 |

**修复**：`strdup(name)` 接管所有权，`huarong_net_destroy` 遍历释放。

### 第四轮：全仓内存安全审计

**提交**：`36c99aa`, `d3e1e31`

#### 高危：裸指针存储未拷贝

| 文件 | 问题 | 修复 |
|------|------|------|
| `src/concept_abstraction.c` | `node->name = name`（栈缓冲区）不拷贝 | `strdup`，`concept_node_destroy` 加 `free` |
| `src/cognitive_controller.c` | `cc->current_input = input` 仅引用不拥有 | `set_context` 改 `strdup`，`reset_round` 加 `free` |
| `src/catastrophic_forgetting.c` | `domain->name/description` 不拷贝 | `strdup`，`destroy` 加 `free` |
| `src/multi_topology.c:2980` | `char** tokens = NULL` 传给 `utf8_tokenize` | 改为栈数组 `char* tokens[100]` |

#### 中危：连续 realloc / 无 NULL 检查

| 文件 | 问题 | 修复 |
|------|------|------|
| `src/causal_reasoning.c:505` | `free(pool_alloc edge)` double-free | 移除 `free`，仅置 NULL |
| `src/causal_reasoning.c:472,1523,807` | 3 处连续 realloc 无检查 / malloc 无检查 | 加临时变量 + NULL 检查 |
| `src/cognitive_controller.c:1137,1208` | 2 处 realloc 无 NULL 检查 | 加检查 + return -1 |
| `src/string_pool.c:88` | 三重 realloc 部分失败后池悬空 | `malloc+memcpy+free` 替代链式 realloc |
| `src/pretrain.c:1464` | 4 次连续 realloc，部分成功状态不一致 | 标注风险 |
| `src/template_builder.c:286` | 三重 realloc 失败路径 `nc/ir` 泄漏 | 失败时先 `free(nc); free(ir); free(ct)` |
| `src/multi_topology.c:212` | 双重 realloc 部分成功悬空 | `malloc+memcpy+free` 替代 |
| `src/concept_abstraction.c:158` | 先扩 nodes 后扩 id_index，失败时容量不匹配 | 先扩 id_index 再扩 nodes |

## 模式总结

整个修复覆盖了三大类 realloc 安全模式缺陷：

1. **缓存裸指针 → 改用索引**：`struct { int* list }` 替代 `struct { T** list }`，每次 `&base[index]` 实时取址
2. **直接存外部指针 → strdup 接管**：哈希表/结构体字段存储字符串时，`strdup` 拷贝，destroy 时 `free`
3. **链式 realloc → 原子化**：用 `malloc+memcpy+free` 替代多重 realloc，任一失败不破坏原数组
