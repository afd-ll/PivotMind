"""test_stress.py — Concurrent request stability."""

from helpers import (
    start_gateway, stop_gateway, chat, health,
    test, set_module, find_free_port
)
import threading
import time


def run(port=None, count=20):
    set_module("Stress Tests")
    if port is None:
        port = find_free_port()

    proc, port = start_gateway(timeout=30)
    results = {"ok": 0, "fail": 0, "timeout": 0}
    lock = threading.Lock()

    def worker(q):
        try:
            reply = chat(port, q)
            if reply:
                with lock:
                    results["ok"] += 1
            else:
                with lock:
                    results["fail"] += 1
        except Exception:
            with lock:
                results["timeout"] += 1

    questions = [
        "你好", "什么是学习", "hello", "how are you",
        "人工智能", "机器学习", "神经网络", "深度学习",
        "什么是意识", "科学是什么",
    ] * (count // 10 + 1)
    questions = questions[:count]

    threads = []
    for q in questions:
        t = threading.Thread(target=worker, args=(q,))
        threads.append(t)
        t.start()

    for t in threads:
        t.join(timeout=15)

    test(f"all {count} requests completed", results["fail"] + results["timeout"] == 0,
         f"ok={results['ok']} fail={results['fail']} timeout={results['timeout']}")
    test(f"ok rate >= 80%", results["ok"] >= count * 0.8,
         f"rate={results['ok']/count:.0%}")

    # Health check after stress
    time.sleep(1)
    h = health(port)
    test("server healthy after stress", h is not None)

    stop_gateway(proc)
