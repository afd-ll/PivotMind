#!/usr/bin/env python3
"""
PivotMind 喂料：从文件读取语料，喂给 gateway，跑 N 轮
mode=zh: 喂中文语料文件
mode=en: 喂英文语料文件
"""
import sys, os, json, time, random, urllib.request, urllib.error
from datetime import datetime

GW_URL = "http://localhost:19531/learn"
CHUNK_SIZE = 1800
RATE_LIMIT = 4  # gateway 限流 5/s，保守 4/s
ROUNDS = 10
DATA_DIR = os.path.expanduser("~/pivotmind/cron/data")

def log(msg):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)

def find_latest(mode):
    """找最新语料文件"""
    candidates = []
    for f in os.listdir(DATA_DIR):
        if f.startswith(f"{mode}_") and f.endswith(".jsonl"):
            path = os.path.join(DATA_DIR, f)
            candidates.append((os.path.getmtime(path), path))
    if not candidates:
        return None
    candidates.sort(reverse=True)
    return candidates[0][1]

def load_texts(path):
    """读取语料文件"""
    texts = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    d = json.loads(line)
                    t = d.get("text", "")
                    if t:
                        texts.append(t)
                except:
                    pass
    return texts

def ensure_gateway():
    """检测 gateway 是否在运行，没有则启动"""
    import subprocess
    try:
        r = urllib.request.urlopen("http://localhost:19531/status", timeout=3)
        return True
    except:
        log("Gateway 未运行，正在启动...")
        subprocess.Popen(
            ["./build/bin/pivotmind_gateway", "19531", "."],
            cwd=os.path.expanduser("~/pivotmind"),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        time.sleep(15)
        return False

def feed(texts, rounds=ROUNDS):
    if not texts:
        log("没有语料，跳过")
        return 0

    # 确保 gateway 在跑
    ensure_gateway()

    # 切 chunk
    chunks = []
    for t in texts:
        while len(t) > CHUNK_SIZE:
            chunks.append(t[:CHUNK_SIZE])
            t = t[CHUNK_SIZE:]
        if t:
            chunks.append(t)

    total_chunks = len(chunks)
    total_nodes = 0
    log(f"共 {total_chunks} 个 chunk，跑 {rounds} 轮")

    for r in range(1, rounds + 1):
        random.shuffle(chunks)
        sent = 0
        round_nodes = 0

        for chunk in chunks:
            payload = json.dumps({"msg": chunk}).encode()
            req = urllib.request.Request(
                GW_URL, data=payload,
                headers={"Content-Type": "application/json"},
                method="POST"
            )
            try:
                resp = urllib.request.urlopen(req, timeout=5)
                result = json.loads(resp.read().decode())
                round_nodes += result.get("added", 0)
                sent += 1
            except urllib.error.HTTPError as e:
                if e.code == 429:
                    time.sleep(0.5)
                continue
            except:
                continue
            time.sleep(1.0 / RATE_LIMIT)

        total_nodes += round_nodes
        log(f"第 {r}/{rounds} 轮: 喂 {sent}/{total_chunks} 条, 新增 {round_nodes} 节点")
        time.sleep(3)

    log(f"喂料完成: {rounds} 轮, 新增 {total_nodes} 节点")
    return total_nodes

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "zh"

    path = find_latest(mode)
    if not path:
        log(f"没有找到 {mode} 语料文件")
        sys.exit(0)

    log(f"读取: {path}")
    texts = load_texts(path)
    log(f"加载 {len(texts)} 条语料")
    feed(texts)
    log("=== 完成 ===")
