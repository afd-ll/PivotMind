#!/usr/bin/env python3
"""
PivotMind 爬虫：爬取纯中文/纯英文语料保存到文件
mode=zh: 纯中文
mode=en: 纯英文
"""
import sys, os, json, time, random, re, urllib.request, urllib.error, html
from datetime import datetime

UA = "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36"
DATA_DIR = os.path.expanduser("~/pivotmind/cron/data")
os.makedirs(DATA_DIR, exist_ok=True)

def log(msg):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)

def fetch(url, timeout=10):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        resp = urllib.request.urlopen(req, timeout=timeout)
        return resp.read().decode("utf-8", errors="ignore")
    except Exception as e:
        return None

# ══════════════════════════════════════════
# 中文源
# ══════════════════════════════════════════

def grab_baidu_baike(count=15):
    texts = []
    for _ in range(count * 2):
        raw = fetch("https://baike.baidu.com/api/random?format=json", timeout=8)
        if raw:
            try:
                d = json.loads(raw)
                t = d.get("title", "")
                if t:
                    texts.append(f"{t}是{d.get('abstract', '一个条目')}。")
            except: pass
        time.sleep(0.5)
        if len(texts) >= count:
            break
    log(f"  百度百科: {len(texts)} 条")
    return texts[:count]

def grab_zhihu_hot(count=20):
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
        except Exception as e:
            log(f"  知乎解析失败: {e}")
    log(f"  知乎热榜: {len(texts)} 条")
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
        except: pass
    log(f"  新浪新闻: {len(texts)} 条")
    return texts[:count * 2]

def grab_zh_wiki(count=15):
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
    log(f"  中文维基: {len(texts)} 条")
    return texts[:count]

# ══════════════════════════════════════════
# 英文源
# ══════════════════════════════════════════

def grab_gutenberg(count=3):
    texts = []
    books = [
        ("https://www.gutenberg.org/files/1342/1342-0.txt", "Pride and Prejudice"),
        ("https://www.gutenberg.org/files/11/11-0.txt", "Alice in Wonderland"),
        ("https://www.gutenberg.org/files/84/84-0.txt", "Frankenstein"),
        ("https://www.gutenberg.org/files/1661/1661-0.txt", "Sherlock Holmes"),
        ("https://www.gutenberg.org/files/2701/2701-0.txt", "Moby Dick"),
        ("https://www.gutenberg.org/files/98/98-0.txt", "A Tale of Two Cities"),
        ("https://www.gutenberg.org/files/74/74-0.txt", "Tom Sawyer"),
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
        chunk_lines = [l.strip() for l in lines[start:min(start+300, end)] if l.strip() and len(l.strip()) > 15]
        texts.extend(chunk_lines)
        log(f"  {name}: {len(chunk_lines)} 行")
        time.sleep(1)
    return texts

def grab_devto(count=20):
    texts = []
    raw = fetch("https://dev.to/api/articles?per_page=30", timeout=10)
    if raw:
        try:
            arts = json.loads(raw)
            for a in arts:
                title = a.get("title", "")
                desc = a.get("description", "")
                if title:
                    texts.append(f"{title}. {desc}" if desc else title)
        except: pass
    log(f"  Dev.to: {len(texts)} 条")
    return texts[:count]

def grab_github_trending(count=15):
    texts = []
    raw = fetch("https://github.com/trending?since=weekly", timeout=10)
    if raw:
        repos = re.findall(r'href="/([^"]+)"[^>]*>([^<]+)', raw)
        descs = re.findall(r'<p class="col-9[^"]*"[^>]*>([^<]+)', raw)
        for r in repos[:count]:
            texts.append(f"Repository: {r[0]}. {r[1].strip()}")
        for d in descs[:count]:
            t = html.unescape(d.strip())
            if t:
                texts.append(f"Description: {t}")
    log(f"  GitHub Trending: {len(texts)} 条")
    return texts

# ══════════════════════════════════════════
# 主逻辑
# ══════════════════════════════════════════

def save(texts, mode):
    """保存到文件"""
    date = datetime.now().strftime("%Y%m%d")
    path = os.path.join(DATA_DIR, f"{mode}_{date}.jsonl")
    with open(path, "w", encoding="utf-8") as f:
        for t in texts:
            f.write(json.dumps({"text": t}, ensure_ascii=False) + "\n")
    log(f"已保存 {len(texts)} 条到 {path}")
    return path

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "zh"
    texts = []

    if mode == "zh":
        log("=== 中文爬虫 开始 ===")
        texts.extend(grab_baidu_baike(15))
        texts.extend(grab_zhihu_hot(20))
        texts.extend(grab_sina_news(30))
        texts.extend(grab_zh_wiki(15))

        # 过滤：保证纯中文
        clean = []
        seen = set()
        for t in texts:
            t = t.strip()
            if not t or len(t) < 8:
                continue
            zh = sum(1 for c in t if '\u4e00' <= c <= '\u9fff')
            if zh < len(t) * 0.4:
                continue
            en = sum(1 for c in t if c.isascii() and c.isalpha())
            if en > len(t) * 0.5:
                continue
            if t not in seen:
                seen.add(t)
                clean.append(t)

    elif mode == "en":
        log("=== 英文爬虫 开始 ===")
        texts.extend(grab_gutenberg(3))
        texts.extend(grab_devto(20))
        texts.extend(grab_github_trending(15))

        # 过滤：保证纯英文
        clean = []
        seen = set()
        for t in texts:
            t = t.strip()
            if not t or len(t) < 10:
                continue
            en = sum(1 for c in t if c.isascii() and c.isalpha())
            if en < len(t) * 0.6:
                continue
            zh = sum(1 for c in t if '\u4e00' <= c <= '\u9fff')
            if zh > len(t) * 0.2:
                continue
            if t not in seen:
                seen.add(t)
                clean.append(t)
    else:
        print(f"用法: {sys.argv[0]} zh|en")
        sys.exit(1)

    log(f"原始 {len(texts)} 条 → 过滤后 {len(clean)} 条")
    if clean:
        save(clean, mode)
    log("=== 完成 ===")
