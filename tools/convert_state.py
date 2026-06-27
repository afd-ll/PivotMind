#!/usr/bin/env python3
"""convert_state.py — PivotMind state file cross-architecture converter

Converts between binary (.dat) and JSON (.json) state formats.
JSON is architecture-agnostic — train on x86_64, deploy on ARM, or vice versa.

Usage:
    # Binary → JSON (export for cross-arch transfer)
    python3 convert_state.py pivotmind_state.dat -o pivotmind_state.json

    # JSON → Binary (import after transfer)
    python3 convert_state.py pivotmind_state.json -o pivotmind_state.dat

    # Info only (dump summary)
    python3 convert_state.py pivotmind_state.dat --info
"""

import sys
import os
import struct
import json
import argparse

STATE_FORMAT_VERSION = 5
SENTINEL = 0xDEADBEEF
FREQ_SENTINEL = -1  # marks start of frequency table section


def read_binary(path):
    """Read .dat file, return parsed dict."""
    with open(path, "rb") as f:
        data = f.read()

    pos = 0
    result = {
        "format_version": STATE_FORMAT_VERSION,
        "feature_dim": 512,
        "nodes": [],
        "cross_links": [],
    }

    # Header
    fmt_ver, feat_dim = struct.unpack_from("<ii", data, pos)
    pos += 8
    result["format_version"] = fmt_ver
    result["feature_dim"] = feat_dim

    # Node section
    while True:
        remaining = len(data) - pos
        if remaining < 4:
            break

        # Peek: check sentinel
        maybe = struct.unpack_from("<I", data, pos)[0]
        if maybe == SENTINEL:
            pos += 4
            break

        # Read node
        topo_type, node_id, concept_len = struct.unpack_from("<iii", data, pos)
        pos += 12

        if concept_len <= 0 or concept_len > 4096:
            print(f"  WARN: bad concept_len={concept_len} at pos {pos-4}, trying to recover")
            concept_len = 1

        concept = data[pos:pos + concept_len - 1].decode("utf-8", errors="replace")
        pos += concept_len

        activation, = struct.unpack_from("<f", data, pos)
        pos += 4

        node_feat_dim, = struct.unpack_from("<i", data, pos)
        pos += 4

        features = []
        for _ in range(feat_dim):
            fv, = struct.unpack_from("<f", data, pos)
            pos += 4
            features.append(round(fv, 6))  # round for JSON compactness

        edge_count, = struct.unpack_from("<i", data, pos)
        pos += 4
        if edge_count < 0 or edge_count > 100000:
            print(f"  WARN: bad edge_count={edge_count}, clamping to 0")
            edge_count = 0

        edges = []
        for _ in range(edge_count):
            tgt_len, = struct.unpack_from("<i", data, pos)
            pos += 4
            target = ""
            if tgt_len > 0:
                target = data[pos:pos + tgt_len - 1].decode("utf-8", errors="replace")
                pos += tgt_len
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
            "activation": round(activation, 6),
            "feature_dim": node_feat_dim,
            "features": features,
            "edge_count": edge_count,
            "edges": edges,
        })

    # Cross-link section
    if pos + 4 <= len(data):
        cross_count, = struct.unpack_from("<i", data, pos)
        pos += 4
        for _ in range(cross_count):
            ft, fn, tt, tn = struct.unpack_from("<iiii", data, pos)
            pos += 16
            w, = struct.unpack_from("<f", data, pos)
            pos += 4
            uc, = struct.unpack_from("<i", data, pos)
            pos += 4
            result["cross_links"].append({
                "from_topo": ft,
                "from_node": fn,
                "to_topo": tt,
                "to_node": tn,
                "weight": round(w, 6),
                "use_count": uc,
            })

    # Frequency table: read entire tail as raw bytes for round-trip fidelity
    if pos < len(data):
        result["_tail_raw"] = data[pos:].hex()
        result["_tail_pos"] = pos

    result["_total_nodes"] = len(result["nodes"])
    result["_total_cross_links"] = len(result["cross_links"])
    result["_file_kb"] = len(data) // 1024
    return result


def write_json(data, path):
    """Write parsed state to JSON file."""
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    nodes = data["_total_nodes"]
    links = data["_total_cross_links"]
    kb = data["_file_kb"]
    print(f"  Exported: {nodes} nodes, {links} cross-links ({kb} KB binary → JSON)")


def write_binary(data, path):
    """Write parsed state back to .dat binary file."""
    buf = bytearray()

    # Header
    buf += struct.pack("<i", data["format_version"])
    feat_dim = data.get("feature_dim", 512)
    buf += struct.pack("<i", feat_dim)

    # Nodes
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

    # Sentinel
    buf += struct.pack("<I", SENTINEL)

    # Cross-links
    links = data.get("cross_links", [])
    buf += struct.pack("<i", len(links))
    for link in links:
        buf += struct.pack("<iiii", link["from_topo"], link["from_node"],
                           link["to_topo"], link["to_node"])
        buf += struct.pack("<f", link["weight"])
        buf += struct.pack("<i", link["use_count"])

    # Frequency table / tail data restore
    tail_raw = data.get("_tail_raw", "")
    if tail_raw:
        buf += bytes.fromhex(tail_raw)

    with open(path, "wb") as f:
        f.write(buf)

    print(f"  Imported: {len(data['nodes'])} nodes, {len(links)} cross-links ({len(buf)//1024} KB)")


def print_info(data):
    """Print state summary."""
    nodes = data["_total_nodes"]
    links = data["_total_cross_links"]
    kb = data["_file_kb"]
    print(f"  Format version: {data['format_version']}")
    print(f"  Feature dim:    {data.get('feature_dim', 512)}")
    print(f"  Total nodes:    {nodes}")
    print(f"  Cross-links:    {links}")
    print(f"  File size:      {kb} KB")

    if nodes > 0:
        # Topology distribution
        topo_dist = {}
        for n in data["nodes"]:
            t = n["topo_type"]
            topo_dist[t] = topo_dist.get(t, 0) + 1
        topo_names = {0: "词汇", 1: "语义", 2: "情绪", 3: "语法", 4: "上下文",
                      5: "领域", 6: "语用", 7: "文化", 8: "概念", 9: "主拓扑", 10: "模板"}
        print(f"\n  Topology distribution:")
        for t, count in sorted(topo_dist.items()):
            name = topo_names.get(t, f"topo_{t}")
            print(f"    {name}: {count} nodes")

        # Edge stats
        total_edges = sum(len(n.get("edges", [])) for n in data["nodes"])
        avg_edges = total_edges / nodes if nodes else 0
        print(f"\n  Total edges:      {total_edges}")
        print(f"  Avg edges/node:   {avg_edges:.1f}")


def main():
    parser = argparse.ArgumentParser(description="PivotMind state file cross-architecture converter")
    parser.add_argument("input", help="Input file (.dat or .json)")
    parser.add_argument("-o", "--output", help="Output file (.json or .dat)")
    parser.add_argument("--info", action="store_true", help="Print state summary only")
    args = parser.parse_args()

    in_path = args.input
    if not os.path.exists(in_path):
        print(f"ERROR: {in_path} not found")
        return 1

    is_json_in = in_path.endswith(".json")

    # Read
    if is_json_in:
        with open(in_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        data.setdefault("format_version", STATE_FORMAT_VERSION)
        data.setdefault("feature_dim", 512)
        data.setdefault("_total_nodes", len(data.get("nodes", [])))
        data.setdefault("_total_cross_links", len(data.get("cross_links", [])))
    else:
        data = read_binary(in_path)

    # Output
    if args.info:
        print_info(data)
    elif args.output:
        if args.output.endswith(".json"):
            write_json(data, args.output)
        elif args.output.endswith(".dat"):
            write_binary(data, args.output)
        else:
            print("ERROR: output must be .json or .dat")
            return 1
    else:
        # Default: just show info
        print_info(data)

    return 0


if __name__ == "__main__":
    sys.exit(main())
