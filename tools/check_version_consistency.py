#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""秒级门禁：活文档版本串必须等于真值源 `include/pivotmind_version.h`。

用法：
    python3 tools/check_version_consistency.py [--root <仓库根>] [--quiet]

扫描面：
    活文档（受管）  README.md / README.zh-CN.md / ARCHITECTURE.md
    历史（排除）    CHANGELOG.md 历史节 / changelogs/** / docs/** / 活文档里的历史叙述

判定：
    - 活文档中**声明当前版本**的 4 种锚点（badge URL / 正文当前版本句 / 指标表版本行 /
      架构抬头）只要出现与真值源不同的版本串 ⇒ 记一条不一致；
    - CHANGELOG.md 的**最新发布节**若提到 `pivotmind_version.h` 却写着过期的版本串，
      且该节内没有带日期（`更正（YYYY-MM-DD）`）且写明当前真值版本的更正注记 ⇒ 记一条不一致。

输出：
    不一致时逐条打印 `文件:行  现值=…  期望=…`，退出码 1；
    一致时打印 `VERSION-CONSISTENCY: PASS …`，退出码 0。
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import version_common as vc  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description="活文档版本串 vs 真值源一致性门禁")
    ap.add_argument("--root", default=None, help="仓库根（默认 = 本脚本的上级目录）")
    ap.add_argument("--quiet", action="store_true", help="一致时只打印一行结论")
    args = ap.parse_args(argv)

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        version, parts = vc.read_truth(root)
    except vc.TruthError as exc:
        print("VERSION-CONSISTENCY: ERROR %s" % exc, file=sys.stderr)
        return 2

    _, violations = vc.collect_violations(root, version)

    if violations:
        print("VERSION-CONSISTENCY: FAIL  真值源 %s（%s）— 检出 %d 处不一致："
              % (version, vc.TRUTH_REL, len(violations)))
        for item in violations:
            if item["anchor"] == "missing-file":
                print("  %s  <缺失>  期望值=\"%s\"" % (item["file"], item["expected"]))
            else:
                print("  %s:%d  现值=\"%s\"  期望值=\"%s\""
                      % (item["file"], item["line"], item["current"], item["expected"]))
                if item["old_line"]:
                    print("      - %s" % item["old_line"])
                if item["new_line"]:
                    print("      + %s" % item["new_line"])
        print("")
        print("修法：python3 tools/sync_version_docs.py   （CHANGELOG 的更正注记需人工追加）")
        return 1

    spans = sum(1 for doc in vc.LIVE_DOCS for _ in vc.anchors_for(doc))
    print("VERSION-CONSISTENCY: PASS  真值源 %s = %s；活文档 %d 份 / 锚点 %d 处一致；"
          "CHANGELOG 最新节无未更正的陈旧断言"
          % (vc.TRUTH_REL, version, len(vc.LIVE_DOCS), spans))
    return 0


if __name__ == "__main__":
    sys.exit(main())
