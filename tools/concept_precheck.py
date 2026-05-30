#!/usr/bin/env python3
"""
概念预检脚本 — 分析拓扑中多字概念的共激活簇

检查指标：
1. 拓扑总边数和平均置信度
2. 指定多字概念的组成字之间是否有直接连接
3. 连接置信度分布
4. 判断是否已形成稳定共激活簇

用法：
  python3 tools/concept_precheck.py [pivotmind_state.dat]
"""

import sys
import struct
import os

STATE_FORMAT_VERSION = 2

MULTI_CHAR_CONCEPTS = [
    "因果推理", "关联推理", "主动学习",
    "概念抽象", "拓扑网络", "联想传播",
    "置信度", "激活值", "词汇拓扑",
    "语义拓扑", "上下文", "记忆系统",
    "学习", "机器", "推理", "认知",
]

def read_topology_state(path):
    """读取 pivotmind_state.dat (master_save_state 格式)"""
    with open(path, 'rb') as f:
        data = f.read()
    
    pos = 0
    fmt_ver = struct.unpack_from('i', data, pos)[0]; pos += 4
    if fmt_ver != STATE_FORMAT_VERSION:
        print(f"版本不匹配: {fmt_ver} != {STATE_FORMAT_VERSION}")
        return None
    
    # 读取所有节点
    # 文件格式：直接遍历所有拓扑节点，没有拓扑分隔符
    nodes = []          # 所有节点平铺
    connections = []    # 所有连接
    concept_to_nodes = {}  # 概念名 → 节点列表
    concept_set = set()
    topo_type_stats = {}   # 各拓扑类型的节点数和边数
    
    while pos + 16 < len(data):
        topo_type = struct.unpack_from('i', data, pos)[0]; pos += 4
        node_id = struct.unpack_from('i', data, pos)[0]; pos += 4
        concept_len = struct.unpack_from('i', data, pos)[0]; pos += 4
        
        if concept_len <= 0 or concept_len > 256 or pos + concept_len > len(data):
            break  # 没有更多节点了
        
        concept = data[pos:pos+concept_len].decode('utf-8', errors='replace').rstrip('\x00')
        pos += concept_len
        
        activation = struct.unpack_from('f', data, pos)[0]; pos += 4
        
        conn_count = struct.unpack_from('i', data, pos)[0]; pos += 4
        
        node_conns = []
        for c in range(conn_count):
            if pos + 16 > len(data):
                break
            target_id = struct.unpack_from('i', data, pos)[0]; pos += 4
            weight = struct.unpack_from('f', data, pos)[0]; pos += 4
            m_bias = struct.unpack_from('f', data, pos)[0]; pos += 4
            conf = struct.unpack_from('f', data, pos)[0]; pos += 4
            node_conns.append({
                'target_id': target_id,
                'weight': weight,
                'motivational_bias': m_bias,
                'confidence': conf
            })
        
        node = {
            'topo_type': topo_type,
            'node_id': node_id,
            'concept': concept,
            'activation': activation,
            'connections': node_conns
        }
        nodes.append(node)
        
        if concept:
            concept_set.add(concept)
            if concept not in concept_to_nodes:
                concept_to_nodes[concept] = []
            concept_to_nodes[concept].append(node)
        
        # 统计
        if topo_type not in topo_type_stats:
            topo_type_stats[topo_type] = {'name': '', 'nodes': 0, 'edges': 0}
        topo_type_stats[topo_type]['nodes'] += 1
        topo_type_stats[topo_type]['edges'] += conn_count
    
    # 读取跨拓扑连接（文件末尾剩下的数据）
    cross_links = []
    while pos + 24 <= len(data):
        from_topo = struct.unpack_from('i', data, pos)[0]; pos += 4
        from_node = struct.unpack_from('i', data, pos)[0]; pos += 4
        to_topo = struct.unpack_from('i', data, pos)[0]; pos += 4
        to_node = struct.unpack_from('i', data, pos)[0]; pos += 4
        weight = struct.unpack_from('f', data, pos)[0]; pos += 4
        use_count = struct.unpack_from('i', data, pos)[0]; pos += 4
        cross_links.append({
            'from_topo': from_topo, 'from_node': from_node,
            'to_topo': to_topo, 'to_node': to_node,
            'weight': weight, 'use_count': use_count
        })
    
    # 构建节点ID查找表（用于连接目标名称解析）
    id_to_node = {}
    for n in nodes:
        key = (n['topo_type'], n['node_id'])
        id_to_node[key] = n
    
    return {
        'nodes': nodes,
        'concept_set': concept_set,
        'concept_to_nodes': concept_to_nodes,
        'id_to_node': id_to_node,
        'cross_links': cross_links,
        'topo_type_stats': topo_type_stats
    }


def check_concept_clusters(state, concepts):
    """检查多字概念在拓扑中的连接情况"""
    concept_to_nodes = state['concept_to_nodes']
    id_to_node = state['id_to_node']
    concept_set = state['concept_set']
    
    for concept in concepts:
        chars = list(concept)
        
        existing = [c for c in chars if c in concept_set]
        missing = [c for c in chars if c not in concept_set]
        
        if len(existing) < 2:
            continue
        
        total_pairs = 0
        connected_pairs = 0
        confs = []
        
        print(f"\n  【{concept}】({len(existing)}/{len(chars)} 字存在)")
        if missing:
            print(f"    缺失: {''.join(missing)}")
        
        for i in range(len(existing)):
            for j in range(i+1, len(existing)):
                ch_a = existing[i]
                ch_b = existing[j]
                
                # 找 ch_a 节点 -> 检查连接中是否有 ch_b
                found = False
                best_conf = 0
                for node_a in concept_to_nodes.get(ch_a, []):
                    for conn in node_a['connections']:
                        target_key = (node_a['topo_type'], conn['target_id'])
                        target_node = id_to_node.get(target_key)
                        if target_node and target_node['concept'] == ch_b:
                            found = True
                            best_conf = max(best_conf, conn['confidence'])
                
                total_pairs += 1
                if found:
                    connected_pairs += 1
                    confs.append(best_conf)
                    print(f"    {ch_a} ↔ {ch_b}: ✓ (conf={best_conf:.3f})")
                else:
                    print(f"    {ch_a} ↔ {ch_b}: ✗ 无直接连接")
        
        if total_pairs > 0:
            avg_conf = sum(confs)/len(confs) if confs else 0
            ratio = connected_pairs / total_pairs
            if ratio >= 0.8 and avg_conf > 0.3:
                status = "✅ 稳定簇"
            elif ratio >= 0.4:
                status = "⚠️ 部分连接"
            else:
                status = "❌ 未形成簇"
            print(f"  → 连接率: {connected_pairs}/{total_pairs} ({ratio*100:.0f}%), "
                  f"平均置信度: {avg_conf:.3f} [{status}]")


def check_activation_distribution(state, top_n=30):
    """检查激活值最高的节点"""
    nodes_with_act = [(n['concept'], n['activation']) for n in state['nodes'] if n['concept']]
    nodes_with_act.sort(key=lambda x: x[1], reverse=True)
    
    print(f"\n  激活值 Top {top_n}:")
    for concept, act in nodes_with_act[:top_n]:
        if not concept.strip():
            continue
        bar = '█' * max(1, int(act * 20))
        print(f"    {concept:>8s} {act:.3f} {bar}")


def check_confidence_distribution(state):
    """检查置信度分布"""
    confs = []
    for node in state['nodes']:
        for conn in node['connections']:
            confs.append(conn['confidence'])
    
    if not confs:
        print("\n  置信度分布: 无边")
        return
    
    avg_conf = sum(confs) / len(confs)
    buckets = [0]*5
    for c in confs:
        idx = min(int(c * 5), 4)
        buckets[idx] += 1
    
    print(f"\n  置信度分布 (总 {len(confs)} 条边, 平均 {avg_conf:.3f}):")
    labels = ["0.0-0.2", "0.2-0.4", "0.4-0.6", "0.6-0.8", "0.8-1.0"]
    for i, label in enumerate(labels):
        pct = buckets[i]/len(confs)*100
        bar = '█' * int(pct / 2)
        print(f"    {label}: {buckets[i]:>5d} ({pct:5.1f}%) {bar}")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "pivotmind_state.dat"
    if not os.path.exists(path):
        print(f"文件不存在: {path}")
        sys.exit(1)
    
    print(f"读取拓扑状态: {path}")
    state = read_topology_state(path)
    if not state:
        print("读取失败")
        sys.exit(1)
    
    # 总览
    nodes = state['nodes']
    topo_types = {
        0: "TOPO_VOCABULARY", 1: "TOPO_SEMANTIC",
        2: "TOPO_EMOTION", 3: "TOPO_SYNTAX",
        4: "TOPO_CONTEXT", 5: "TOPO_DOMAIN",
        6: "TOPO_PRAGMA", 7: "TOPO_CULTURE",
        8: "TOPO_CONCEPT"
    }
    
    total_edges = sum(len(n['connections']) for n in nodes)
    print(f"\n总节点数: {len(nodes)}")
    print(f"总边数: {total_edges}")
    print(f"总概念数: {len(state['concept_set'])}")
    print(f"跨拓扑连接: {len(state['cross_links'])}")
    
    print(f"\n拓扑分布:")
    for n in nodes:
        t = n['topo_type']
        name = topo_types.get(t, f"UNKNOWN_{t}")
        print(f"  类型 {name}: 节点 {n['concept']} (act={n['activation']:.3f}, edges={len(n['connections'])})")
        break  # 只展示第一个
    
    # 真正统计
    type_counts = {}
    for n in nodes:
        t = n['topo_type']
        if t not in type_counts:
            type_counts[t] = {'node_count': 0, 'edge_count': 0}
        type_counts[t]['node_count'] += 1
        type_counts[t]['edge_count'] += len(n['connections'])
    
    for t, stats in sorted(type_counts.items()):
        name = topo_types.get(t, f"类型{t}")
        print(f"  {name}: {stats['node_count']} 节点, {stats['edge_count']} 边")
    
    # 概念簇检测
    print(f"\n{'='*60}")
    print("多字概念簇检测")
    print(f"{'='*60}")
    check_concept_clusters(state, MULTI_CHAR_CONCEPTS)
    
    # 激活值分布
    print(f"\n{'='*60}")
    print("激活值分布")
    print(f"{'='*60}")
    check_activation_distribution(state, 30)
    
    # 置信度分布
    print(f"\n{'='*60}")
    print("置信度分布")
    print(f"{'='*60}")
    check_confidence_distribution(state)
    
    # 最终评估
    print(f"\n{'='*60}")
    print("最终评估")
    print(f"{'='*60}")
    
    causal_chars = list("因果推理")
    causal_set = set(causal_chars)
    concept_set = {n['concept'] for n in nodes if n['concept']}
    existing_causal = [c for c in causal_chars if c in concept_set]
    
    if len(existing_causal) >= 3:
        pairs_connected = 0
        pairs_total = 0
        for i in range(len(existing_causal)):
            for j in range(i+1, len(existing_causal)):
                ch_a, ch_b = existing_causal[i], existing_causal[j]
                pairs_total += 1
                for node_a in state['concept_to_nodes'].get(ch_a, []):
                    for conn in node_a['connections']:
                        target_key = (node_a['topo_type'], conn['target_id'])
                        target_node = state['id_to_node'].get(target_key)
                        if target_node and target_node['concept'] == ch_b:
                            pairs_connected += 1
                            break
        
        ratio = pairs_connected / pairs_total if pairs_total > 0 else 0
        if ratio >= 0.8:
            print("  ✅ 多字概念已形成稳定共激活簇")
            print("  → 概念抽象层 (v0.3) 不建议立即进场，先让自主学习器再跑几轮")
        elif ratio >= 0.4:
            print("  ⚠️ 部分概念已产生连接，但不够稳定")
            print("  → 建议先跑几轮自主学习器，等置信度稳定后再评估")
        else:
            print("  ❌ 多字概念尚未形成有效连接")
            print("  → 自主学习器创建新边后，概念抽象层 (v0.3) 可能有价值")
    else:
        print(f"  ❌ 概念字不全 (仅 {len(existing_causal)}/{len(causal_chars)})")


if __name__ == '__main__':
    main()
