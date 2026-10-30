#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""玄枢 A/B 测量台 —— 每次跑都用同一份 state 副本、独立进程、独立端口。

为什么需要它（2026-10-30 定）：
  生产 gateway 每轮对话都在学习（首见词建节点 + 加边），所以**同一个输入在不同时刻
  输出必然不同**。过去几轮我拿"生产上跑几条"当 A/B，判据本身不可信（同一个配置两次
  结果都能不一样）。本工具有两个硬约束：
    ① 每轮从同一份 base 快照复制 data 目录 ⇒ 起点逐字节相同
    ② 每轮起独立进程 + 独立端口 ⇒ 不碰 8080 上的生产实例

用法：
  python3 tools/ab_harness.py --tag baseline --runs 2
  python3 tools/ab_harness.py --tag floor012 --runs 2 --env PIVOTMIND_POOL_FLOOR=0.12
  python3 tools/ab_harness.py --make-base          # 用当前生产数据重建 base 快照

输出：逐题列出各轮回复，末行给出「跨轮逐字一致 N/M」与「唯一输出数」。
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
import urllib.request

HOME = "/home/ubuntu/PivotMind"
BIN = os.path.join(HOME, "build/bin/pivotmind_gateway")
ABROOT = "/home/ubuntu/pm_ab"
BASE = os.path.join(ABROOT, "base")

DEFAULT_QS = [
    "我想吃苹果",
    "我喜欢音乐",
    "学习编程",
    "我最近很累",
    "盐是什么味道",
    "今天天气怎么样",
]


def log(msg):
    print(msg, flush=True)


def make_base():
    """把生产 data 目录复制成 base 快照。

    🔴🔴 必须先停服（2026-10-30 踩到的）：`pivotmind_state.dat` 只在 gateway
    shutdown 时才落盘，运行中磁盘上那个文件还是**上一次停服时**的旧状态。
    所以不停服就采，采到的是旧快照 —— 线上跑一整天对话后，base 与线上已经
    分叉（实测：测量台 `学习编程 → 学习编程`，线上 `→ 学习编程的学习编程古文`），
    A/B 结论没法外推。

    代价：停服 + 起服约 35 秒（起服要加载 30MB state）。A/B 本来就是离线分析，
    这个代价可以接受；不接受就只能在「拿旧快照」和「拿真状态」之间二选一。
    """
    src = os.path.join(HOME, "data")
    svc = "pivotmind-gateway"
    was_active = subprocess.call(["systemctl", "is-active", "--quiet", svc],
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL) == 0
    if was_active:
        log("[base] 线上在跑 ⇒ 先停服让 state 落盘（否则采到的是旧状态）")
        subprocess.call(["systemctl", "stop", svc])
        time.sleep(3)
    try:
        if os.path.exists(BASE):
            shutil.rmtree(BASE)
        os.makedirs(ABROOT, exist_ok=True)
        shutil.copytree(src, BASE)
    finally:
        if was_active:
            log("[base] 起服（约 28s 才 ready，但 A/B 用独立实例，不等它）")
            subprocess.call(["systemctl", "start", svc])
    n = 0
    for name in os.listdir(BASE):
        p = os.path.join(BASE, name)
        if os.path.isfile(p):
            n += 1
    log("[base] 快照完成 %s（%d 个文件, %.0f MB）" %
        (BASE, n, sum(os.path.getsize(os.path.join(BASE, f))
                      for f in os.listdir(BASE)
                      if os.path.isfile(os.path.join(BASE, f))) / 1048576.0))
    st = os.path.join(BASE, "pivotmind_state.dat")
    if os.path.exists(st):
        log("[base] state %.0f MB · mtime %s" %
            (os.path.getsize(st) / 1048576.0,
             time.strftime("%H:%M:%S", time.localtime(os.path.getmtime(st)))))


def token_of(d):
    with open(os.path.join(d, "gw_token"), encoding="utf-8") as f:
        return f.read().strip()


def status(port, tok, timeout=10):
    req = urllib.request.Request("http://127.0.0.1:%d/status" % port,
                                 headers={"X-Pivot-Token": tok})
    return json.loads(urllib.request.urlopen(req, timeout=timeout).read().decode())


def chat(port, tok, q, timeout=120):
    body = json.dumps({"msg": q}, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request("http://127.0.0.1:%d/chat" % port, data=body,
                                 method="POST",
                                 headers={"Content-Type": "application/json",
                                          "X-Pivot-Token": tok})
    raw = urllib.request.urlopen(req, timeout=timeout).read().decode()
    try:
        j = json.loads(raw)
        # /chat 回的是 {"reply": "...", "nodes": N, "dialogs": M}（不是 response）
        return j.get("reply") or j.get("response") or ""
    except Exception:
        return "<非 JSON: %s>" % raw[:80]


def run_once(tag, idx, port, envs, qs):
    rundir = os.path.join(ABROOT, tag, "r%d" % idx)
    if os.path.exists(rundir):
        shutil.rmtree(rundir)
    os.makedirs(os.path.dirname(rundir), exist_ok=True)
    os.makedirs(rundir, exist_ok=True)
    # 路径 SSOT：pm_file() 一律解析到 <PIVOTMIND_HOME>/data/xxx
    # ⇒ 快照必须落到 <home>/data，不是 <home> 根。放错位置会 fail-loud「静默从空脑开始」。
    shutil.copytree(BASE, os.path.join(rundir, "data"))

    logfile = os.path.join(rundir, "gw.log")
    env = dict(os.environ)
    env["PIVOTMIND_HOME"] = rundir
    env["PIVOTMIND_LOG_FILE"] = logfile
    for k, v in envs:
        env[k] = v

    lf = open(logfile, "wb")
    proc = subprocess.Popen([BIN, str(port), rundir], cwd=HOME, env=env,
                            stdout=lf, stderr=lf, preexec_fn=os.setsid)
    tok = token_of(os.path.join(rundir, "data"))
    out = {"tag": tag, "run": idx, "port": port, "answers": {},
           "nodes_before": None, "nodes_after": None, "ok": False}
    try:
        st = None
        deadline = time.time() + 180
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            try:
                st = status(port, tok)
                if st.get("status") == "running":
                    break
            except Exception:
                pass
            time.sleep(2)
        if not st or st.get("status") != "running":
            tail = ""
            if os.path.exists(logfile):
                with open(logfile, "rb") as f:
                    tail = f.read()[-600:].decode("utf-8", "replace")
            log("  !! r%d 起不来（端口 %d）\n%s" % (idx, port, tail))
            return out

        out["nodes_before"] = st.get("total_nodes")
        log("  r%d 起来: nodes=%s" % (idx, st.get("total_nodes")))
        for q in qs:
            a = chat(port, tok, q)
            out["answers"][q] = a
            log("    %-14s -> %s" % (q, a[:46]))
        try:
            out["nodes_after"] = status(port, tok).get("total_nodes")
        except Exception:
            pass
        out["ok"] = True
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            for _ in range(40):
                if proc.poll() is not None:
                    break
                time.sleep(0.5)
            if proc.poll() is None:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            pass
        lf.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="ab")
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--questions", default="")
    ap.add_argument("--make-base", action="store_true")
    args = ap.parse_args()

    if args.make_base:
        make_base()
        return 0
    if not os.path.isdir(BASE):
        log("!! 没有 base 快照，先跑 --make-base")
        return 2

    # base 陈旧告警：快照越旧，A/B 结论越不能外推到线上（2026-10-30 教训）
    st_path = os.path.join(BASE, "pivotmind_state.dat")
    if os.path.exists(st_path):
        age_h = (time.time() - os.path.getmtime(st_path)) / 3600.0
        log("[base] 快照 age=%.1f 小时 · %.0f MB" %
            (age_h, os.path.getsize(st_path) / 1048576.0))
        if age_h > 2.0:
            log("[base] ⚠️ 快照超过 2 小时，线上 state 可能已漂移 ⇒ "
                "结论只代表「该快照下」的对比，别直接外推到线上")

    qs = ([x for x in args.questions.split(",") if x] or DEFAULT_QS)
    envs = []
    for e in args.env:
        if "=" not in e:
            log("!! --env 要 K=V 形式: %r" % e)
            return 2
        k, v = e.split("=", 1)
        envs.append((k, v))

    log("=== tag=%s runs=%d port=%d env=%s ===" % (args.tag, args.runs, args.port, envs or "无"))
    results = []
    for i in range(1, args.runs + 1):
        r = run_once(args.tag, i, args.port, envs, qs)
        results.append(r)

    ok = [r for r in results if r["ok"]]
    if not ok:
        log("!! 没有一轮成功")
        return 3

    log("\n=== 汇总 ===")
    same = 0
    uniq_all = set()
    for q in qs:
        vals = [r["answers"].get(q, "<缺失>") for r in ok]
        uniq = len(set(vals))
        uniq_all.update(vals)
        if uniq == 1:
            same += 1
        log("  %-14s 唯一=%d  %s" % (q, uniq, " | ".join(v[:28] for v in vals)))
    log("  跨轮逐字一致: %d/%d · 全局唯一输出 %d 条" % (same, len(qs), len(uniq_all)))
    for r in ok:
        log("  r%d nodes %s -> %s" % (r["run"], r["nodes_before"], r["nodes_after"]))

    outp = os.path.join(ABROOT, "%s-result.json" % args.tag)
    with open(outp, "w", encoding="utf-8") as f:
        json.dump(results, f, ensure_ascii=False, indent=1)
    log("  结果落盘: %s" % outp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
