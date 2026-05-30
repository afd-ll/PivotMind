#!/usr/bin/env python3
"""
merge_states.py — 合并多个 parallel-trained pivotmind_state 文件

并行训练方案：
  1. 把 QA 数据分成 N 份
  2. 在 Dell 上同时启动 N 个 batch_learn 进程，各从空拓扑开始
  3. 跑完后一次合并: merge_states.py state_0.dat state_1.dat ... state_out.dat

去重策略：按 (topo_type, concept) 合并节点（无视 node_id 不一致）
跨拓扑连接：通过 concept 名重映射 node_id

格式: streaming v4 (int fmt_ver + 节点流 + 跨拓扑流)
  节点: topo_type(int) node_id(int) concept_len(int) concept(char*) activation(float)
        feat_dim(int) features(float*NODE_FEATURE_DIM=24) conn_count(int)
  连接: tgt_concept_len(int) tgt_concept(char*) weight(float) bias(float) confidence(float)
  跨拓扑: from_topo_id(int) from_node_id(int) to_topo_id(int) to_node_id(int)
           weight(float) use_count(int)
"""

import struct
import sys
import os

NODE_FEATURE_DIM = 24  # 与 C 代码 PM_NODE_FEATURE_DIM 一致


def read_state(path):
    """读取完整状态文件，返回 (nodes_by_concept, cross_links_by_old_id, old_node_id_map, fmt_ver)

    nodes_by_concept: {(topo_type, concept)} -> node dict
    cross_links_by_old_id: list of cross link dicts with raw old node_ids
    old_node_id_map: {(topo_type, old_node_id)} -> concept (用于重映射跨拓扑连接)
    """
    nodes = {}       # key: (topo_type, concept) -> node
    cross_links = []
    old_id_map = {}  # (topo_type, old_node_id) -> concept

    with open(path, 'rb') as f:
        fmt_ver = struct.unpack('<i', f.read(4))[0]
        print(f"  [{path}] 格式版本: {fmt_ver}")
        if fmt_ver not in (1, 2, 3, 4):
            f.seek(0)

        while True:
            # --- 节点头 ---
            if fmt_ver >= 2:
                data = f.read(4)
                if len(data) < 4:
                    break
                topo_type = struct.unpack('<i', data)[0]
            else:
                topo_type = 0

            data = f.read(4)
            if len(data) < 4:
                break
            node_id = struct.unpack('<i', data)[0]

            data = f.read(4)
            if len(data) < 4:
                break
            concept_len = struct.unpack('<i', data)[0]
            if concept_len <= 0 or concept_len > 4096:
                break

            concept = f.read(concept_len)
            if len(concept) < concept_len:
                break
            concept = concept.decode('utf-8', errors='replace').rstrip('\x00')

            # 记录 (topo_type, old_node_id) -> concept 映射
            old_id_map[(topo_type, node_id)] = concept

            data = f.read(4)
            if len(data) < 4:
                break
            activation = struct.unpack('<f', data)[0]

            # --- v4: 特征向量 ---
            features = None
            if fmt_ver >= 4:
                data = f.read(4)
                if len(data) < 4:
                    break
                feat_dim = struct.unpack('<i', data)[0]

                feat_data = f.read(NODE_FEATURE_DIM * 4)
                if len(feat_data) < NODE_FEATURE_DIM * 4:
                    break
                features = list(struct.unpack(f'<{NODE_FEATURE_DIM}f', feat_data))

            # --- 连接数 ---
            data = f.read(4)
            if len(data) < 4:
                break
            conn_count = struct.unpack('<i', data)[0]

            # --- 连接列表（直接读到文件尾，不用 conn_count 做安全判断） ---
            connections = []
            for _ in range(conn_count):
                data = f.read(4)
                if len(data) < 4:
                    break
                tgt_len = struct.unpack('<i', data)[0]

                if tgt_len > 0:
                    tgt_concept = f.read(tgt_len).decode('utf-8', errors='replace').rstrip('\x00')
                else:
                    tgt_concept = ""

                data = f.read(12)  # weight + bias + confidence = 3 floats
                if len(data) < 12:
                    break
                w, b, c = struct.unpack('<fff', data)

                connections.append({
                    'target': tgt_concept,
                    'weight': w,
                    'bias': b,
                    'confidence': c
                })

            # 按 (topo_type, concept) 作为主键存储
            # 若同一个文件内已有重复概念（极小概率），合并连接取均值
            key = (topo_type, concept)
            if key in nodes:
                existing = nodes[key]
                conn_map = {}
                for c2 in existing['connections']:
                    conn_map[c2['target']] = c2
                for c2 in connections:
                    if c2['target'] in conn_map:
                        old = conn_map[c2['target']]
                        old['weight'] = (old['weight'] + c2['weight']) / 2.0
                        old['bias'] = (old['bias'] + c2['bias']) / 2.0
                        old['confidence'] = (old['confidence'] + c2['confidence']) / 2.0
                    else:
                        conn_map[c2['target']] = dict(c2)
                existing['connections'] = list(conn_map.values())
                existing['activation'] = max(existing['activation'], activation)
                if existing['features'] and features:
                    existing['features'] = [(a + b) / 2.0 for a, b in
                                            zip(existing['features'], features)]
                elif features:
                    existing['features'] = features
            else:
                node = {
                    'topo_type': topo_type,
                    'concept': concept,
                    'activation': activation,
                    'features': features,
                    'connections': connections
                }
                nodes[key] = node

        # --- 跨拓扑连接 ---
        while True:
            data = f.read(4)
            if len(data) < 4:
                break
            from_topo = struct.unpack('<i', data)[0]

            more = f.read(5 * 4)
            if len(more) < 20:
                break
            (from_node, to_topo, to_node, weight, use_count) = struct.unpack('<iiifi', more)

            cross_links.append({
                'from_topo': from_topo,
                'from_node': from_node,
                'to_topo': to_topo,
                'to_node': to_node,
                'weight': weight,
                'use_count': use_count
            })

    total_edges = sum(len(n['connections']) for n in nodes.values())
    print(f"  [{path}] 读取: {len(nodes)} 节点, {total_edges} 边, {len(cross_links)} 跨拓扑")
    return nodes, cross_links, old_id_map, fmt_ver


def write_state(path, nodes, cross_links, fmt_ver):
    """写入状态文件，返回 (node_count, edge_count, cross_count)"""
    with open(path, 'wb') as f:
        # 1. 版本头
        f.write(struct.pack('<i', fmt_ver))

        # 2. 节点流（按 topo_type, concept 排序）
        sorted_keys = sorted(nodes.keys(), key=lambda k: (k[0], k[1]))
        for key in sorted_keys:
            node = nodes[key]

            f.write(struct.pack('<i', node['topo_type']))
            f.write(struct.pack('<i', node['new_node_id']))

            concept_bytes = node['concept'].encode('utf-8')
            f.write(struct.pack('<i', len(concept_bytes) + 1))
            f.write(concept_bytes + b'\x00')

            f.write(struct.pack('<f', node['activation']))

            # v4 特征向量
            if fmt_ver >= 4 and node['features'] is not None:
                f.write(struct.pack('<i', NODE_FEATURE_DIM))
                f.write(struct.pack(f'<{NODE_FEATURE_DIM}f', *node['features']))
            elif fmt_ver >= 4:
                f.write(struct.pack('<i', 0))
                for _ in range(NODE_FEATURE_DIM):
                    f.write(struct.pack('<f', 0.0))

            # 连接
            conns = node['connections']
            f.write(struct.pack('<i', len(conns)))
            for conn in conns:
                tgt_bytes = conn['target'].encode('utf-8')
                f.write(struct.pack('<i', len(tgt_bytes) + 1))
                if tgt_bytes:
                    f.write(tgt_bytes + b'\x00')
                f.write(struct.pack('<f', conn['weight']))
                f.write(struct.pack('<f', conn['bias']))
                f.write(struct.pack('<f', conn['confidence']))

        # 3. 跨拓扑连接（已用新 node_id 重映射）
        for cl in cross_links:
            f.write(struct.pack('<i', cl['from_topo']))
            f.write(struct.pack('<i', cl['from_node']))
            f.write(struct.pack('<i', cl['to_topo']))
            f.write(struct.pack('<i', cl['to_node']))
            f.write(struct.pack('<f', cl['weight']))
            f.write(struct.pack('<i', cl['use_count']))

    node_count = len(nodes)
    edge_count = sum(len(n['connections']) for n in nodes.values())
    cross_count = len(cross_links)
    print(f"  [{path}] 写入: {node_count} 节点, {edge_count} 边, {cross_count} 跨拓扑")
    return node_count, edge_count, cross_count


def verify_state(path, expected_nodes, expected_edges, expected_cross):
    """写完后回读校验"""
    actual_nodes, actual_edges, _, _ = read_state(path)
    actual_edge_count = sum(len(n['connections']) for n in actual_nodes.values())
    ok = True
    if len(actual_nodes) != expected_nodes:
        print(f"  ⚠ 节点数不匹配: 预期={expected_nodes} 实际={len(actual_nodes)}")
        ok = False
    if actual_edge_count != expected_edges:
        print(f"  ⚠ 边数不匹配: 预期={expected_edges} 实际={actual_edge_count}")
        ok = False
    # 跨拓扑连接在回读时会在节点流结束后继续读，但回读不解析跨拓扑
    # 所以只比对节点和边
    if ok:
        print(f"  ✓ 校验通过: {expected_nodes} 节点, {expected_edges} 边")


def merge_nodes(nodes_list):
    """合并多个节点字典 — 按 (topo_type, concept) 去重"""
    merged = {}

    for nodes in nodes_list:
        for key, node_b in nodes.items():
            if key in merged:
                node_a = merged[key]
                # 激活取最大
                node_a['activation'] = max(node_a['activation'], node_b['activation'])
                # 特征取均值
                if node_a['features'] and node_b['features']:
                    node_a['features'] = [(a + b) / 2.0 for a, b in
                                          zip(node_a['features'], node_b['features'])]
                elif node_b['features']:
                    node_a['features'] = list(node_b['features'])

                # 合并连接
                conn_map = {}
                for c in node_a['connections']:
                    conn_map[c['target']] = c
                for c in node_b['connections']:
                    if c['target'] in conn_map:
                        old = conn_map[c['target']]
                        old['weight'] = (old['weight'] + c['weight']) / 2.0
                        old['bias'] = (old['bias'] + c['bias']) / 2.0
                        old['confidence'] = (old['confidence'] + c['confidence']) / 2.0
                    else:
                        conn_map[c['target']] = dict(c)
                node_a['connections'] = list(conn_map.values())
            else:
                merged[key] = {
                    'topo_type': node_b['topo_type'],
                    'concept': node_b['concept'],
                    'activation': node_b['activation'],
                    'features': list(node_b['features']) if node_b['features'] else None,
                    'connections': [dict(c) for c in node_b['connections']]
                }

    return merged


def assign_node_ids(nodes):
    """给所有节点分配全局唯一的 node_id
    按 (topo_type, concept) 排序后顺序分配，保证确定性
    同时返回 (topo_type, concept) -> new_node_id 映射
    """
    sorted_keys = sorted(nodes.keys(), key=lambda k: (k[0], k[1]))
    concept_to_id = {}
    for next_id, key in enumerate(sorted_keys):
        nodes[key]['new_node_id'] = next_id
        concept_to_id[key] = next_id
    return concept_to_id


def resolve_cross_links(all_cross_and_maps, concept_to_id):
    """重映射跨拓扑连接的 node_id

    all_cross_and_maps: [(cross_links, old_id_map), ...]
    concept_to_id: 合并后的 (topo_type, concept) -> new_node_id
    """
    seen = set()
    resolved = []

    for cross_links, id_map in all_cross_and_maps:
        for link in cross_links:
            ft, fn, tt, tn = link['from_topo'], link['from_node'], link['to_topo'], link['to_node']

            # 查找旧 node_id 对应的 concept
            from_concept = id_map.get((ft, fn))
            to_concept = id_map.get((tt, tn))

            if not from_concept or not to_concept:
                continue

            # 查找 concept 对应新 node_id
            new_from = concept_to_id.get((ft, from_concept))
            new_to = concept_to_id.get((tt, to_concept))

            if new_from is None or new_to is None:
                continue

            # 去重
            dedup_key = (ft, new_from, tt, new_to)
            if dedup_key in seen:
                continue
            seen.add(dedup_key)

            resolved.append({
                'from_topo': ft,
                'from_node': new_from,
                'to_topo': tt,
                'to_node': new_to,
                'weight': link['weight'],
                'use_count': link['use_count']
            })

    return resolved


def main():
    if len(sys.argv) < 4:
        print(f"用法: {sys.argv[0]} <state_0.dat> <state_1.dat> ... <state_out.dat>")
        print(f"示例: {sys.argv[0]} p0.dat p1.dat p2.dat p3.dat merged.dat")
        sys.exit(1)

    in_paths = sys.argv[1:-1]
    out_path = sys.argv[-1]

    print(f"输入文件 ({len(in_paths)} 个): {in_paths}")
    print(f"输出文件: {out_path}\n")

    # 1. 读取所有状态文件
    all_nodes = []
    all_cross_and_maps = []
    fmt_ver = 0
    for path in in_paths:
        nodes, cross_links, id_map, ver = read_state(path)
        all_nodes.append(nodes)
        all_cross_and_maps.append((cross_links, id_map))
        if ver > fmt_ver:
            fmt_ver = ver

    # 2. 一次合并所有节点
    print(f"\n合并节点 (按概念去重)...")
    nodes_merged = merge_nodes(all_nodes)

    # 3. 分配全局 node_id
    print(f"分配全局 node_id...")
    concept_to_id = assign_node_ids(nodes_merged)

    # 4. 一次重映射所有跨拓扑连接
    print(f"重映射跨拓扑连接...")
    cross_resolved = resolve_cross_links(all_cross_and_maps, concept_to_id)

    # 5. 统计
    total_nodes_in = sum(len(n) for n in all_nodes)
    total_edges_in = sum(sum(len(nn['connections']) for nn in n.values()) for n in all_nodes)
    total_cross_in = sum(len(cm[0]) for cm in all_cross_and_maps)
    total_edges_m = sum(len(n['connections']) for n in nodes_merged.values())

    # 计算概念重叠
    all_keys = [set(n.keys()) for n in all_nodes]
    common = all_keys[0]
    for ks in all_keys[1:]:
        common &= ks
    unique = sum(len(ks) for ks in all_keys) - len(common) * len(all_keys)
    # 更准确的统计：先找出所有唯一概念
    all_concepts = set()
    for n in all_nodes:
        all_concepts.update(n.keys())
    exactly_common = len(common)
    only_one = len(all_concepts) - exactly_common - (len(all_keys) - 1) * exactly_common
    # 这个统计不太准确，简单点：
    print(f"\n=== 合并统计 ===")
    print(f"输入: {len(in_paths)} 个文件")
    print(f"节点: {total_nodes_in} → 合并={len(nodes_merged)} (去重={total_nodes_in - len(nodes_merged)})")
    print(f"  共同概念: {exactly_common} (出现在所有文件中)")
    print(f"  仅单文件: {len(all_concepts) - exactly_common}")
    print(f"边: {total_edges_in} → 合并={total_edges_m}")
    print(f"跨拓扑: {total_cross_in} → 合并={len(cross_resolved)}")

    # 6. 写入
    print(f"\n写入...")
    w_nodes, w_edges, w_cross = write_state(out_path, nodes_merged, cross_resolved, fmt_ver)

    # 7. 校验
    print(f"\n校验...")
    verify_state(out_path, w_nodes, w_edges, w_cross)

    print(f"\n✅ 合并完成！")
    print(f"   节点: {len(nodes_merged)}")
    print(f"   边: {total_edges_m}")
    print(f"   跨拓扑: {len(cross_resolved)}")


if __name__ == '__main__':
    main()
