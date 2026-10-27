/* tools/emerge_probe.c — CX-34 验证：未分类池接上后，能否真的长出词类
 *
 * 做法：手动把两批「应当各自成类」的非种子词塞进池，调 emergent_pos_try_emerge，
 * 看它长出的类像不像话。池本身不落盘，所以离线塞词与线上等价。
 *
 * 用法: ./emerge_probe <state_file>
 */
#include "pivotmind_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "emergent_pos.h"
#include "word_embed.h"
#include "common.h"

/* try_emerge 非 static，但未在 header 导出（设计上只供 classify_soft 内部触发） */
extern int emergent_pos_try_emerge(EmergentPOS* ep);

static MasterTopology* load_master(const char* path) {
    MasterTopology* master = master_topology_create(16);
    if (!master) return NULL;
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 60000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC,   "语义拓扑", 20000, 9);
    master_add_sub_topology(master, TOPO_EMOTION,    "情绪拓扑", 8000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX,     "语法拓扑", 4000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT,    "上下文拓扑", 4000, 6);
    master_add_sub_topology(master, TOPO_DOMAIN,     "领域拓扑", 4000, 5);
    master_add_sub_topology(master, TOPO_PRAGMA,     "语用拓扑", 4000, 4);
    master_add_sub_topology(master, TOPO_CULTURE,    "文化拓扑", 4000, 3);
    master_add_sub_topology(master, TOPO_CONCEPT,    "概念拓扑", 20000, 9);
    master_add_sub_topology(master, TOPO_MASTER,     "主拓扑", 100, 0);
    master_add_sub_topology(master, TOPO_TEMPLATE,   "模板拓扑", 8000, 8);
    master_add_sub_topology(master, TOPO_VISUAL,     "视觉拓扑", 8000, 7);
    if (master_load_state(master, path) < 0) {
        master_topology_destroy(master);
        return NULL;
    }
    return master;
}

static int push(EmergentPOS* ep, const char* w) {
    if (!w || strlen(w) >= 4) return 0;
    if (ep->unclassified_count >= 64) return 0;
    int slot = ep->unclassified_count;
    snprintf(ep->unclassified_words[slot], 4, "%s", w);
    ep->unclassified_count++;
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s <state_file>\n", argv[0]); return 2; }
    MasterTopology* master = load_master(argv[1]);
    if (!master) { fprintf(stderr, "加载失败\n"); return 1; }
    if (!wemb_ensure()) { fprintf(stderr, "指纹不可用\n"); master_topology_destroy(master); return 1; }

    EmergentPOS* ep = emergent_pos_create("zh");
    if (!ep) { master_topology_destroy(master); return 1; }
    printf("[锚点] init_centroids = %d, anchor_count = %d\n",
           emergent_pos_init_centroids(ep, master), emergent_pos_anchor_count(ep));

    /* 两批应当各自成类的非种子词 */
    const char* verbs[] = {"走", "拿", "睡", "写", "唱", "跳", NULL};
    const char* advs[]  = {"更", "太", "还", "又", "却", "再", NULL};
    const char* nouns[] = {"树", "河", "车", "鱼", "门", "路", NULL};
    const char* adjs[]  = {"慢", "甜", "苦", "冷", "甜", "硬", NULL};
    int c = 0;
    for (int i = 0; verbs[i]; i++) c += push(ep, verbs[i]);
    for (int i = 0; advs[i]; i++)  c += push(ep, advs[i]);
    for (int i = 0; nouns[i]; i++) c += push(ep, nouns[i]);
    for (int i = 0; adjs[i] && i < 5; i++) c += push(ep, adjs[i]);
    printf("[池] 塞入 %d 个词（动词组 6 + 副词组 6）\n", c);

    /* 先量 pairwise：不然调阈值就是瞎猜（今天已经吃过「猜参数」的亏） */
    {
        float vin = 0, vout = 0, mx = 0;
        int cin = 0, cout_ = 0;
        for (int i = 0; i < 6; i++)
            for (int j = i + 1; j < 6; j++) {
                float s = wemb_similar(verbs[i], verbs[j]);
                if (s >= 0) { vin += s; cin++; if (s > mx) mx = s; }
            }
        for (int i = 0; i < 6; i++)
            for (int j = i + 1; j < 6; j++) {
                float s = wemb_similar(advs[i], advs[j]);
                if (s >= 0) { vin += s; cin++; if (s > mx) mx = s; }
            }
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < 6; j++) {
                float s = wemb_similar(verbs[i], advs[j]);
                if (s >= 0) { vout += s; cout_++; if (s > mx) mx = s; }
            }
        printf("[pairwise] 组内均值=%.3f（%d 对）· 组间均值=%.3f（%d 对）· 全局最大=%.3f\n",
               cin ? vin / cin : 0.0, cin, cout_ ? vout / cout_ : 0.0, cout_, (double)mx);
    }

    int created = emergent_pos_try_emerge(ep);
    printf("[涌现] try_emerge 返回 %d，extra_class_count = %d\n", created, ep->extra_class_count);
    for (int i = 0; i < ep->extra_class_count; i++) {
        printf("  新类 class_id=%d size=%d coherence=%.3f 成员标签=%s\n",
               ep->extra_classes[i].class_id, ep->extra_classes[i].member_count,
               (double)ep->extra_classes[i].coherence, ep->extra_classes[i].label_hint);
    }
    if (ep->extra_class_count == 0)
        printf("  ⇒ 一个类都没长出来（看上面的「放弃」日志，是紧密度不够还是别的原因）\n");
    printf("[池剩余] %d\n", ep->unclassified_count);

    emergent_pos_destroy(ep);
    master_topology_destroy(master);
    return 0;
}
