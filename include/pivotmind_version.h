/**
 * @file pivotmind_version.h
 * @brief 玄枢 (PivotMind) 版本信息
 *
 * 语义联想引擎 — 非 Transformer 路径
 *
 * 版本格式: MAJOR.MINOR.PATCH
 *   MAJOR: 架构性重大变更（接口不兼容）
 *   MINOR: 功能新增（向前兼容）
 *   PATCH: 故障修复/小改进
 *
 * 状态文件格式版本见 multi_topology.c STATE_FORMAT_VERSION
 */

#ifndef PIVOTMIND_VERSION_H
#define PIVOTMIND_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

#define PIVOTMIND_VERSION       "0.4.5"
#define PIVOTMIND_MAJOR         0
#define PIVOTMIND_MINOR         4
#define PIVOTMIND_PATCH         5

/* 版本字符串（编译时间戳自动追加） */
#define PIVOTMIND_VERSION_FULL  PIVOTMIND_VERSION " (" __DATE__ " " __TIME__ ")"

#ifdef __cplusplus
}
#endif

#endif /* PIVOTMIND_VERSION_H */
