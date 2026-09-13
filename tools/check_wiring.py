#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""接线门禁：列出「有定义/声明、零调用点」的函数（疑似「写了一半没接线」的死代码）。

用法：
    python3 tools/check_wiring.py [--root <仓库根>] [--whitelist <文件>] [--quiet]

判据（复用 pivotmind-diagnosis skill 的启发式，见 scripts/deadcode_scan.py）：
    一行里出现 `name(` 之前的部分含类型关键字 ⇒ 定义；`.h` 里以 `;` 结尾 ⇒ 声明；
    其余 ⇒ 调用。**零调用 = 可疑**。
    扫描面与函数族：`src/ include/ tools/ demos/ tests/` 下的 `.c/.h`，
    函数名限定 `init_*` / `ensure_*` / `*_from_env` / `*_config` 这一族
    （历史上「机制写一半、没接线」的高发族）。

复核（消除启发式的已知误报）：
    原启发式会把**缩进的调用**误判成定义（前缀里含 `int` 等类型关键字），
    例如 `        int added = some_helper(...);` 这种带 `=` 的缩进调用。
    因此每个候选都要在全仓做一次**逐行精确分类**（`classify_robust`）：
    只有「明确是声明/定义位置」的行才算 def/decl，含赋值符/操作符/实参逗号的
    一律算调用。**只要发现任一调用点，即判为误报并剔除**，只有复核通过的才报出来。

白名单：
    `tools/wiring_whitelist.txt`，一行一个函数名，`#` 起为注释。
    命中白名单的列为「① 豁免」、不计入退出码。

输出 / 退出码：
    分「① 白名单豁免」「② 未豁免命中」两类逐条打印（`文件:行号` + 复核计数）；
    有未豁免命中 ⇒ 退出码 1；全清 ⇒ 退出码 0。
"""

import argparse
import collections
import os
import re
import sys

# 只扫这些目录与扩展名（与 skill 的 deadcode_scan.py 一致）
SCAN_DIRS = ["src", "include", "tools", "demos", "tests"]
EXTS = (".c", ".h")

# 函数族判据（与 skill 的 deadcode_scan.py 同源）
NAME_RE = re.compile(
    r"\b((?:init|ensure)_[A-Za-z0-9_]+|[A-Za-z0-9_]+_from_env|[A-Za-z0-9_]+_config)\s*\(")

# 启发式的「类型关键字」集合（同 skill）
TYPEKW = (
    "void", "int", "float", "double", "char", "bool", "size_t", "long", "short",
    "unsigned", "signed", "struct", "const", "inline", "static", "uint", "int8_t",
    "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "PmLang", "MasterTopology", "SubTopology", "ReasoningNode",
)

# 复核分类器：`name` 之前的文本一旦含这些字符，就说明是「表达式上下文」而非声明/定义
# （含 '=' 赋值、'(' 条件/实参、',' 实参、各类操作符、';' 语句分隔）。
_EXPR_CHARS = set("=()+-/%&|!?:;,<>[]{}")


def classify_heur(line, rel, name):
    """skill 原启发式：'def' / 'decl' / 'call'（**含已知误报**，仅用于发现候选）。"""
    idx = line.find(name)
    prefix = line[:idx]
    if prefix.strip().endswith("*") or any(k in prefix for k in TYPEKW):
        return "def"
    if rel.endswith(".h") and line.rstrip().endswith(";"):
        return "decl"
    return "call"


def classify_robust(line, name):
    """复核用（更严）：只有明确处在声明/定义位置才算 def/decl，其余一律 call。

    判定「明确是声明/定义位置」的充要：
      · `name` 后面紧跟 `(`（排除注释/字符串里的提及）；
      · `name` 之前的文本非空、且不含任何表达式字符（`=` `(` `,` 操作符…）；
      · 不以 `return` 结尾（那是调用）。
    典型纠正：`int added = some_helper(...)` 的 pre 含 `=` ⇒ call。
    """
    idx = line.find(name)
    if idx < 0:
        return "call"
    tail = line[idx + len(name):].lstrip()
    if not tail.startswith("("):
        return "call"
    pre = line[:idx].strip()
    if not pre or pre.endswith("return"):
        return "call"
    if any(ch in _EXPR_CHARS for ch in pre):
        return "call"
    # 到这里 pre 只剩「类型 + 限定词 + 指针 *」
    return "decl" if line.rstrip().endswith(";") else "def"


def load_whitelist(path):
    """读白名单：一行一个函数名，`#` 起注释。返回 {name: 理由}。"""
    names = {}
    if not os.path.isfile(path):
        return names
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            names[line] = raw.split("#", 1)[1].strip() if "#" in raw else ""
    return names


def collect(root):
    """扫描全仓，返回 {name: [(rel, lineno, text, heur_kind), ...]}。"""
    occ = collections.defaultdict(list)
    for d in SCAN_DIRS:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for dp, _dn, fn in os.walk(base):
            for f in sorted(fn):
                if not f.endswith(EXTS):
                    continue
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, root).replace(os.sep, "/")
                try:
                    text = open(p, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                for i, ln in enumerate(text.splitlines(), 1):
                    for m in NAME_RE.finditer(ln):
                        n = m.group(1)
                        occ[n].append((rel, i, ln, classify_heur(ln, rel, n)))
    return occ


def main(argv=None):
    ap = argparse.ArgumentParser(description="接线门禁：有定义/声明、零调用点的函数")
    ap.add_argument("--root", default=None, help="仓库根（默认 = 本脚本的上级目录）")
    ap.add_argument("--whitelist", default=None, help="白名单文件路径")
    ap.add_argument("--quiet", action="store_true", help="精简输出")
    args = ap.parse_args(argv)

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    wl_rel = "tools/wiring_whitelist.txt"
    wl_path = args.whitelist or os.path.join(root, wl_rel)
    if os.path.abspath(wl_path).startswith(os.path.abspath(root) + os.sep):
        wl_disp = os.path.relpath(wl_path, root).replace(os.sep, "/")
    else:
        wl_disp = wl_path

    occ = collect(root)

    # 1) 发现（skill 判据）：有定义、零调用
    candidates = {}
    for name, v in occ.items():
        kinds = [e[3] for e in v]
        if "def" in kinds and "call" not in kinds:
            candidates[name] = v

    # 2) 复核（精确分类）：任一出现行是「调用」⇒ 误报剔除
    confirmed, dropped = {}, []
    for name, v in candidates.items():
        robust = [classify_robust(e[2], name) for e in v]
        if "call" in robust:
            dropped.append((name, v, robust))
        else:
            confirmed[name] = (v, robust)

    # 3) 白名单分流
    whitelist = load_whitelist(wl_path)
    exempt, alarm = {}, {}
    for name in sorted(confirmed):
        (exempt if name in whitelist else alarm)[name] = confirmed[name]

    # 4) 输出
    print("=== make check-wiring · 接线门禁（有定义/声明、零调用点） ===")
    print("扫描面: %s 下的 .c/.h" % " ".join(d + "/" for d in SCAN_DIRS))
    print("判据  : 「name( 前缀含类型关键字⇒定义；.h 以 ; 结尾⇒声明；其余⇒调用」，零调用=可疑")
    print("复核  : 每条候选全仓逐行精确分类，发现任一调用点即剔除（消除缩进调用误报）")
    print("白名单: %s（%d 条生效）" % (wl_disp, len(whitelist)))
    print()

    def dump(name, v, robust, mark):
        locs = []
        for (rel, lineno, _t, _k), kind in zip(v, robust):
            locs.append("%s %s:%d" % (kind, rel, lineno))
        print("  %s %s" % (mark, name))
        for loc in locs:
            print("        %s" % loc)
        print("        复核计数: %d 次" % len(v))

    print("── ① 白名单豁免（不计入报警）: %d 个 ──" % len(exempt))
    if exempt:
        for name in sorted(exempt):
            v, robust = exempt[name]
            print("  ✓ %s   [%s]" % (name, whitelist.get(name) or "白名单豁免"))
            for (rel, lineno, _t, _k), kind in zip(v, robust):
                print("        %s %s:%d" % (kind, rel, lineno))
            print("        复核计数: %d 次" % len(v))
    else:
        print("  （无）")
    print()

    print("── ② 未豁免命中（会报警）: %d 个 ──" % len(alarm))
    if alarm:
        for name in sorted(alarm):
            dump(name, alarm[name][0], alarm[name][1], "✗")
    else:
        print("  （无）")
    print()

    if dropped:
        print("── 复核剔除的误报（启发式命中但实为已接线）: %d 个 ──" % len(dropped))
        for name, v, robust in sorted(dropped):
            callers = ["%s:%d" % (e[0], e[1]) for e, k in zip(v, robust) if k == "call"]
            print("  ~ %s  ← 实际存在调用点: %s" % (name, ", ".join(callers)))
        print()

    if alarm:
        print("check-wiring: FAIL  未豁免命中 %d 个（白名单豁免 %d 个）；退出码 1"
              % (len(alarm), len(exempt)))
        print("修法：清掉死代码接线，或在 %s 里登记豁免理由。" % wl_disp)
        return 1
    print("check-wiring: PASS  无未豁免命中（白名单豁免 %d 个）；退出码 0" % len(exempt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
