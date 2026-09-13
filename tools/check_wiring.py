#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""接线门禁 v2：两类「看着接了、其实没接」的静默空转。

判据 A —— **零引用**：函数有定义/声明，但全仓找不到任何引用点。
判据 B —— **显式丢弃**：函数体里写 `(void)参数;`，参数被主动扔掉
          （高概率「接线了但没用上」，v0.5.39 的 `perception_tick` 就是这么发现的）。

用法：
    python3 tools/check_wiring.py [--root <仓库根>] [--verbose] [--emit-baseline]

── 判据 A 的「引用」认定（2026-09-13 关掉两个盲区）────────────────────────────
  · **命名族放宽**：不再只认 `init_*` / `ensure_*` / `*_from_env` / `*_config` 这四族，
    改为「名字里**含** init|ensure|config|setup|create|default|build|make」。
    旧判据漏检 `pretrain_config_create_default`（关键词在中间）这类。
  · **函数指针引用算引用**：任何「非声明 / 非定义」的出现都算引用，**不要求后面跟 `(`**。
    旧判据只认 `name(`，把 `pthread_once(&k, activation_key_init)` 这种
    **取地址 / 回调注册** 当成零引用 ⇒ 假死报。

── 判据 B 的由来 ───────────────────────────────────────────────────────
  静态检测「恒返回常量」在本仓**不可行**（实测只捞到 2 个 OpenMP 兜底桩，
  真案例 `is_valid_query` 反而漏掉 —— 它有循环有真逻辑，只是判据对其定义域恒假）。
  能捞到真案例的是 `(void)参数;` 显式丢弃扫描，故以此为准。

── 基线 / 白名单 ──────────────────────────────────────────────────────
  `tools/wiring_whitelist.txt` —— **有意为之**的零引用（对外 API / 回调 / 宏注册）。
     含义：「结论：这样是对的」。命中 ⇒ ① 豁免，不计退出码。
  `tools/wiring_baseline.txt` —— **已知欠账**（真需要处置、但还没处置的）。
     含义：「知道，还没做」。命中 ⇒ ② 静默（只计数）。
  **不在白名单、也不在基线的命中 ⇒ ③ 新增，退出码 1。**
  基线条目若已不再命中 ⇒ ④ 打印「过期」，提示清理（不失败）。
  ⇒ 门禁对**新增**回归有牙齿，又不必把已知欠账谎称「有意豁免」。

退出码：有 ③ 新增命中 ⇒ 1；否则 0。
"""

import argparse
import collections
import os
import re
import sys

# 只扫这些目录与扩展名（与 skill 的 deadcode_scan.py 一致）
SCAN_DIRS = ["src", "include", "tools", "demos", "tests"]
EXTS = (".c", ".h")
C_EXTS = (".c",)

# ── 判据 A：险类关键词（名字里含其一即纳入候选池）──
HAZARD_KW = "init|ensure|config|setup|create|default|build|make"
NAME_RE = re.compile(r"\b([A-Za-z0-9_]*(?:%s)[A-Za-z0-9_]*)\s*\(" % HAZARD_KW)

# 启发式的「类型关键字」集合（保留：用于「这行是不是定义」的粗判）
TYPEKW = (
    "void", "int", "float", "double", "char", "bool", "size_t", "long", "short",
    "unsigned", "signed", "struct", "const", "inline", "static", "uint", "int8_t",
    "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "PmLang", "MasterTopology", "SubTopology", "ReasoningNode",
)

# 出现行里，名字之前一旦含这些字符 ⇒ 表达式上下文（调用），而非声明/定义
_EXPR_CHARS = set("=()+-/%&|!?:;,<>[]{}")

WL_REL = "tools/wiring_whitelist.txt"
BL_REL = "tools/wiring_baseline.txt"


# ══════════════════════════════════════════════════════════════════════
# 基础工具
# ══════════════════════════════════════════════════════════════════════

def _is_comment_line(t):
    s = t.lstrip()
    return s.startswith("//") or s.startswith("*") or s.startswith("/*")


def classify(line, name, idx):
    """给定「name 在 line 的第 idx 个字符处」，判定 kind。

    'def' / 'decl' / 'use' / 'comment'
    ⚠️ 注释判据只看**行首**或名字**之前**是否有 `//`；绝不能用 `pre.endswith("*")`
       —— 那会把指针返回类型 `LSTMLayer* foo(` 误判成注释续行。
    """
    if idx < 0:
        return "use"
    if _is_comment_line(line) or "//" in line[:idx]:
        return "comment"
    pre = line[:idx].strip()
    post = line[idx + len(name):].lstrip()
    if not post.startswith("("):
        return "use"            # 取地址 / 函数指针 / 赋值 ⇒ 算引用
    if not pre or pre.endswith("return"):
        return "use"
    if any(ch in _EXPR_CHARS for ch in pre):
        return "use"
    return "decl" if line.rstrip().endswith(";") else "def"


def classify_heur(line, rel, name):
    """粗判（仅用于建候选池）：'def' / 'decl' / 'call'。"""
    idx = line.find(name)
    prefix = line[:idx]
    if prefix.strip().endswith("*") or any(k in prefix for k in TYPEKW):
        return "def"
    if rel.endswith(".h") and line.rstrip().endswith(";"):
        return "decl"
    return "call"


def read_lines(root):
    """{rel: [行文本]}（只含 SCAN_DIRS 下的 .c/.h）。"""
    out = {}
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
                    out[rel] = open(p, encoding="utf-8", errors="replace").read().splitlines()
                except OSError:
                    continue
    return out


def load_kv(path):
    """读「一行一条、`#` 起注释」的文件 → {key: 理由}。key 保留原始多词形式。"""
    items = {}
    if not os.path.isfile(path):
        return items
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            body = raw.split("#", 1)[0].strip()
            if not body:
                continue
            reason = raw.split("#", 1)[1].strip() if "#" in raw else ""
            items[body] = reason
    return items


# ══════════════════════════════════════════════════════════════════════
# 判据 A：零引用
# ══════════════════════════════════════════════════════════════════════

def scan_zero_ref(lines_by_file):
    """返回 {name: [(rel, lineno, kind), ...]}，只含「零引用」者。"""
    # 1) 候选池：名字含险类关键词，且至少有一处「像定义」的出现
    cand = set()
    for rel, lines in lines_by_file.items():
        for ln in lines:
            for m in NAME_RE.finditer(ln):
                n = m.group(1)
                if classify_heur(ln, rel, n) == "def":
                    cand.add(n)
    if not cand:
        return {}

    # 2) 一次扫全仓：任何非 def/decl 的出现都算引用
    pat = re.compile(r"\b(%s)\b" % "|".join(
        re.escape(n) for n in sorted(cand, key=len, reverse=True)))
    uses = collections.defaultdict(list)
    defs = collections.defaultdict(list)
    for rel, lines in lines_by_file.items():
        for i, ln in enumerate(lines, 1):
            for m in pat.finditer(ln):
                n = m.group(1)
                k = classify(ln, n, m.start(1))
                if k == "use":
                    uses[n].append((rel, i))
                elif k in ("def", "decl"):
                    defs[n].append((k, rel, i))

    out = {}
    for n in cand:
        if n not in defs or uses.get(n):
            continue
        det = [(rel, i, k) for (k, rel, i) in defs[n]]
        # D = 悬空声明（只有声明、全仓无定义）⇒ 比「有定义没人调」更硬的一类
        out[n] = ("A" if any(k == "def" for k, _r, _i in defs[n]) else "D", det)
    return out


# ══════════════════════════════════════════════════════════════════════
# 判据 B：显式丢弃 `(void)参数;`
# ══════════════════════════════════════════════════════════════════════

DEF_FUNC_RE = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([A-Za-z_]\w*)\s*\(([^;{)]*)\)\s*\{", re.M)
VOID_CAST_RE = re.compile(r"\(void\)\s*([A-Za-z_]\w*)\s*;")


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return "\n".join(ln[:ln.find("//")] if "//" in ln else ln for ln in text.splitlines())


def _brace_body(text, open_idx):
    depth = 0
    for i in range(open_idx, len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:i]
    return None


def _params_of(param_text):
    """从形参表文本里抽参数名（取每个参数的最后一段标识符）。"""
    names = []
    for part in param_text.split(","):
        part = part.strip()
        if not part or part == "void":
            continue
        ids = re.findall(r"[A-Za-z_]\w*", part)
        if ids:
            names.append(ids[-1])
    return names


def scan_discard(lines_by_file):
    """返回 {"func param": [(rel, lineno, param), ...]}。"""
    hits = collections.defaultdict(list)
    for rel, lines in lines_by_file.items():
        if not rel.endswith(C_EXTS):
            continue
        raw = "\n".join(lines)
        text = _strip_comments(raw)
        for m in DEF_FUNC_RE.finditer(text):
            fname = m.group(1)
            params = _params_of(m.group(2))
            if not params:
                continue
            body = _brace_body(text, text.find("{", m.start()))
            if not body:
                continue
            ln = raw[:m.start()].count("\n") + 1
            for vm in VOID_CAST_RE.finditer(body):
                p = vm.group(1)
                if p in params:
                    hits["%s %s" % (fname, p)].append((rel, ln, p))
    return dict(hits)


# ══════════════════════════════════════════════════════════════════════
# main
# ══════════════════════════════════════════════════════════════════════

def main(argv=None):
    ap = argparse.ArgumentParser(description="接线门禁：零引用 / 显式丢弃")
    ap.add_argument("--root", default=None, help="仓库根（默认 = 本脚本的上级目录）")
    ap.add_argument("--whitelist", default=None)
    ap.add_argument("--baseline", default=None)
    ap.add_argument("--verbose", action="store_true", help="连基线命中的明细一起列")
    ap.add_argument("--emit-baseline", action="store_true",
                    help="把当前全部命中打成基线格式（用于初始化基线文件）")
    args = ap.parse_args(argv)

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    wl_path = args.whitelist or os.path.join(root, WL_REL)
    bl_path = args.baseline or os.path.join(root, BL_REL)

    lines_by_file = read_lines(root)
    zero = scan_zero_ref(lines_by_file)
    disc = scan_discard(lines_by_file)

    # 键统一为 `<类> <名字>`（与基线/白名单文件一一对应，便于人工核对）
    hits = {}
    for n, (kind, det) in zero.items():
        hits["%s %s" % (kind, n)] = (kind, det)
    for k, det in disc.items():
        hits["B %s" % k] = ("B", det)

    if args.emit_baseline:
        for key in sorted(hits):
            print(key)
        return 0

    whitelist = load_kv(wl_path)
    baseline = load_kv(bl_path)

    exempt, base_hit, new_hit = [], [], []
    for key in sorted(hits):
        if key in whitelist:
            exempt.append(key)
        elif key in baseline:
            base_hit.append(key)
        else:
            new_hit.append(key)

    dup = [k for k in hits if k in whitelist and k in baseline]
    stale = [k for k in baseline if k not in hits]

    def rel_disp(p):
        return os.path.relpath(p, root).replace(os.sep, "/") if \
            os.path.abspath(p).startswith(os.path.abspath(root) + os.sep) else p

    print("=== make check-wiring · 接线门禁 v2（零引用 / 显式丢弃） ===")
    print("扫描面: %s 下的 .c/.h" % " ".join(d + "/" for d in SCAN_DIRS))
    print("判据A : 名字含 [%s] 的函数，**全仓零引用** = 可疑" % HAZARD_KW.replace("|", "/"))
    print("        引用认定：任何非声明/非定义的出现都算（**含无括号的函数指针引用**）")
    print("判据D : 同上候选池里**只有声明、全仓无定义**的（悬空声明 ⇒ 一调就链接失败）")
    print("判据B : 函数体里 `(void)形参;` **显式丢弃**（接线了但没用上）")
    print("白名单: %s （有意为之；%d 条生效）" % (rel_disp(wl_path), len(whitelist)))
    print("基线  : %s （已知欠账，静默；%d 条生效）" % (rel_disp(bl_path), len(baseline)))
    print()

    print("── ① 白名单豁免（有意为之，不计退出码）: %d 个 ──" % len(exempt))
    for key in exempt:
        print("  ✓ %s   [%s]" % (key, whitelist.get(key) or "白名单豁免"))
    if not exempt:
        print("  （无）")
    print()

    print("── ② 基线命中（已知欠账，静默）: %d 个 ──" % len(base_hit))
    if args.verbose:
        for key in base_hit:
            det = hits[key][1]
            print("  · %s   @ %s:%s   [%s]"
                  % (key, det[0][0], det[0][1], baseline.get(key) or ""))
    else:
        ca = sum(1 for k in base_hit if hits[k][0] in ("A", "D"))
        cb = sum(1 for k in base_hit if hits[k][0] == "B")
        print("  零引用(A有定义 %d / D悬空声明 %d) / 显式丢弃(B) %d（明细见 %s；用 `--verbose` 当场列）"
              % (ca - sum(1 for k in base_hit if hits[k][0] == "D"),
                 sum(1 for k in base_hit if hits[k][0] == "D"), cb, rel_disp(bl_path)))
    print()

    print("── ③ 新增命中（会失败）: %d 个 ──" % len(new_hit))
    for key in new_hit:
        print("  ✗ %s" % key)
        for d in hits[key][1]:
            print("        %s" % " ".join(str(x) for x in d))
    if not new_hit:
        print("  （无）")
    print()

    if stale:
        print("── ④ 基线过期（已不再命中，建议从基线删除）: %d 条 ──" % len(stale))
        for key in stale:
            print("  ~ %s" % key)
        print()
    if dup:
        print("── ⚠️ 同一条同时出现在白名单与基线（应只留白名单）: %d 条 ──" % len(dup))
        for key in dup:
            print("  ! %s" % key)
        print()

    if new_hit:
        print("check-wiring: FAIL  新增命中 %d 个（白名单 %d / 基线 %d）；退出码 1"
              % (len(new_hit), len(exempt), len(base_hit)))
        print("修法：接线或删除；确属有意为之 ⇒ 登记 %s；已知欠账 ⇒ 登记 %s。"
              % (rel_disp(wl_path), rel_disp(bl_path)))
        return 1
    print("check-wiring: PASS  新增 0 个（白名单 %d / 基线 %d）；退出码 0"
          % (len(exempt), len(base_hit)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
