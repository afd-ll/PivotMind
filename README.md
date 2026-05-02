# 玄枢 (PivotMind) — 溯智网络 AI 框架

一个用纯C语言实现的AI框架，支持神经网络训练、中文NLP、智能阅读系统和AI词典学习。

## 项目结构

```
ai_C/
├── src/              # 核心源代码（28个文件）
│   ├── tensor.c      # 张量计算
│   ├── model.c       # 模型实现
│   ├── layer_*.c     # 网络层（RNN/LSTM/GRU）
│   ├── chinese.c     # 中文处理
│   └── ...
│
├── include/          # 头文件（26个文件）
├── examples/         # 示例程序（44个文件）
├── tests/            # 测试代码（15个文件）
├── tools/            # 工具脚本
├── docs/             # 文档
├── data/             # 数据文件
├── scripts/          # 构建脚本
├── logs/             # 日志目录
├── build/            # 构建输出
├── build_cmake/      # CMake构建
│
├── ai_auto_train.exe           # AI词典学习系统（最新版本）
├── ai_dictionary_learner.c     # AI学习系统源代码
├── dictionary.c/h              # 词典模块
├── cedict.db                   # CC-CEDICT词典数据库（28.7 MB）
├── main.c                      # 主程序入口
├── CMakeLists.txt              # CMake配置
└── Makefile                    # Make配置
```

## 核心功能

### 1. AI词典学习系统 🆕
- **AI自主学习**: 完全自动化的学习和测试流程
- **溯智网络拓扑**: 可视化学习状态和知识图谱
- **真实词典数据**: 集成CC-CEDICT词典（124,465个词汇）
- **置信度追踪**: 动态评估学习效果
- **多轮训练**: 自动复习和强化学习

### 2. 深度学习框架
- 张量计算引擎
- 神经网络层（RNN/LSTM/GRU）
- 优化器（SGD/Adam）

### 3. 中文NLP
- 中文分词器
- UTF-8字符处理
- CC-CEDICT词典集成

### 4. 智能阅读系统
- 古文分词
- 智能查词
- 语境理解

## 快速开始

### 运行AI词典学习系统 🆕
```bash
.\ai_auto_train.exe
```

**推荐训练流程**:
1. 输入 `1` - 选择"AI自主学习+测试"
2. 选择 `1` - 快速训练（100词 x 3轮）
3. 等待完成 - AI自动完成所有训练
4. 查看结果 - 自动显示学习统计

### 构建项目

```bash
# 使用 Makefile
make

# 使用 CMake
mkdir build_cmake && cd build_cmake
cmake ..
cmake --build .
```

## AI词典学习系统功能

### 自动化训练（推荐）
- **1. AI自主学习+测试**: 完全自动化的训练流程
- **2. 批量自主学习**: 指定数量的大规模学习（200-2000词）
- **3. 多轮训练**: 学习→测试→复习循环

### 训练效果示例

```
训练轮次 1/3:
  学习: 100个新词汇
  准确率: 40.00%
  
训练轮次 2/3:
  复习已学词汇
  准确率: 67.00% ⬆️
  
训练轮次 3/3:
  复习已学词汇
  准确率: 85.00% ⬆️⬆️
```

### 核心算法

**置信度计算**:
```
confidence = accuracy × 0.7 + learn_bonus
accuracy = correct_count / test_count
learn_bonus = min(learn_count × 0.05, 0.3)
```

**记忆强度（AI自动测试）**:
```
memory_strength = confidence × 0.5 + 
                 learn_count × 0.03 + 
                 activation × 0.3
```

### 编译AI学习系统

```bash
gcc -o ai_auto_train.exe ai_dictionary_learner.c dictionary.c \
    -I./libs -L./libs -lsqlite3 -lm -O2
```

## 文档

- **AI学习系统报告**: [docs/LEARNING_SYSTEM_REPORT.md](docs/LEARNING_SYSTEM_REPORT.md)
- **代码质量报告**: [docs/CODE_QUALITY_REPORT.md](docs/CODE_QUALITY_REPORT.md)
- **学习系统说明**: [docs/README_LEARNER.md](docs/README_LEARNER.md)
- **项目清理报告**: [docs/CLEANUP_REPORT_20260331.md](docs/CLEANUP_REPORT_20260331.md)
- [开发日记](docs/DEVELOPMENT_DIARY.md)
- [记忆模块设计](docs/MEMORY_MODULE_DESIGN.md)

## 数据来源

CC-CEDICT词典数据库：
- 词汇数量: 124,465个
- 文件大小: 28.7 MB
- 格式: SQLite3数据库

## 依赖项

- **SQLite3** - 词典数据库
- **C标准库** - math.h, stdio.h等
- **GCC/Clang** - C编译器

## 示例程序

查看 `examples/` 目录，包含：
- 基础神经网络示例
- 词典查询示例
- 华容道AI
- PDF解析

## 测试

- `tests/unit/` - 单元测试
- `tests/integration/` - 集成测试
- `tests/fixtures/` - 测试数据

## 许可证

本项目仅供学习和研究使用。

## 作者

凌文清（数字分身）

## 开发进度

- ✅ 核心框架
- ✅ 中文NLP
- ✅ 词典系统
- ✅ AI词典学习系统 🆕
- 🔄 智能阅读系统
- 📋 知识内化系统

---

**最后更新**: 2026-03-31
