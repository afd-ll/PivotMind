#!/usr/bin/env python3
"""
PivotMind 爬虫+喂料脚本
mode=zh: 爬纯中文 → 喂 gateway
mode=en: 爬纯英文 → 喂 gateway
"""
import sys, os, json, time, random, re, urllib.request, urllib.error, html
from datetime import datetime

GW_URL = "http://localhost:19531/learn"
CHUNK_SIZE = 1800
RATE_LIMIT = 4
ROUNDS = 10
UA = "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36"

def log(msg):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)

def fetch(url, timeout=10, retries=2):
    for i in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            resp = urllib.request.urlopen(req, timeout=timeout)
            return resp.read().decode("utf-8", errors="ignore")
        except Exception as e:
            if i < retries - 1:
                time.sleep(2)
            else:
                return None

# ══════════════════════════════════════════
# 中文源
# ══════════════════════════════════════════

def grab_baidu_baike(count=20):
    texts = []
    for _ in range(count):
        raw = fetch("https://baike.baidu.com/api/random?format=json", timeout=8)
        if raw:
            try:
                d = json.loads(raw)
                t = d.get("title", "")
                a = d.get("abstract", "")
                if t and a:
                    texts.append(f"{t}是{a}")
            except: pass
        time.sleep(0.3)
    return texts

def grab_zhihu(count=30):
    texts = []
    raw = fetch("https://www.zhihu.com/api/v3/feed/topstory/hot-lists/total?limit=50", timeout=15)
    if raw:
        try:
            d = json.loads(raw)
            for item in d.get("data", []):
                t = item.get("target", {})
                title = t.get("title", "")
                excerpt = t.get("excerpt", "")
                if title:
                    texts.append(f"{title}。{excerpt}" if excerpt else title)
        except: pass
    return texts[:count]

def grab_sina_news(count=30):
    texts = []
    raw = fetch("https://feed.mix.sina.com.cn/api/roll/get?pageid=153&lid=2509&num=50", timeout=10)
    if raw:
        try:
            d = json.loads(raw)
            for item in d.get("result", {}).get("data", []):
                t = item.get("title", "")
                i = item.get("intro", "")
                if t:
                    texts.append(t)
                if i and len(i) > 10:
                    texts.append(i)
        except:
            pass
    return texts[:count * 2]

def grab_zh_wiki(count=10):
    texts = []
    raw = fetch(
        "https://zh.wikipedia.org/w/api.php?action=query&generator=random&grnnamespace=0&prop=extracts&exintro=1&explaintext=1&format=json&grnlimit=20",
        timeout=15
    )
    if raw:
        try:
            d = json.loads(raw)
            for pid, page in d.get("query", {}).get("pages", {}).items():
                t = page.get("extract", "").strip()
                if t and len(t) > 30:
                    texts.append(t)
        except: pass
    return texts[:count]

def crawl_zh():
    texts = []
    log("爬中文: 百度百科...")
    texts.extend(grab_baidu_baike(30))
    log(f"        百度百科 → {len(texts)} 条")
    log("爬中文: 知乎热榜...")
    texts.extend(grab_zhihu(30))
    log(f"        知乎 → {len(texts)} 条")
    log("爬中文: 新浪新闻...")
    texts.extend(grab_sina_news(30))
    log(f"        新浪 → {len(texts)} 条")
    log("爬中文: 维基百科...")
    texts.extend(grab_zh_wiki(10))
    log(f"        维基 → {len(texts)} 条")

    # 去重+过滤
    seen = set()
    clean = []
    for t in texts:
        t = t.strip()
        if not t or len(t) < 10:
            continue
        zh_chars = sum(1 for c in t if '\u4e00' <= c <= '\u9fff')
        if zh_chars < len(t) * 0.4:
            continue
        # 过滤纯英文行
        en_chars = sum(1 for c in t if c.isascii() and c.isalpha())
        if en_chars > len(t) * 0.6:
            continue
        if t not in seen:
            seen.add(t)
            clean.append(t)

    log(f"中文: 去重过滤后 {len(clean)} 条")
    return clean

# ══════════════════════════════════════════
# 英文源
# ══════════════════════════════════════════

def grab_gutenberg(count=3):
    """取 Gutenberg 公版书片段"""
    texts = []
    books = [
        ("https://www.gutenberg.org/files/1342/1342-0.txt", "Pride and Prejudice"),
        ("https://www.gutenberg.org/files/11/11-0.txt", "Alice in Wonderland"),
        ("https://www.gutenberg.org/files/84/84-0.txt", "Frankenstein"),
        ("https://www.gutenberg.org/files/1661/1661-0.txt", "Sherlock Holmes"),
        ("https://www.gutenberg.org/files/2701/2701-0.txt", "Moby Dick"),
        ("https://www.gutenberg.org/files/98/98-0.txt", "A Tale of Two Cities"),
        ("https://www.gutenberg.org/files/74/74-0.txt", "The Adventures of Tom Sawyer"),
        ("https://www.gutenberg.org/cache/epub/45/pg45.txt", "Anne of Green Gables"),
    ]
    for url, name in books[:count]:
        raw = fetch(url, timeout=20)
        if not raw:
            continue
        lines = raw.split('\n')
        start, end = 0, len(lines)
        for i, l in enumerate(lines):
            if "*** START OF" in l or "***START OF" in l:
                start = i + 1
            if "*** END OF" in l or "***END OF" in l:
                end = i
                break
        # 取正文前 200 行
        for i in range(start, min(start + 300, end)):
            l = lines[i].strip()
            if l and len(l) > 15:
                texts.append(l)
        log(f"        {name}: {min(300, end-start)} 行")
        time.sleep(1)
    return texts

def grab_devto(count=15):
    texts = []
    raw = fetch("https://dev.to/api/articles?per_page=30", timeout=10)
    if raw:
        try:
            articles = json.loads(raw)
            for a in articles:
                title = a.get("title", "")
                desc = a.get("description", "")
                tags = a.get("tag_list", [])
                if title:
                    texts.append(f"{title}. {desc}" if desc else title)
        except: pass
    return texts[:count]

def grab_github_trending(count=20):
    texts = []
    raw = fetch("https://github.com/trending?since=weekly", timeout=10)
    if raw:
        repos = re.findall(r'<h2[^>]*>.*?href="/([^"]+)"[^>]*>([^<]+)<', raw)
        descs = re.findall(r'<p class="col-9[^"]*"[^>]*>([^<]+)', raw)
        for r in repos[:count]:
            texts.append(f"{r[0]}: {r[1].strip()}")
        for d in descs[:count]:
            t = html.unescape(d.strip())
            if t:
                texts.append(t)
    return texts

def grab_en_wiki(count=10):
    texts = []
    # 尝试用可访问的维基镜像
    mirrors = [
        "https://en.wikipedia.org/w/api.php?action=query&generator=random&grnnamespace=0&prop=extracts&exintro=1&explaintext=1&format=json&grnlimit=20",
    ]
    for url in mirrors:
        raw = fetch(url, timeout=15)
        if raw:
            try:
                d = json.loads(raw)
                for pid, page in d.get("query", {}).get("pages", {}).items():
                    t = page.get("extract", "").strip()
                    if t and len(t) > 30:
                        texts.append(t)
            except: pass
            break
    return texts[:count]

def crawl_en():
    texts = []
    log("爬英文: Project Gutenberg...")
    texts.extend(grab_gutenberg(3))
    log(f"        Gutenberg → {len(texts)} 条")
    log("爬英文: Dev.to...")
    texts.extend(grab_devto(15))
    log(f"        Dev.to → {len(texts)} 条")
    log("爬英文: GitHub Trending...")
    texts.extend(grab_github_trending(10))
    log(f"        GitHub → {len(texts)} 条")
    log("爬英文: Wikipedia...")
    texts.extend(grab_en_wiki(10))
    log(f"        Wikipedia → {len(texts)} 条")

    # 去重+过滤
    seen = set()
    clean = []
    for t in texts:
        t = t.strip()
        if not t or len(t) < 10:
            continue
        en_chars = sum(1 for c in t if c.isascii() and c.isalpha())
        if en_chars < len(t) * 0.6:
            continue
        zh_chars = sum(1 for c in t if '\u4e00' <= c <= '\u9fff')
        if zh_chars > len(t) * 0.3:
            continue
        if t not in seen:
            seen.add(t)
            clean.append(t)

    log(f"英文: 去重过滤后 {len(clean)} 条")
    return clean

# ══════════════════════════════════════════
# 喂料
# ══════════════════════════════════════════

def feed(texts, rounds=ROUNDS):
    if not texts:
        log("没有语料，跳过喂料")
        return 0

    chunks = []
    for t in texts:
        while len(t) > CHUNK_SIZE:
            chunks.append(t[:CHUNK_SIZE])
            t = t[CHUNK_SIZE:]
        if t:
            chunks.append(t)

    total_chunks = len(chunks)
    total_nodes = 0

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

    log(f"喂料完成: 共 {rounds} 轮, 新增 {total_nodes} 节点")
    return total_nodes

# ══════════════════════════════════════════
# 入口
# ══════════════════════════════════════════

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "zh"
    if mode == "zh":
        log("=== 中文爬虫+喂料 开始 ===")
        feed(crawl_zh())
    elif mode == "en":
        log("=== 英文爬虫+喂料 开始 ===")
        feed(crawl_en())
    else:
        print(f"用法: {sys.argv[0]} zh|en")
        sys.exit(1)
    log("=== 完成 ===")
