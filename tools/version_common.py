#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""版本号「单一真值源」(SSOT) 共享库 —— sync 生成器与 check 门禁共用。

真值源（唯一）：
    include/pivotmind_version.h   →   PIVOTMIND_VERSION "X.Y.Z" + MAJOR/MINOR/PATCH

活文档（受管、必须等于真值源）：
    README.md / README.zh-CN.md / ARCHITECTURE.md

历史文档（**排除**，一律不得改写，也不得参与一致性判定）：
    CHANGELOG.md 的历史节 / changelogs/** / docs/** / tests/README.md 的历史叙述

设计要点
--------
1. 只认「版本串锚点」——即活文档中**声明当前版本**的那 4 种固定形态：
     - shields.io badge URL 的版本段      `badge/version-v0.5.25-blue.svg`
     - 正文当前版本句（英/中）            `**Current version: v0.5.25.**` / `**当前版本：v0.5.25。**`
     - 指标表版本行（英/中）              `| Version | `0.5.25` |` / `| 版本 | `0.5.25` |`
     - 架构文档抬头（中）                 `> 当前版本: **v0.5.0** — ...`
   锚点之外的任何版本串（历史叙述、changelog、状态文件格式 vN）**一概不碰**。
2. 逐行处理并 `splitlines(keepends=True)`，只替换锚点内部那一段 ⇒ 行尾（LF/CRLF）、
   其余字节逐字保留。这同时保证了**幂等**：值已相等时替换结果与原文逐字节相同。
3. 门禁另有一条「陈旧断言必须带更正注记」规则，只管 CHANGELOG.md 的**最新一个发布节**：
   若该节提到 `pivotmind_version.h` 却写着一个不等于真值源的版本串，则必须在该节内
   存在一条形如 `更正（YYYY-MM-DD）` 且写明当前真值版本的注记；否则判不一致。
   （历史节 + 未来不提及版本头的节都自然豁免，规则不会随版本 bump 而误报。）
"""

import os
import re
import sys

TRUTH_REL = "include/pivotmind_version.h"          # 唯一真值源
LIVE_DOCS = ("README.md", "README.zh-CN.md", "ARCHITECTURE.md")  # 受管活文档
CHANGELOG_REL = "CHANGELOG.md"                     # 仅用于「更正注记」规则

_VER = r"\d+\.\d+\.\d+"

# ---------------------------------------------------------------- 版本串锚点
# 每个锚点 = (anchor_id, 已编译正则, 带 {v} 占位的替换模板)
BADGE = ("badge",
         re.compile(r"(img\.shields\.io/badge/version-)v?(?P<v>" + _VER + r")(-blue\.svg)"),
         r"\g<1>v{v}\g<3>")
EN_CUR = ("current-version",
          re.compile(r"(\*\*Current version: )v?(?P<v>" + _VER + r")(\.\*\*)"),
          r"\g<1>v{v}\g<3>")
ZH_CUR = ("current-version",
          re.compile(r"(\*\*当前版本：)v?(?P<v>" + _VER + r")(。\*\*)"),
          r"\g<1>v{v}\g<3>")
EN_TBL = ("table-version",
          re.compile(r"(\| Version \| `)v?(?P<v>" + _VER + r")(` \|)"),
          r"\g<1>{v}\g<3>")
ZH_TBL = ("table-version",
          re.compile(r"(\| 版本 \| `)v?(?P<v>" + _VER + r")(` \|)"),
          r"\g<1>{v}\g<3>")
ARCH_CUR = ("arch-current",
            re.compile(r"(> 当前版本: \*\*)v?(?P<v>" + _VER + r")(\*\*)"),
            r"\g<1>v{v}\g<3>")


def anchors_for(doc):
    """返回某份活文档适用的锚点元组（其余文件为空，不会被误改）。"""
    base = os.path.basename(doc)
    if base == "ARCHITECTURE.md":
        return (ARCH_CUR,)
    if base == "README.zh-CN.md":
        return (BADGE, ZH_CUR, ZH_TBL)
    if base == "README.md":
        return (BADGE, EN_CUR, EN_TBL)
    return ()


# ---------------------------------------------------------------- 真值源
class TruthError(Exception):
    pass


def read_truth(root="."):
    """解析真值源；返回 (version, (major, minor, patch))。自洽性不成立即报错。"""
    path = os.path.join(root, TRUTH_REL)
    if not os.path.isfile(path):
        raise TruthError("找不到真值源文件：%s" % path)
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8")

    def grab(pattern, what):
        m = re.search(pattern, text, re.M)
        if not m:
            raise TruthError("真值源 %s 缺少 %s（正则 %s）" % (TRUTH_REL, what, pattern))
        return m.group(1)

    version = grab(r'^#define\s+PIVOTMIND_VERSION\s+"([^"]+)"\s*$', "PIVOTMIND_VERSION")
    major = grab(r'^#define\s+PIVOTMIND_MAJOR\s+(\d+)\s*$', "PIVOTMIND_MAJOR")
    minor = grab(r'^#define\s+PIVOTMIND_MINOR\s+(\d+)\s*$', "PIVOTMIND_MINOR")
    patch = grab(r'^#define\s+PIVOTMIND_PATCH\s+(\d+)\s*$', "PIVOTMIND_PATCH")

    if not re.fullmatch(_VER, version):
        raise TruthError('PIVOTMIND_VERSION "%s" 不是 MAJOR.MINOR.PATCH 形态' % version)
    composed = "%s.%s.%s" % (major, minor, patch)
    if version != composed:
        raise TruthError(
            '真值源自相矛盾：PIVOTMIND_VERSION="%s" 但 MAJOR/MINOR/PATCH=%s'
            % (version, composed))
    return version, (major, minor, patch)


# ---------------------------------------------------------------- 逐行扫描
def read_lines(path):
    with open(path, "rb") as fh:
        data = fh.read()
    encoding = "utf-8"
    return data.decode(encoding).splitlines(keepends=True), encoding


def scan_doc(root, doc, version):
    """扫一份活文档，返回不一致清单。

    每条 = dict(file, line, anchor, current, expected, old_line, new_line)
    """
    path = os.path.join(root, doc)
    if not os.path.isfile(path):
        return [dict(file=doc, line=0, anchor="missing-file",
                     current="<文件不存在>", expected=version,
                     old_line="", new_line="")]
    lines, _ = read_lines(path)
    out = []
    anchors = anchors_for(doc)
    for idx, raw in enumerate(lines, 1):
        for anchor_id, regex, template in anchors:
            m = regex.search(raw)
            if not m:
                continue
            current = m.group("v")
            if current == version:
                continue
            expected_line = regex.sub(template.format(v=version), raw, count=1)
            out.append(dict(file=doc, line=idx, anchor=anchor_id,
                            current=current, expected=version,
                            old_line=raw.rstrip("\r\n"),
                            new_line=expected_line.rstrip("\r\n")))
    return out


def scan_all_live(root, version):
    out = []
    for doc in LIVE_DOCS:
        out.extend(scan_doc(root, doc, version))
    return out


# ---------------------------------------------------------------- CHANGELOG 更正注记
_HEADING_RE = re.compile(r"^##\s+v?(\d+\.\d+\.\d+)\b")
_CORRECTION_RE = re.compile(r"更正（\d{4}-\d{2}-\d{2}）")
_VERSION_LITERAL_RE = re.compile(r"\"?v?(\d+\.\d+\.\d+)\"?")


def scan_changelog_correction(root, version):
    """最新发布节若断言了过期的版本头，必须带一条带日期的更正注记。

    只扫**最新一节**（第一个 `## vX.Y.Z` 到下一个 `## vX.Y.Z` 之前），
    因此历史节天然豁免；不提及 `pivotmind_version.h` 的节也天然通过。
    """
    path = os.path.join(root, CHANGELOG_REL)
    if not os.path.isfile(path):
        return []
    lines, _ = read_lines(path)

    start = None
    end = len(lines)
    for idx, raw in enumerate(lines):
        if _HEADING_RE.match(raw):
            if start is None:
                start = idx
            else:
                end = idx
                break
    if start is None:
        return []

    section = lines[start:end]
    stale = []
    for off, raw in enumerate(section):
        if "pivotmind_version.h" not in raw:
            continue
        # 本身就是一条带日期的更正注记 ⇒ 它引用的旧版本是「被更正的历史」，
        # 不是「当前版本的断言」，不再重复计入。
        if _CORRECTION_RE.search(raw):
            continue
        found = [v for v in _VERSION_LITERAL_RE.findall(raw) if v != version]
        if found:
            stale.append((start + off + 1, raw.rstrip("\r\n"), found))
    if not stale:
        return []
    if any(_CORRECTION_RE.search(raw) and version in raw for raw in section):
        return []

    out = []
    for line_no, raw, found in stale:
        out.append(dict(file=CHANGELOG_REL, line=line_no,
                        anchor="changelog-stale-claim",
                        current=",".join(found),
                        expected="%s + 带日期的更正注记" % version,
                        old_line=raw, new_line=""))
    return out


def collect_violations(root=".", version=None):
    if version is None:
        version, _ = read_truth(root)
    violations = scan_all_live(root, version)
    violations.extend(scan_changelog_correction(root, version))
    violations.sort(key=lambda d: (d["file"], d["line"]))
    return version, violations


def main_doc():
    return __doc__
