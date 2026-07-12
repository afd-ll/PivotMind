# 贡献指南

欢迎为玄枢贡献力量！

## 快速开始

```bash
git clone https://github.com/afd-ll/PivotMind.git
cd PivotMind
make -j$(nproc)
./build/bin/pivotmind_gateway 8899 .
```

## 提交规范

```
type: 简短标题

类型: feat/fix/docs/refactor/perf/chore
```

## 测试

```bash
make test    # 全部单元测试
```

提交前请确保 `make test` 全部通过。

## 版本号

`X.Y.Z` — X=发布版本, Y=架构版本, Z=变动计数。详见 `changelogs/README.md`。

## 行为准则

- 保持代码风格一致（C99）
- 新增功能需写 changelog
- 破坏性变更需更新版本号
