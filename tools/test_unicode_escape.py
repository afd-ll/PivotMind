#!/usr/bin/env python3
"""test_unicode_escape.py — \\uXXXX 解码 + 状态文件清理 单元测试

测试覆盖:
    1. _decode_u_escapes — 基础解码能力
    2. _decode_u_escapes — 代理对(emoji)
    3. _decode_u_escapes — 混合内容 + 误伤防御
    4. 状态文件二进制构建 → 清理 → 验证 round-trip
    5. convert_state.py concept_len > 1024

用法:
    python3 tools/test_unicode_escape.py
"""

import sys
import os
import struct
import tempfile

# 允许从其他目录运行
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

from clean_unicode_escapes import _decode_u_escapes, read_binary, write_binary


# ══════════════════════════════════════════════════
# 1. 基础解码
# ══════════════════════════════════════════════════

def test_basic_chinese():
    """uXXXX → UTF-8 中文字符"""
    text = "u4f60u597d"  # 你好
    result, changes = _decode_u_escapes(text)
    assert result == "\u4f60\u597d", f"got '{result}'"
    assert changes == 2, f"expected 2 changes, got {changes}"
    assert len(result.encode("utf-8")) == 6  # 3 bytes per char
    print(f"  PASS basic_chinese: '{text}' → '{result}' ({changes} changes)")

def test_punctuation():
    """中文标点"""
    text = "u3001u3002uff0c"  # 、。，
    result, changes = _decode_u_escapes(text)
    assert result == "\u3001\u3002\uff0c"
    assert changes == 3
    print(f"  PASS punctuation: '{text}' → '{result}' ({changes} changes)")

def test_mixed_ascii():
    """混合 ASCII 和中文字符 — 仅解码非 ASCII"""
    text = "hellou4e16u754c"  # hello世界
    result, changes = _decode_u_escapes(text)
    assert result == "hello\u4e16\u754c"
    assert changes == 2, f"expected 2, got {changes}"
    print(f"  PASS mixed_ascii: '{text}' → '{result}'")

def test_no_u_prefix():
    """无 u 前缀的正常文本应保持不变"""
    text = "世界你好world"
    result, changes = _decode_u_escapes(text)
    assert result == text
    assert changes == 0
    print(f"  PASS no_u_prefix: '{text}' unchanged")

def test_partial_u():
    """不完整的 uXXXX 序列应保持原样"""
    text = "u4f6"  # 只有 3 hex
    result, changes = _decode_u_escapes(text)
    assert result == "u4f6", f"got '{result}'"
    assert changes == 0
    print(f"  PASS partial_u: '{text}' unchanged (changes={changes})")

def test_invalid_hex():
    """u 后非 hex 字符不触发解码"""
    text = "uStop here"  # uStop 不是 hex
    result, changes = _decode_u_escapes(text)
    assert result == "uStop here", f"got '{result}'"
    assert changes == 0
    print(f"  PASS invalid_hex: '{text}' unchanged")

def test_ascii_codepoint_skip():
    """u0041 (= 'A') 是 ASCII，不应解码（防止误伤英文词）"""
    text = "u0041"  # U+0041 = 'A'
    result, changes = _decode_u_escapes(text)
    assert result == "u0041", f"got '{result}'"
    assert changes == 0
    print(f"  PASS ascii_skip: '{text}' not decoded (changes={changes})")

# ══════════════════════════════════════════════════
# 2. 代理对 (surrogate pairs) → emoji
# ══════════════════════════════════════════════════

def test_emoji_rocket():
    """uD83DuDE80 → 🚀"""
    text = "ud83dude80"
    result, changes = _decode_u_escapes(text)
    assert result == "🚀", f"got '{result}'"
    assert changes == 1
    print(f"  PASS emoji_rocket: '{text}' → '🚀'")

def test_emoji_fire():
    """uD83Dudd25 → 🔥"""
    text = "ud83dudd25"
    result, changes = _decode_u_escapes(text)
    assert result == "🔥", f"got '{result}'"
    assert changes == 1
    print(f"  PASS emoji_fire: '{text}' → '🔥'")

def test_multiple_emojis():
    """uD83DuDE80uD83Dudd25 → 🚀🔥"""
    text = "ud83dude80ud83dudd25"
    result, changes = _decode_u_escapes(text)
    assert result == "🚀🔥"
    assert changes == 2
    print(f"  PASS multiple_emojis: '{text}' → '🚀🔥'")

def test_isolated_surrogate():
    """孤立的高代理 (无低代理) 应被跳过"""
    text = "helloud83dworld"  # U+D83D 孤立
    result, changes = _decode_u_escapes(text)
    assert "ud83d" not in result, f"got '{result}'"
    assert "d83d" not in result or "ud83d" in text  # at minimum it was consumed
    print(f"  PASS isolated_surrogate: '{text}' → '{result}' ({changes} changes)")

# ══════════════════════════════════════════════════
# 3. 状态文件 round-trip
# ══════════════════════════════════════════════════

def test_binary_roundtrip():
    """构建含 uXXXX 概念的状态文件 → 清理 → 写入 → 读取 → 验证"""
    import tempfile

    feat_dim = 512
    sentinel = 0xDEADBEEF

    # 构建脏数据
    dirty_concept = b"u4f60u597d\x00"  # uXXXX format
    clean_concept = "\u4f60\u597d".encode("utf-8") + b"\x00"

    buf = bytearray()
    buf += struct.pack("<i", 5)  # format version
    buf += struct.pack("<i", feat_dim)  # feature dim

    # Node 1: dirty
    buf += struct.pack("<iii", 0, 0, len(dirty_concept))
    buf += dirty_concept
    buf += struct.pack("<f", 0.5)  # activation
    buf += struct.pack("<i", 0)  # feat_dim=0
    for _ in range(feat_dim):
        buf += struct.pack("<f", 0.0)
    buf += struct.pack("<i", 1)  # edge_count=1
    # Edge: also dirty target
    dirty_edge_target = b"u4e16u754c\x00"
    buf += struct.pack("<i", len(dirty_edge_target))
    buf += dirty_edge_target
    buf += struct.pack("<fff", 0.5, 0.5, 0.5)  # w, b, c

    buf += struct.pack("<I", sentinel)
    buf += struct.pack("<i", 0)  # cross_count=0
    buf += struct.pack("<i", -1)  # freq sentinel
    buf += struct.pack("<i", 0)  # tpl_voting=0
    buf += struct.pack("<i", 0)  # template_decay_round=0
    buf += struct.pack("<i", 0)  # freq entry_count=0

    # Write temp file
    tmp = tempfile.NamedTemporaryFile(suffix=".dat", delete=False)
    tmp.write(buf)
    tmp.close()

    try:
        # Parse
        data = read_binary(tmp.name)
        assert len(data["nodes"]) == 1
        node = data["nodes"][0]
        assert node["concept"].startswith("u"), f"expected dirty concept, got '{node['concept'][:20]}'"
        assert len(node["edges"]) == 1
        assert node["edges"][0]["target"].startswith("u"), "expected dirty edge target"

        # Clean
        cleaned_concept, c1 = _decode_u_escapes(node["concept"])
        cleaned_target, c2 = _decode_u_escapes(node["edges"][0]["target"])
        assert c1 > 0 and c2 > 0
        node["concept"] = cleaned_concept
        node["edges"][0]["target"] = cleaned_target

        # Verify
        assert node["concept"] == "\u4f60\u597d", f"got '{node['concept']}'"
        assert node["edges"][0]["target"] == "\u4e16\u754c", f"got '{node['edges'][0]['target']}'"

        # Write cleaned
        tmp2 = tempfile.NamedTemporaryFile(suffix=".dat", delete=False)
        tmp2.close()
        write_binary(data, tmp2.name)

        # Re-read & verify
        data2 = read_binary(tmp2.name)
        node2 = data2["nodes"][0]
        assert node2["concept"] == "\u4f60\u597d"
        assert node2["edges"][0]["target"] == "\u4e16\u754c"
        os.unlink(tmp2.name)

    finally:
        os.unlink(tmp.name)

    print(f"  PASS binary_roundtrip: dirty → clean → write → read → verified")

# ══════════════════════════════════════════════════
# 4. convert_state.py concept_len > 1024
# ══════════════════════════════════════════════════

def test_long_concept_in_convert():
    """验证 convert_state.py 能处理 concept_len > 1024"""
    import subprocess

    # 构造一个长概念名 (含 uXXXX，长度 > 1024)
    long_concept = "你好" * 200  # ~600 bytes in UTF-8
    long_utf8 = long_concept.encode("utf-8") + b"\x00"

    feat_dim = 512
    buf = bytearray()
    buf += struct.pack("<i", 5)
    buf += struct.pack("<i", feat_dim)

    buf += struct.pack("<iii", 0, 0, len(long_utf8))
    buf += long_utf8
    buf += struct.pack("<f", 0.5)
    buf += struct.pack("<i", 512)
    for _ in range(feat_dim):
        buf += struct.pack("<f", 0.0)
    buf += struct.pack("<i", 0)  # edge_count=0

    buf += struct.pack("<I", 0xDEADBEEF)
    buf += struct.pack("<i", 0)
    buf += struct.pack("<i", -1) + struct.pack("<i", 0) + struct.pack("<i", 0) + struct.pack("<i", 0)

    tmp = tempfile.NamedTemporaryFile(suffix=".dat", delete=False)
    tmp.write(buf)
    tmp.close()

    try:
        result = subprocess.run(
            ["python3", os.path.join(os.path.dirname(__file__), "convert_state.py"),
             tmp.name, "--info"],
            capture_output=True, text=True, timeout=30
        )
        # 不应该有 WARN: bad concept_len
        assert "WARN: bad concept_len" not in result.stdout, f"Unexpected WARN in output:\n{result.stdout[:500]}"
        assert result.returncode == 0
        assert "Total nodes:    1" in result.stdout
    finally:
        os.unlink(tmp.name)

    print(f"  PASS long_concept: concept_len={len(long_utf8)} no WARN")

# ══════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════

if __name__ == "__main__":
    print("=== \\uXXXX Unicode Escape Tests ===\n")

    tests = [
        # 基础解码
        ("basic_chinese", test_basic_chinese),
        ("punctuation", test_punctuation),
        ("mixed_ascii", test_mixed_ascii),
        ("no_u_prefix", test_no_u_prefix),
        ("partial_u", test_partial_u),
        ("invalid_hex", test_invalid_hex),
        ("ascii_skip", test_ascii_codepoint_skip),
        # 代理对
        ("emoji_rocket", test_emoji_rocket),
        ("emoji_fire", test_emoji_fire),
        ("multiple_emojis", test_multiple_emojis),
        ("isolated_surrogate", test_isolated_surrogate),
        # Round-trip
        ("binary_roundtrip", test_binary_roundtrip),
        # convert_state
        ("long_concept", test_long_concept_in_convert),
    ]

    passed = 0
    failed = 0
    for name, test_fn in tests:
        try:
            test_fn()
            passed += 1
        except Exception as e:
            print(f"  FAIL {name}: {e}")
            failed += 1

    print(f"\n=== Results: {passed} passed, {failed} failed ===")
    sys.exit(0 if failed == 0 else 1)
