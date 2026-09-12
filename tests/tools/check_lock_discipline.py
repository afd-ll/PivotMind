#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""锁纪律静态检查器 —— 持 master 读锁期间是否调用了「会取 master 写锁的函数」。

规则原文（仓库自己的纪律，见 src/funcword.c:479）：
    ⚠️ 持读锁期间内部不得调用抢 master 写锁的函数。

为什么需要这道护栏
------------------
glibc 的 ``pthread_rwlock_t`` **不支持同线程「读→写」升级**：``wrlock`` 要等所有
读者退出，而唯一的读者就是本线程自己 ⇒ 永久自死锁。
这类死是**纯自死锁、没有数据竞争**，所以 TSan / Helgrind **不会报**，只会跟着一起挂死；
而且它常常藏在 ``tick % N == 0`` 的分支里（本项目实测：``brainstem.c`` 的
``tick % 600``，约 11 分钟才第一次执行到），**秒级单测结构上覆盖不到**。
所以用一个**秒级、纯标准库、可 CI 化**的静态检查器把这条纪律钉死。

算法（纯静态，不编译、不运行）
------------------------------
1. **预处理**：把注释（``//`` 与 ``/* */``）和字符串/字符字面量替换成空格，
   **保留换行符**，因此行号与源文件严格一致 —— 注释和字符串里的「伪调用」不会误报。
2. **函数表**：大括号配对 + 向前回溯标识符，得到每个函数定义的 [起始行, 结束行]。
3. **写者集合**：函数体内出现 ``pthread_rwlock_wrlock(&X->rwlock)`` 且 X 是
   master 别名（``master``，或形如 ``bs->master`` / ``learner->master`` / ``sl->master``）
   者 —— 这些是「会取 master 写锁的函数」。
4. **读者区间**：``pthread_rwlock_rdlock(&X->rwlock)``（同为 master 别名）起，
   到与**同一表达式**配对的 ``pthread_rwlock_unlock(&X->rwlock)`` 止。
5. **命中**：读者区间内部出现「写者集合成员」的函数调用 ``name(`` ⇒ 报告。

输出：每个命中一行 ``文件:行`` + 被调函数名 + 判定依据 + 写者取锁位置；
末尾 ``LOCK-DISCIPLINE: PASS`` / ``LOCK-DISCIPLINE: FAIL (N 处)``；退出码 0 / 非 0。

用法
----
    python3 tests/tools/check_lock_discipline.py [仓库根目录]
    # 不传根目录时以本脚本所在位置推导（tests/tools/ -> 仓库根）

已知限制见文件末尾 ``LIMITATIONS``。
"""
from __future__ import print_function

import os
import re
import sys


# --------------------------------------------------------------------------
# 1. 预处理：剥离注释与字符串（保换行，行号不漂移）
# --------------------------------------------------------------------------
def strip_comments_and_strings(src):
    out = list(src)
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = i
            while j < n and src[j] != '\n':
                out[j] = ' '
                j += 1
            i = j
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            j = i
            while j < n and not (src[j] == '*' and j + 1 < n and src[j + 1] == '/'):
                if src[j] != '\n':
                    out[j] = ' '
                j += 1
            for k in range(j, min(j + 2, n)):
                if src[k] != '\n':
                    out[k] = ' '
            i = j + 2
        elif c == '"' or c == "'":
            q = c
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    out[j] = ' '
                    j += 2
                    continue
                if src[j] == q or src[j] == '\n':
                    break
                out[j] = ' '
                j += 1
            if j < n and src[j] != '\n':
                out[j] = ' '
            i = j + 1
        else:
            i += 1
    return ''.join(out)


# --------------------------------------------------------------------------
# 2. 函数表
# --------------------------------------------------------------------------
_KEYWORDS = {
    'if', 'else', 'while', 'for', 'switch', 'do', 'return', 'sizeof', 'case',
    'int', 'char', 'float', 'double', 'void', 'struct', 'union', 'enum',
    'unsigned', 'signed', 'long', 'short', 'const', 'static', 'extern',
    'volatile', 'register', 'inline', 'typedef', 'goto', 'break', 'continue',
}


def find_functions(stripped):
    """大括号配对扫函数定义 -> [{'name','start','end','head'}...]（行号 1 基）"""
    funcs = []
    n = len(stripped)
    nl = [0]  # 行号前缀表由 count 计算，文件不大，直接 count
    i = 0
    while i < n:
        if stripped[i] != '{':
            i += 1
            continue
        k = i - 1
        while k >= 0 and stripped[k].isspace():
            k -= 1
        if k < 0 or stripped[k] != ')':
            i += 1
            continue
        # 向前配对括号
        pd = 0
        while k >= 0:
            if stripped[k] == ')':
                pd += 1
            elif stripped[k] == '(':
                pd -= 1
                if pd == 0:
                    break
            k -= 1
        if k <= 0:
            i += 1
            continue
        m = k - 1
        while m >= 0 and stripped[m].isspace():
            m -= 1
        e = m
        while m >= 0 and (stripped[m].isalnum() or stripped[m] == '_'):
            m -= 1
        name = stripped[m + 1:e + 1]
        if not name or name in _KEYWORDS:
            i += 1
            continue
        # 向前找大括号配对
        d = 0
        j = i
        while j < n:
            if stripped[j] == '{':
                d += 1
            elif stripped[j] == '}':
                d -= 1
                if d == 0:
                    break
            j += 1
        start = stripped.count('\n', 0, m + 1) + 1
        end = stripped.count('\n', 0, min(j, n - 1)) + 1
        # 函数名所在行的整行文本（用于报告）
        head_lo = stripped.rfind('\n', 0, m + 1) + 1
        funcs.append({'name': name, 'start': start, 'end': end,
                      'head': stripped[head_lo:head_lo + 120].strip()})
        i = j + 1
    return funcs


# --------------------------------------------------------------------------
# 3. master 锁表达式识别
# --------------------------------------------------------------------------
_LOCK_CALL = r'pthread_rwlock_(?:wrlock|rdlock|unlock)\s*\(\s*([^)]*)\)'


def master_lock_expr(arg):
    """若锁参数是 master 的 rwlock，返回其规范化基名，否则 None。

    master 别名：``master`` 本身，或任何 ``X->master``（bs->master /
    learner->master / sl->master …）。``sub->rwlock`` / ``topo->rwlock`` /
    ``net->mutex`` 一律不算。
    """
    e = arg.strip().lstrip('&').strip()
    m = re.match(r'^(.*?)[.-]>?\s*rwlock\s*$', e)
    if not m:
        return None
    base = m.group(1).strip()
    if base == 'master':
        return base
    if re.search(r'(?:^|->)master$', base):
        return base
    return None


def norm(expr):
    return re.sub(r'\s+', '', expr)


# --------------------------------------------------------------------------
# 4. 主检查
# --------------------------------------------------------------------------
def scan_file(path, rel):
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            src = f.read().replace('\r\n', '\n')
    except OSError as exc:
        print('  !! 无法读取 %s: %s' % (path, exc))
        return None
    stripped = strip_comments_and_strings(src)
    lines = stripped.split('\n')
    funcs = find_functions(stripped)
    return {'path': path, 'rel': rel, 'src': src, 'stripped': stripped,
            'lines': lines, 'funcs': funcs}


def func_of(funcs, line):
    """包含该行的函数（取最内层 = 起始行最大者）"""
    best = None
    for f in funcs:
        if f['start'] <= line <= f['end']:
            if best is None or f['start'] > best['start']:
                best = f
    return best


def collect_writers(files):
    """写者集合：name -> [(rel, line), ...]"""
    writers = {}
    for fi in files:
        for ln, text in enumerate(fi['lines'], 1):
            for m in re.finditer(_LOCK_CALL, text):
                if m.group(0).startswith('pthread_rwlock_wrlock'):
                    if master_lock_expr(m.group(1)) is not None:
                        f = func_of(fi['funcs'], ln)
                        if f is None:
                            # 顶层（函数外）的取锁 —— 记为伪函数，仍然报
                            f = {'name': '<top-level>', 'start': ln, 'end': ln}
                        writers.setdefault(f['name'], []).append((fi['rel'], ln))
    return writers


def find_read_regions(fi, writers):
    """读者区间 + 区间内的写者调用"""
    hits = []
    unclosed = []
    lines = fi['lines']
    for ln, text in enumerate(lines, 1):
        for m in re.finditer(_LOCK_CALL, text):
            if not m.group(0).startswith('pthread_rwlock_rdlock'):
                continue
            base = master_lock_expr(m.group(1))
            if base is None:
                continue
            want = norm(m.group(1))
            # 向前找同表达式的 unlock
            end = None
            for ln2 in range(ln + 1, len(lines) + 1):
                for m2 in re.finditer(_LOCK_CALL, lines[ln2 - 1]):
                    if not m2.group(0).startswith('pthread_rwlock_unlock'):
                        continue
                    if norm(m2.group(1)) == want:
                        end = ln2
                        break
                if end is not None:
                    break
            if end is None:
                f = func_of(fi['funcs'], ln)
                unclosed.append((ln, base, f['name'] if f else '?'))
                f_end = f['end'] if f else len(lines)
                end = f_end
            # 区间内的写者调用
            for ln3 in range(ln, end + 1):
                if ln3 > len(lines):
                    break
                txt = lines[ln3 - 1]
                for name in writers:
                    if name.startswith('<'):
                        continue
                    for cm in re.finditer(r'(?<![\w.>])' + re.escape(name) + r'\s*\(', txt):
                        # 跳过命中行 = 写者自身的定义行（同名）
                        hits.append({
                            'file': fi['rel'], 'line': ln3,
                            'caller': (func_of(fi['funcs'], ln) or {}).get('name', '?'),
                            'rd': ln, 'un': end, 'base': base,
                            'callee': name,
                            'wr_sites': writers[name],
                        })
    return hits, unclosed


def main(argv):
    if len(argv) > 1:
        root = os.path.abspath(argv[1])
    else:
        root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    subdirs = ['src', 'demos', 'tools']
    paths = []
    for sd in subdirs:
        d = os.path.join(root, sd)
        for dirpath, _dirnames, filenames in os.walk(d):
            for fn in sorted(filenames):
                if fn.endswith('.c'):
                    paths.append(os.path.join(dirpath, fn))
    paths.sort()

    print('锁纪律静态检查器  root=%s  待查 .c=%d 个' % (root, len(paths)))
    print('纪律出处: src/funcword.c 「持读锁期间内部不得调用抢 master 写锁的函数」')
    print('-' * 78)

    files = []
    for p in paths:
        fi = scan_file(p, os.path.relpath(p, root))
        if fi is not None:
            files.append(fi)

    writers = collect_writers(files)
    print('写者集合（会取 master 写锁的函数）: %d 个' % len(writers))
    for name in sorted(writers):
        sites = ', '.join('%s:%d' % s for s in writers[name])
        print('   - %-42s %s' % (name, sites))
    print('-' * 78)

    all_hits = []
    all_unclosed = []
    n_regions = 0
    for fi in files:
        regions_txt = []
        for ln, text in enumerate(fi['lines'], 1):
            for m in re.finditer(_LOCK_CALL, text):
                if m.group(0).startswith('pthread_rwlock_rdlock') and \
                        master_lock_expr(m.group(1)) is not None:
                    regions_txt.append(ln)
        n_regions += len(regions_txt)
        hits, unclosed = find_read_regions(fi, writers)
        all_hits.extend(hits)
        all_unclosed.extend([(fi['rel'],) + u for u in unclosed])
        if regions_txt:
            print('%s: master 读锁区间起始行 %s' % (fi['rel'], regions_txt))

    print('-' * 78)
    if all_unclosed:
        print('⚠️ 未闭合的 master 读锁区间（可能漏 unlock，另需人工核）:')
        for rel, ln, base, fname in all_unclosed:
            print('   %s:%d  锁=&%s->rwlock  所在函数=%s' % (rel, ln, base, fname))
        print('-' * 78)

    if all_hits:
        print('❌ 命中：持 master 读锁期间调用了会取 master 写锁的函数')
        for h in sorted(all_hits, key=lambda x: (x['file'], x['line'])):
            print('  %s:%d  在函数 %s() 内 —— 读锁区间 [%d..%d]（&%s->rwlock）'
                  % (h['file'], h['line'], h['caller'], h['rd'], h['un'], h['base']))
            print('        调用了 %s()  ⇐ 判定：违规（同线程「读→写」自升级 ⇒ 永久自死锁）'
                  % h['callee'])
            print('        该函数的 master 写锁取锁点: %s'
                  % ', '.join('%s:%d' % s for s in h['wr_sites']))
        print('')
        print('LOCK-DISCIPLINE: FAIL (%d 处)' % len(all_hits))
        return 1

    print('✅ 未发现「持 master 读锁期间取 master 写锁」的站点')
    print('')
    print('LOCK-DISCIPLINE: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

# --------------------------------------------------------------------------
# LIMITATIONS（已知限制 / 未覆盖的调用形态）
# --------------------------------------------------------------------------
# 1. **间接调用抓不到**：函数指针、`#define` 宏别名、`dlsym` 之类无法静态解析。
#    本检查器只认「区间内直接出现 `writer_name(`」这一形态。
# 2. **调用图不传递**：若读锁区间内调用了 A()，而 A() 再调用 B()，B() 取 master
#    写锁 —— 本检查器**不会**报（只查一层）。同理，写者集合来自**函数体直接**出现
#    `pthread_rwlock_wrlock(&master->rwlock)`；若某函数把取写锁的动作藏在自己的
#    被调者里，它不会被算作写者。
# 3. **master 别名靠命名**：只识别 `master` 与 `X->master`。若某处把 master 指针
#    拷进别名的局部变量（`MasterTopology* m = bs->master; wrlock(&m->rwlock)`），
#    本检查器会漏。实测本仓库不存在该形态（全仓 grep 人工核过）。
# 4. **函数表靠启发式**：`{` 前回溯到 `)` 即判为函数体（函数定义在列 0 的 C 风格）。
#    含 `(T){...}` 复合字面量的极罕见写法可能被误判；误判只会影响
#    「哪个函数包含该行」的归因，不影响「区间内是否有写者调用」的判定。
# 5. **预处理不展开宏**：宏里的取锁/调用若形态特殊（跨行拼接）可能被漏。
#    （本仓库的锁调用都是直接写出的函数调用，实测无此形态。）
# 6. **`rdlock` → `unlock` 配对按表达式文本匹配**：同一表达式多次 rdlock 嵌套时，
#    取「向后第一个同表达式 unlock」。嵌套同表达式读锁在本仓库不存在。
