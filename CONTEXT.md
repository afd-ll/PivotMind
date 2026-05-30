# PivotMind 项目上下文记录

> 创建: 2026-05-25 | 工具: CodeWhale

## 路径

- **主目录**: `D:\work\玄枢-pivotmind`

## 目录

| 目录 | 用途 |
|------|------|
| include/ | 58个头文件 |
| src/ | 57个核心源文件 (~5.4万行) |
| tools/ | 工具脚本 |
| data/ | 运行时数据 (.bin .dat .json) |
| build/ | 编译产物 |
| libs/ | 第三方库 (sqlite3) |
| logs/ | 运行日志 |
| scripts/ | 构建脚本 (.bat .sh) |
| archive/ | 归档 |
| reports/ | 审查与修复报告 (NNN-A / NNN-B) |
| changelogs/ | 改动记录 (NNN-xxx.md) |

## 规则

1. 每次代码改动 -> changelogs/NNN-简短描述.md (三位编号递增)
2. 编号配对: 审查为 NNN-A, 修复为 NNN-B, 一次审查对应一次修复
3. 代码审查 -> reports/NNN-A.md, 修复报告 -> reports/NNN-B.md
4. 编号可追溯，互相关联
5. 一切操作在 `D:\work\玄枢-pivotmind` 内完成
6. 审查完的代码修复之后必须在对应修复报告中写明已修复项
