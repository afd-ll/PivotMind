#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""幂等生成器：把活文档里的版本串统一改写为真值源的值。

用法：
    python3 tools/sync_version_docs.py [--root <仓库根>] [--dry-run] [--quiet]

语义：
    - 读 `include/pivotmind_version.h`（唯一真值源）；
    - 只改写活文档（README.md / README.zh-CN.md / ARCHITECTURE.md）里的**版本串锚点**，
      即「声明当前版本」的那 4 种固定形态（badge / 正文当前版本句 / 指标表版本行 / 架构抬头）；
    - ⛔ 历史一律不动：changelogs/**、CHANGELOG.md 的历史节、docs/**、
      以及活文档里的历史叙述（如 "v0.5.21 reset baseline"）都不是锚点，永不匹配；
    - **幂等**：值已等于真值源时替换结果与原文逐字节相同 ⇒ 第二次运行零改动（脚本自证）。

退出码：0 = 成功（含「无改动」）；1 = 真值源异常 / IO 失败。
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import version_common as vc  # noqa: E402


def sync_doc(root, doc, version):
    """改写单份文档，返回 (改动条数, 明细列表)。只写回真的有变化的文件。"""
    anchors = vc.anchors_for(doc)
    if not anchors:
        return 0, []
    path = os.path.join(root, doc)
    if not os.path.isfile(path):
        return 0, []
    lines, _ = vc.read_lines(path)
    changed = []
    for idx, raw in enumerate(lines, 1):
        new = raw
        hit_anchor = None
        for anchor_id, regex, template in anchors:
            m = regex.search(new)
            if not m:
                continue
            if m.group("v") == version:
                continue
            hit_anchor = anchor_id
            new = regex.sub(template.format(v=version), new, count=1)
        if new != raw:
            changed.append((idx, hit_anchor or anchors[-1][0],
                            raw.rstrip("\r\n"), new.rstrip("\r\n")))
            lines[idx - 1] = new
    if not changed:
        return 0, []
    with open(path, "wb") as fh:
        fh.write("".join(lines).encode("utf-8"))
    return len(changed), changed


def main(argv=None):
    ap = argparse.ArgumentParser(description="把活文档版本串同步到真值源（幂等）")
    ap.add_argument("--root", default=None, help="仓库根（默认 = 本脚本的上级目录）")
    ap.add_argument("--dry-run", action="store_true", help="只打印将要改的内容，不落盘")
    ap.add_argument("--quiet", action="store_true", help="静默模式（无改动时不打印明细）")
    args = ap.parse_args(argv)

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        version, parts = vc.read_truth(root)
    except vc.TruthError as exc:
        print("SYNC-VERSION: ERROR %s" % exc, file=sys.stderr)
        return 1

    print("真值源 %s → PIVOTMIND_VERSION=\"%s\" (MAJOR.MINOR.PATCH=%s.%s.%s)"
          % (vc.TRUTH_REL, version, parts[0], parts[1], parts[2]))

    total = 0
    details = []
    for doc in vc.LIVE_DOCS:
        n, changed = sync_doc(root, doc, version)
        total += n
        for line_no, anchor_id, old, new in changed:
            details.append((doc, line_no, anchor_id, old, new))

    if details and not args.quiet:
        print("")
    for doc, line_no, anchor_id, old, new in details:
        print("  %s:%d  [%s]" % (doc, line_no, anchor_id))
        print("    - %s" % old)
        print("    + %s" % new)

    if args.dry_run:
        print("SYNC-VERSION: DRY-RUN 将改写 %d 处（未落盘）" % total)
        return 0

    if total == 0:
        print("SYNC-VERSION: NOOP 零改动（活文档已与真值源 %s 一致，幂等自证）" % version)
    else:
        print("SYNC-VERSION: 已改写 %d 处（活文档 → %s）" % (total, version))

    # 提醒（不改写）：CHANGELOG 最新节若有陈旧版本头断言，需要人工补带日期的更正注记
    stale = vc.scan_changelog_correction(root, version)
    if stale:
        print("")
        print("提示：%s 还有 %d 处陈旧版本头断言且缺带日期的更正注记（脚本刻意不改写历史）："
              % (vc.CHANGELOG_REL, len(stale)))
        for item in stale:
            print("    %s:%d  %s" % (item["file"], item["line"], item["old_line"]))
        print("    请人工在该行后追加形如「更正（YYYY-MM-DD）：… 已 bump 至 %s」的注记。"
              % version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
