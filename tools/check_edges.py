#!/usr/bin/env python3
"""检查指定字符的边情况"""
import sys, struct

path = sys.argv[1] if len(sys.argv) > 1 else "pivotmind_state.dat"
targets = sys.argv[2:] if len(sys.argv) > 2 else ["学", "习", "机", "器"]

with open(path, 'rb') as f:
    data = f.read()

pos = 4  # skip format version
nodes = {}  # id -> concept

while pos + 16 < len(data):
    topo_type = struct.unpack_from('i', data, pos)[0]
    node_id = struct.unpack_from('i', data, pos+4)[0]
    concept_len = struct.unpack_from('i', data, pos+8)[0]
    if concept_len <= 0 or concept_len > 256:
        break
    concept = data[pos+12:pos+12+concept_len].decode('utf-8', errors='replace').rstrip('\x00')
    activation = struct.unpack_from('f', data, pos+12+concept_len)[0]
    conn_count = struct.unpack_from('i', data, pos+12+concept_len+4)[0]
    
    conn_pos = pos + 12 + concept_len + 8
    conns = []
    for c in range(conn_count):
        target_id = struct.unpack_from('i', data, conn_pos)[0]
        weight = struct.unpack_from('f', data, conn_pos+4)[0]
        conf = struct.unpack_from('f', data, conn_pos+12)[0]
        conns.append((target_id, weight, conf))
        conn_pos += 16
    nodes[node_id] = {
        'concept': concept, 'type': topo_type,
        'activation': activation, 'conns': conns
    }
    pos = conn_pos

# Print targets
for t in targets:
    found = [(nid, n) for nid, n in nodes.items() if n['concept'] == t]
    if not found:
        print(f"「{t}」: 不存在")
        continue
    for nid, n in found:
        tname = {0:'VOCAB',8:'CONCEPT'}.get(n['type'], f'T{n['type']}')
        print(f"「{t}」({tname}#{nid}, act={n['activation']:.3f}): {len(n['conns'])} 条边")
        sorted_conns = sorted(n['conns'], key=lambda x: x[2], reverse=True)[:8]
        for tid, w, c in sorted_conns:
            target_concept = nodes.get(tid, {}).get('concept', f'#{tid}')
            print(f"  → {target_concept} (weight={w:.3f}, conf={c:.3f})")
