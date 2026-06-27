#!/usr/bin/env python3
"""clean_unicode_escapes.py — 修复状态文件中 uXXXX 格式的脏概念名

将 literal "u9a71u52a8..." 格式解码为真正的 UTF-8 "驱动的..."
concept_len 同步更新，边目标名同样处理。

用法:
    python3 clean_unicode_escapes.py pivotmind_state.dat
    python3 clean_unicode_escapes.py pivotmind_state.dat -o cleaned_state.dat
    python3 clean_unicode_escapes.py pivotmind_state.dat --dry-run    (仅统计不写入)

算法来源: perception.c _decode_unicode_escapes (分支 0: uXXXX 无反斜杠)
"""

import re
import sys
import struct
import argparse

STATE_FORMAT_VERSION = 5
SENTINEL = 0xDEADBEEF

# 匹配无前缀的 uXXXX（非 ASCII 码点，避免误伤英文词如 "usage"）
_U_HEX_RE = re.compile(r'u([0-9a-fA-F]{4})')


def _decode_u_escapes(text: str) -> tuple:
    """解码 uXXXX 序列为 UTF-8 字符串，返回 (decoded_str, changed_count).

    与 perception.c _decode_unicode_escapes 逻辑一致：
    仅当 cp >= 0x80 (非 ASCII) 时才解码，避免误伤英文单词。
    额外处理代理对 (emoji)：uD83DuDE80 → 🚀 等。
    """
    changed = 0
    result = []
    i = 0
    while i < len(text):
        if (text[i] == 'u' and i + 5 <= len(text) and
                all(c in '0123456789abcdefABCDEF' for c in text[i+1:i+5])):
            cp = int(text[i+1:i+5], 16)
            # 代理对: high surrogate + low surrogate → 4 字节 UTF-8
            if 0xD800 <= cp <= 0xDBFF and i + 10 <= len(text):
                # 尝试解析 low surrogate
                if (text[i+5] == 'u' and
                        all(c in '0123456789abcdefABCDEF' for c in text[i+6:i+10])):
                    low = int(text[i+6:i+10], 16)
                    if 0xDC00 <= low <= 0xDFFF:
                        cp = 0x10000 + (cp - 0xD800) * 0x400 + (low - 0xDC00)
                        if cp >= 0x80:
                            result.append(chr(cp))
                            changed += 1
                            i += 10
                            continue
            # 跳过孤立代理
            if 0xD800 <= cp <= 0xDFFF:
                i += 5
                continue
            if cp >= 0x80:
                result.append(chr(cp))
                changed += 1
                i += 5
                continue
        result.append(text[i])
        i += 1
    return ''.join(result), changed


def read_binary(path: str) -> dict:
    """读取 .dat 文件为中间字典。与 convert_state.py 解析逻辑一致。"""
    with open(path, "rb") as f:
        data = f.read()

    pos = 0
    result = {
        "format_version": STATE_FORMAT_VERSION,
        "feature_dim": 512,
        "nodes": [],
        "cross_links": [],
    }

    fmt_ver, feat_dim = struct.unpack_from("<ii", data, pos)
    pos += 8
    result["format_version"] = fmt_ver
    result["feature_dim"] = feat_dim

    while True:
        if pos + 4 > len(data):
            break
        maybe = struct.unpack_from("<I", data, pos)[0]
        if maybe == SENTINEL:
            pos += 4
            break

        topo_type, node_id, concept_len = struct.unpack_from("<iii", data, pos)
        pos += 12

        if concept_len <= 0 or concept_len > 4096:
            print(f"  ERROR: bad concept_len={concept_len} at pos {pos-4}, aborting")
            break

        if pos + concept_len > len(data): break
        concept_raw = data[pos:pos + concept_len - 1]
        concept = concept_raw.decode("utf-8", errors="replace")
        pos += concept_len

        activation = struct.unpack_from("<f", data, pos)[0]
        pos += 4

        if pos + 4 > len(data): break
        node_feat_dim = struct.unpack_from("<i", data, pos)[0]
        pos += 4

        features = []
        if pos + feat_dim * 4 > len(data): break
        for _ in range(feat_dim):
            fv = struct.unpack_from("<f", data, pos)[0]
            pos += 4
            features.append(round(fv, 6))

        if pos + 4 > len(data): break
        edge_count = struct.unpack_from("<i", data, pos)[0]
        pos += 4
        if edge_count < 0 or edge_count > 100000:
            print(f"  ERROR: bad edge_count={edge_count}, aborting")
            break

        edges = []
        for _ in range(edge_count):
            tgt_len = struct.unpack_from("<i", data, pos)[0]
            pos += 4
            target = ""
            if tgt_len > 0 and tgt_len <= 4096:
                target_raw = data[pos:pos + tgt_len - 1]
                target = target_raw.decode("utf-8", errors="replace")
                pos += tgt_len
            elif tgt_len > 4096:
                pos += tgt_len  # skip corrupted
            w, b, c = struct.unpack_from("<fff", data, pos)
            pos += 12
            edges.append({
                "target": target,
                "weight": round(w, 6),
                "bias": round(b, 6),
                "confidence": round(c, 6),
            })

        result["nodes"].append({
            "topo_type": topo_type,
            "node_id": node_id,
            "concept": concept,
            "concept_raw": concept_raw,
            "activation": round(activation, 6),
            "feature_dim": node_feat_dim,
            "features": features,
            "edge_count": edge_count,
            "edges": edges,
        })

    # Cross-link section
    if pos + 4 <= len(data):
        cross_count = struct.unpack_from("<i", data, pos)[0]
        pos += 4
        for _ in range(cross_count):
            ft, fn, tt, tn = struct.unpack_from("<iiii", data, pos)
            pos += 16
            w = struct.unpack_from("<f", data, pos)[0]
            pos += 4
            uc = struct.unpack_from("<i", data, pos)[0]
            pos += 4
            result["cross_links"].append({
                "from_topo": ft,
                "from_node": fn,
                "to_topo": tt,
                "to_node": tn,
                "weight": round(w, 6),
                "use_count": uc,
            })

    # Tail data for round-trip fidelity
    if pos < len(data):
        result["_tail_raw"] = data[pos:].hex()
        result["_tail_pos"] = pos

    result["_total_nodes"] = len(result["nodes"])
    result["_total_cross_links"] = len(result["cross_links"])
    result["_file_kb"] = len(data) // 1024
    return result


def write_binary(data: dict, path: str) -> None:
    """写入 .dat 二进制文件。与 convert_state.py write_binary 逻辑一致。"""
    buf = bytearray()
    feat_dim = data.get("feature_dim", 512)

    buf += struct.pack("<i", data["format_version"])
    buf += struct.pack("<i", feat_dim)

    for node in data["nodes"]:
        buf += struct.pack("<i", node["topo_type"])
        buf += struct.pack("<i", node["node_id"])

        concept_bytes = node["concept"].encode("utf-8") + b"\x00"
        buf += struct.pack("<i", len(concept_bytes))
        buf += concept_bytes

        buf += struct.pack("<f", node["activation"])

        fd = node.get("feature_dim", 0)
        buf += struct.pack("<i", fd)
        feats = node.get("features", [])
        for i in range(feat_dim):
            v = feats[i] if i < len(feats) else 0.0
            buf += struct.pack("<f", v)

        edges = node.get("edges", [])
        buf += struct.pack("<i", len(edges))
        for e in edges:
            tgt = e.get("target", "")
            if tgt:
                tgt_bytes = tgt.encode("utf-8") + b"\x00"
                buf += struct.pack("<i", len(tgt_bytes))
                buf += tgt_bytes
            else:
                buf += struct.pack("<i", 0)
            buf += struct.pack("<fff", e["weight"], e.get("bias", 0), e.get("confidence", 0))

    buf += struct.pack("<I", SENTINEL)

    links = data.get("cross_links", [])
    buf += struct.pack("<i", len(links))
    for link in links:
        buf += struct.pack("<iiii", link["from_topo"], link["from_node"],
                           link["to_topo"], link["to_node"])
        buf += struct.pack("<f", link["weight"])
        buf += struct.pack("<i", link["use_count"])

    tail_raw = data.get("_tail_raw", "")
    if tail_raw:
        buf += bytes.fromhex(tail_raw)

    with open(path, "wb") as f:
        f.write(buf)


def main():
    parser = argparse.ArgumentParser(
        description="修复 PivotMind 状态文件中 uXXXX 格式的脏概念名")
    parser.add_argument("input", help="输入状态文件 (.dat)")
    parser.add_argument("-o", "--output", help="输出文件 (默认: <input>.cleaned)")
    parser.add_argument("--dry-run", action="store_true", help="仅统计，不写入文件")
    args = parser.parse_args()

    if args.output:
        out_path = args.output
    else:
        if args.input.endswith(".dat"):
            out_path = args.input[:-4] + ".cleaned.dat"
        else:
            out_path = args.input + ".cleaned"

    # 读取
    print(f"[读取] {args.input}")
    data = read_binary(args.input)
    total_nodes = data["_total_nodes"]
    total_edges = data["_total_cross_links"]
    kb = data["_file_kb"]
    print(f"  格式版本: {data['format_version']}, 特征维度: {data['feature_dim']}")
    print(f"  节点: {total_nodes}, 跨拓扑链接: {total_edges}, 文件: {kb} KB")

    # 清理概念名
    cleaned_nodes = 0
    cleaned_edges = 0
    old_bytes = 0
    new_bytes = 0

    for node in data["nodes"]:
        concept = node["concept"]
        # 只处理含 "u" 前缀的脏概念
        if concept.startswith("u") and len(concept) > 5:
            cleaned, changes = _decode_u_escapes(concept)
            if changes > 0:
                old_bytes += len(concept.encode("utf-8"))
                new_bytes += len(cleaned.encode("utf-8"))
                node["concept"] = cleaned
                cleaned_nodes += 1

        # 处理边的目标概念名
        for edge in node["edges"]:
            tgt = edge.get("target", "")
            if tgt and tgt.startswith("u") and len(tgt) > 5:
                cleaned, changes = _decode_u_escapes(tgt)
                if changes > 0:
                    edge["target"] = cleaned
                    cleaned_edges += 1

    space_saved = old_bytes - new_bytes if old_bytes > 0 else 0
    print(f"\n[清理] 节点概念: {cleaned_nodes} 个已修复")
    print(f"  边目标名:     {cleaned_edges} 个已修复")
    if space_saved > 0:
        kb_saved = space_saved / 1024.0
        mb_saved = space_saved / (1024.0 * 1024.0)
        print(f"  估计节省空间: {space_saved} 字节 ({kb_saved:.1f} KB / {mb_saved:.2f} MB)")

    if cleaned_nodes == 0 and cleaned_edges == 0:
        print("  无需修复，概念名已经是干净的 UTF-8。")
        return 0

    if args.dry_run:
        print(f"\n[dry-run] 不写入文件。")
        return 0

    # 写入
    print(f"\n[写入] {out_path}")
    write_binary(data, out_path)
    new_kb = len(open(out_path, "rb").read()) // 1024
    print(f"  完成。新文件: {new_kb} KB (节省 {kb - new_kb if new_kb < kb else 0} KB)")


if __name__ == "__main__":
    main()
