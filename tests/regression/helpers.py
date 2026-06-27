"""helpers.py — PivotMind regression test utilities."""

import subprocess
import json
import time
import sys
import os
import signal

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GATEWAY_BIN = os.path.join(PROJECT_ROOT, "build", "bin", "pivotmind_gateway")

def find_free_port():
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_gateway(binary=None, port=None, timeout=30):
    """Start gateway in background. Returns (process, port)."""
    if binary is None:
        binary = GATEWAY_BIN
    if port is None:
        port = find_free_port()

    if not os.path.exists(binary):
        raise FileNotFoundError(f"Gateway binary not found: {binary}")

    proc = subprocess.Popen(
        [binary, str(port)],
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            resp = _get(f"http://127.0.0.1:{port}/health", timeout=2)
            if resp is not None:
                return proc, port
        except Exception:
            pass
        if proc.poll() is not None:
            stderr = proc.stderr.read().decode(errors="replace")
            raise RuntimeError(f"Gateway exited with code {proc.returncode}: {stderr[-500:]}")
        time.sleep(0.5)

    proc.terminate()
    proc.wait(timeout=5)
    raise TimeoutError("Gateway did not start within timeout")


def stop_gateway(proc):
    """Gracefully stop gateway."""
    if proc is None:
        return
    try:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=10)
    except Exception:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


def _get(url, timeout=5):
    """HTTP GET with urllib, returns parsed JSON or raises."""
    from urllib.request import urlopen, Request
    req = Request(url, headers={"User-Agent": "pivotmind-regression-test"})
    with urlopen(req, timeout=timeout) as resp:
        if resp.status != 200:
            return None
        data = resp.read().decode("utf-8")
        try:
            return json.loads(data)
        except json.JSONDecodeError:
            return data


def _post(url, body, timeout=10):
    """HTTP POST with urllib, returns parsed JSON or raises."""
    from urllib.request import urlopen, Request
    from urllib.error import HTTPError
    data = json.dumps(body).encode("utf-8")
    req = Request(url, data=data, headers={
        "Content-Type": "application/json",
        "User-Agent": "pivotmind-regression-test",
    })
    try:
        with urlopen(req, timeout=timeout) as resp:
            if resp.status != 200:
                return None
            resp_data = resp.read().decode("utf-8")
            try:
                return json.loads(resp_data)
            except json.JSONDecodeError:
                return resp_data
    except HTTPError as e:
        if e.code == 400:
            return None  # bad request, not a crash
        raise


def health(port):
    return _get(f"http://127.0.0.1:{port}/health")


def status(port):
    return _get(f"http://127.0.0.1:{port}/status")


def chat(port, query):
    """POST /chat and return reply string. API expects {"msg": query}."""
    result = _post(f"http://127.0.0.1:{port}/chat", {"msg": query})
    if isinstance(result, dict):
        return result.get("reply", result.get("response", str(result)))
    return str(result) if result else ""


# English function words (subset, most common offenders)
FW_EN = {
    "the", "be", "is", "am", "are", "was", "were", "been", "being",
    "have", "has", "had", "having", "do", "does", "did",
    "will", "would", "shall", "should", "may", "might", "must", "can", "could",
    "of", "in", "to", "for", "with", "on", "at", "from", "by", "about",
    "up", "down", "out", "off", "over", "under",
    "not", "no", "nor", "only", "so", "than", "too", "very",
    "that", "this", "what", "which", "who",
    "it", "they", "them", "he", "she", "we", "you",
    "his", "her", "its", "their", "our", "my", "your",
    "and", "but", "or", "if", "while", "because", "as",
}

# Chinese function words (subset, UTF-8 encoded)
FW_ZH = {
    "的", "了", "在", "是", "我", "你", "他", "她", "它", "们",
    "不", "这", "那", "也", "就", "都", "还", "把", "被",
    "和", "与", "或", "对", "从", "到", "向", "给", "让",
    "要", "能", "会", "可", "以", "很", "最", "更",
    "一", "个", "些", "里", "中", "上", "下",
    "很", "得", "地", "着", "过", "吗", "呢", "吧",
}


def count_fw(text):
    """Count function words in text. Returns (en_count, zh_count)."""
    words = text.lower().split()
    en = sum(1 for w in words if w in FW_EN)
    zh = sum(1 for c in text if c in FW_ZH)
    return en, zh


# Test result tracking
_results = {"pass": 0, "fail": 0, "skip": 0}
_current_module = ""


def set_module(name):
    global _current_module
    _current_module = name
    print(f"\n── {name} ──")


def test(name, condition, detail=""):
    """Assert a condition. Prints PASS/FAIL and updates counters."""
    ok = bool(condition)
    label = "PASS" if ok else "FAIL"
    print(f"  [{label}] {name}" + (f" — {detail}" if detail and not ok else ""))
    _results["pass" if ok else "fail"] += 1


def summary():
    """Print and return test summary."""
    total = _results["pass"] + _results["fail"]
    print(f"\n{'='*50}")
    print(f"Results: {_results['pass']}/{total} passed, {_results['fail']} failed")
    if _results["fail"] > 0:
        print("STATUS: FAILED")
    else:
        print("STATUS: PASSED")
    return _results["fail"] == 0
