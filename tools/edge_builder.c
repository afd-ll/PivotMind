/* edge_builder.c — 字符共现建边（哈希加速版）
 * 从文本文件中提取相邻匹配词对，在词汇拓扑间建边
 * 用法: ./build/bin/edge_builder pivotmind_state.dat textfile1.txt [textfile2.txt ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "feature_io.h"

#define HASH_SIZE 32768

/* 简易哈希表: concept → node_id */
typedef struct hash_entry { char* key; int val; struct hash_entry* next; } hash_entry;
static hash_entry* g_hash[HASH_SIZE];

static unsigned hash_str(const char* s) {
    unsigned h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h & (HASH_SIZE - 1);
}

static void hash_put(const char* key, int val) {
    unsigned idx = hash_str(key);
    hash_entry* e = malloc(sizeof(*e));
    e->key = strdup(key);
    e->val = val;
    e->next = g_hash[idx];
    g_hash[idx] = e;
}

static int hash_get(const char* key) {
    for (hash_entry* e = g_hash[hash_str(key)]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e->val;
    return -1;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <state.dat> <textfiles...>\n", argv[0]);
        return 1;
    }

    MasterTopology* topo = master_topology_create(16);
    master_add_sub_topology(topo, TOPO_VOCABULARY, "词汇拓扑", 100000, 10);
    for (int i = 1; i <= 10; i++)
        master_add_sub_topology(topo, i, "sub", 100, 0);

    if (!master_load_state(topo, argv[1])) {
        fprintf(stderr, "Failed to load state\n"); return 1;
    }
    SubTopology* vocab = master_get_sub_topology_by_type(topo, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) { fprintf(stderr, "No vocab topology\n"); return 1; }

    int nc = vocab->net->node_count;
    printf("Loaded %d nodes. Building hash...\n", nc);

    /* 构建哈希表: concept → node_id */
    int max_len = 0;
    for (int i = 0; i < nc; i++) {
        if (!vocab->net->nodes[i] || !vocab->net->nodes[i]->concept) continue;
        const char* c = vocab->net->nodes[i]->concept;
        hash_put(c, vocab->net->nodes[i]->node_id);
        int l = (int)strlen(c);
        if (l > max_len) max_len = l;
    }
    printf("Hash built, max concept length=%d\n", max_len);

    /* 逐文件处理 */
    long edge_count = 0;
    for (int fi = 2; fi < argc; fi++) {
        FILE* f = fopen(argv[fi], "rb");
        if (!f) { fprintf(stderr, "skip: %s\n", argv[fi]); continue; }

        char* text = NULL;
        long tsz = 0;
        fseek(f, 0, SEEK_END);
        tsz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (tsz > 0 && tsz < 10 * 1024 * 1024) {
            text = malloc(tsz + 1);
            if (text) {
                size_t got = fread(text, 1, (size_t)tsz, f);
                text[got] = '\0';
            }
        }
        fclose(f);
        if (!text) continue;

        /* 贪婪匹配: 从每个位置找最长匹配 */
        int prev_id = -1;
        char* p = text;
        while (*p) {
            int best_id = -1, best_len = 0;
            for (int l = max_len; l >= 1; l--) {
                if (p + l > text + tsz) continue;
                char save = p[l];
                p[l] = '\0';
                int id = hash_get(p);
                p[l] = save;
                if (id >= 0) { best_id = id; best_len = l; break; }
            }

            if (best_id >= 0) {
                if (prev_id >= 0 && best_id != prev_id) {
                    huarong_net_add_connection(vocab->net, prev_id, best_id, 0.3f);
                    edge_count++;
                }
                prev_id = best_id;
                p += best_len;
            } else {
                prev_id = -1;  /* 重置，跳过未知字符 */
                p++;
            }
        }
        free(text);
        printf("  %s: %ld edges so far\n", argv[fi], edge_count);
    }

    master_save_state(topo, argv[1]);
    printf("Saved: %ld total edges\n", edge_count);

    /* 清理哈希表 */
    for (int i = 0; i < HASH_SIZE; i++) {
        hash_entry* e = g_hash[i];
        while (e) { hash_entry* n = e->next; free(e->key); free(e); e = n; }
    }
    master_topology_destroy(topo);
    return 0;
}
