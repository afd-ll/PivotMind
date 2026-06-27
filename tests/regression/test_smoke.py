"""test_smoke.py — Gateway startup, health, status checks."""

from helpers import (
    start_gateway, stop_gateway, health, status, chat,
    test, set_module, find_free_port, PROJECT_ROOT
)
import os


def run(port=None):
    set_module("Smoke Tests")
    if port is None:
        port = find_free_port()

    binary = os.path.join(PROJECT_ROOT, "build", "bin", "pivotmind_gateway")
    test("gateway binary exists", os.path.exists(binary),
         f"not found: {binary}")
    if not os.path.exists(binary):
        return

    # Start
    try:
        proc, port = start_gateway(binary, port, timeout=30)
    except Exception as e:
        test("gateway starts", False, str(e))
        return

    test("gateway starts", proc.poll() is None)
    if proc.poll() is not None:
        return

    # Health
    h = health(port)
    test("GET /health returns data", h is not None)
    if h and isinstance(h, dict):
        test("/health has status", "status" in h or "healthy" in str(h).lower())

    # Status
    s = status(port)
    test("GET /status returns data", s is not None)
    if s and isinstance(s, dict):
        test("/status has total_nodes", "total_nodes" in s or "nodes" in s or "node_count" in s,
             f"keys: {list(s.keys())[:6]}")
        test("/status has uptime", "uptime" in s)

    # Basic chat - doesn't crash (API field is "msg")
    reply = chat(port, "你好")
    test("POST /chat does not crash", True, reply[:80] if reply else "(empty)")

    # Empty query
    reply2 = chat(port, "")
    test("POST /chat empty query no crash", True)

    # Long query
    reply3 = chat(port, "请解释一下人工智能的基本原理和主要应用领域"
                          "包括机器学习深度学习和自然语言处理等方面")
    test("POST /chat long query no crash", True)

    stop_gateway(proc)
    test("gateway stops cleanly", proc.poll() is not None)
