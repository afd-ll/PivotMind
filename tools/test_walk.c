/**
 * 璧拌竟璺緞鐢熸垚娴嬭瘯
 * 娴嬭瘯 topology_walk_greedy 鐨勮矾寰勮繛璐€с€佺粓鐐瑰垽鏂€佽法璧风偣閲嶈瘯
 */
#include "multi_topology.h"
#include "node_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// 杈呭姪锛氬悜瀛愭嫇鎵戞坊鍔犺妭鐐癸紝鑷姩娣诲姞鑺傜偣鍝堝笇
static ReasoningNode* add_node(SubTopology* sub, const char* concept, float activation, float valence) {
    HuarongTopologyNet* net = sub->net;
    ReasoningNode* node = huarong_net_add_node(net, concept, NULL, 0);
    if (!node) return NULL;
    node->activation = activation;
    node->confidence = 0.5f;
    node->valence = valence;
    node_hash_add(sub->node_hash, node);
    return node;
}

// 杈呭姪锛氬湪涓や釜鑺傜偣涔嬮棿娣诲姞鍙屽悜杈?static void add_edge(SubTopology* sub, ReasoningNode* from, ReasoningNode* to,
                     float weight, float conf, float bias) {
    if (!from || !to) return;
    HuarongTopologyNet* net = sub->net;

    // 纭繚 connection 鏁扮粍鏈夌┖闂达紙绠€鍖栫増锛氱洿鎺ユ浛鎹㈠凡鏈夎繛鎺ワ級
    int idx = from->connection_count;
    int cap = from->connection_capacity;

    if (idx >= cap) {
        int new_cap = cap == 0 ? 8 : cap * 2;
        ReasoningNode** conn = realloc(from->connections, sizeof(ReasoningNode*) * new_cap);
        float* w = realloc(from->connection_weights, sizeof(float) * new_cap);
        float* cf = realloc(from->connection_confidences, sizeof(float) * new_cap);
        float* mb = realloc(from->connection_motivational_bias, sizeof(float) * new_cap);
        if (!conn || !w || !cf || !mb) {
            free(conn); free(w); free(cf); free(mb);
            return;
        }
        from->connections = conn;
        from->connection_weights = w;
        from->connection_confidences = cf;
        from->connection_motivational_bias = mb;
        from->connection_capacity = new_cap;
    }

    from->connections[idx] = to;
    from->connection_weights[idx] = weight;
    from->connection_confidences[idx] = conf;
    from->connection_motivational_bias[idx] = bias;
    from->connection_count++;
}

static void print_path(SubTopology* sub, int* path, int len) {
    printf("  璺緞(%d): ", len);
    for (int i = 0; i < len; i++) {
        int nid = path[i];
        if (nid >= 0 && nid < sub->net->node_count && sub->net->nodes[nid]) {
            printf("%s", sub->net->nodes[nid]->concept);
            if (i < len - 1) printf("鈫?);
        } else {
            printf("[?]");
        }
    }
    printf("\n");
}

// ===== 娴嬭瘯1: 璺緞杩炶疮鎬?=====
// 鏋勫缓 浜衡啋宸モ啋鏅衡啋鑳解啋鏄啋浠€鈫掍箞 鐨勯摼锛岀湅鏄惁鑳藉畬鏁磋蛋鍑?static void test_coherence() {
    printf("\n========== 娴嬭瘯1: 璺緞杩炶疮鎬?==========\n");

    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "璇嶆眹鎷撴墤", 100, 10);
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);

    // 鏋勫缓: 浜衡啋宸モ啋鏅衡啋鑳解啋鏄啋浠€鈫掍箞 (楂樻潈閲嶉摼)
    ReasoningNode* ren   = add_node(vocab, "浜?, 0.95f, 0.5f);
    ReasoningNode* gong  = add_node(vocab, "宸?, 0.90f, 0.4f);
    ReasoningNode* zhi   = add_node(vocab, "鏅?, 0.85f, 0.6f);
    ReasoningNode* neng  = add_node(vocab, "鑳?, 0.80f, 0.5f);
    ReasoningNode* shi   = add_node(vocab, "鏄?, 0.75f, 0.3f);
    ReasoningNode* shen  = add_node(vocab, "浠€", 0.70f, 0.2f);
    ReasoningNode* me    = add_node(vocab, "涔?, 0.65f, 0.1f);

    // 寮鸿繛鎺ラ摼锛氫富璺緞
    add_edge(vocab, ren, gong, 0.9f, 0.95f, 0.5f);
    add_edge(vocab, gong, zhi, 0.85f, 0.90f, 0.5f);
    add_edge(vocab, zhi, neng, 0.80f, 0.88f, 0.5f);
    add_edge(vocab, neng, shi, 0.75f, 0.85f, 0.4f);
    add_edge(vocab, shi, shen, 0.70f, 0.80f, 0.3f);
    add_edge(vocab, shen, me, 0.65f, 0.75f, 0.3f);

    // 娣诲姞骞叉壈杈癸細浜衡啋A(涓嶇浉鍏充絾婵€娲婚珮)
    ReasoningNode* otherA = add_node(vocab, "A", 0.92f, -0.1f);
    ReasoningNode* otherB = add_node(vocab, "B", 0.88f, -0.2f);
    add_edge(vocab, ren, otherA, 0.3f, 0.4f, 0.1f);  // 寮辫竟浣嗙洰鏍囨縺娲婚珮
    add_edge(vocab, otherA, otherB, 0.2f, 0.3f, 0.1f);

    // 鍙嶅悜杈癸紙浣庢潈閲嶏級
    add_edge(vocab, gong, ren, 0.1f, 0.2f, 0.0f);

    printf("浠庛€屼汉銆嶅嚭鍙戣蛋杈?\n");
    int path[20];
    float scores[20];
    int len = topology_walk_greedy(vocab, ren->node_id, path, scores, 10, NULL, 1.0f, NULL, NULL);
    print_path(vocab, path, len);
    printf("  棰勬湡: 浜衡啋宸モ啋鏅衡啋鑳解啋鏄啋浠€鈫掍箞\n");
    printf("  涓嶅寘鍚? A銆丅锛堝共鎵拌竟寰楀垎浣庯紝搴旇琚繃婊ゆ帀锛塡n");

    // 浠庝腑闂磋妭鐐瑰嚭鍙?    printf("\n浠庛€屾櫤銆嶅嚭鍙戣蛋杈?\n");
    len = topology_walk_greedy(vocab, zhi->node_id, path, scores, 10, NULL, 1.0f, NULL, NULL);
    print_path(vocab, path, len);
    printf("  棰勬湡: 鏅衡啋鑳解啋鏄啋浠€鈫掍箞\n");

    master_topology_destroy(master);
}

// ===== 娴嬭瘯2: 缁堢偣鍒ゆ柇 =====
// 鏋勫缓涓嶅悓闀垮害鐨勯摼锛岀湅闃堝€?.05鏄惁鍚堢悊
static void test_termination() {
    printf("\n========== 娴嬭瘯2: 缁堢偣鍒ゆ柇 ==========\n");

    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "璇嶆眹鎷撴墤", 100, 10);
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);

    // 鏋勫缓 瀛︹啋涔犫啋鏂扳啋鐭モ啋璇?(渚濇閫掑噺鐨勮竟鏉冮噸锛屾ā鎷熸湯绔嚜鐒剁粨鏉?
    ReasoningNode* xue  = add_node(vocab, "瀛?, 0.90f, 0.6f);
    ReasoningNode* xi   = add_node(vocab, "涔?, 0.85f, 0.5f);
    ReasoningNode* xin  = add_node(vocab, "鏂?, 0.60f, 0.4f);
    ReasoningNode* zhi  = add_node(vocab, "鐭?, 0.40f, 0.3f);
    ReasoningNode* shi2 = add_node(vocab, "璇?, 0.30f, 0.2f);

    add_edge(vocab, xue, xi, 0.90f, 0.92f, 0.5f);   // 寮?    add_edge(vocab, xi, xin, 0.70f, 0.75f, 0.4f);    // 涓?    add_edge(vocab, xin, zhi, 0.30f, 0.35f, 0.2f);   // 寮?    add_edge(vocab, zhi, shi2, 0.08f, 0.10f, 0.1f);  // 鏋佸急锛屾帴杩戦槇鍊?
    printf("浠庛€屽銆嶅嚭鍙戣蛋杈癸紙娓愯繘寮卞寲閾撅級:\n");
    int path[20];
    float scores[20];
    int len = topology_walk_greedy(vocab, xue->node_id, path, scores, 10, NULL, 1.0f, NULL, NULL);
    print_path(vocab, path, len);
    printf("  姣忔寰楀垎: ");
    for (int i = 0; i < len; i++) printf("%.4f ", scores[i]);
    printf("\n");
    printf("  棰勬湡: 瀛︹啋涔犫啋鏂扳啋鐭ワ紙銆岃瘑銆嶇殑杩炴帴鏉冮噸+婵€娲诲€煎彲鑳戒綆浜?.05闃堝€艰鎴柇锛塡n");

    // 鏃犺矾鍙蛋鐨勫彾瀛愯妭鐐?    ReasoningNode* leaf = add_node(vocab, "鍙?, 0.50f, 0.0f);
    printf("\n浠庛€屽彾銆嶅嚭鍙戯紙鏃犺繛鎺ワ級锛歕n");
    len = topology_walk_greedy(vocab, leaf->node_id, path, scores, 10, NULL, 1.0f, NULL, NULL);
    print_path(vocab, path, len);
    printf("  棰勬湡: 鍙讹紙鍙湁璧风偣锛屾棤璺緞锛塡n");

    master_topology_destroy(master);
}

// ===== 娴嬭瘯3: 璺ㄨ捣鐐归噸璇?=====
// 绗竴璧风偣璧颁笉闀匡紝鎹㈢浜岃捣鐐?static void test_multi_start() {
    printf("\n========== 娴嬭瘯3: 璺ㄨ捣鐐归噸璇?==========\n");

    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "璇嶆眹鎷撴墤", 100, 10);
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);

    // 瀛ょ珛璧风偣A锛堝彧鏈?姝ヨ繛鎺ワ級
    ReasoningNode* a1 = add_node(vocab, "A", 0.95f, 0.3f);
    ReasoningNode* a2 = add_node(vocab, "A2", 0.30f, 0.2f);
    add_edge(vocab, a1, a2, 0.5f, 0.5f, 0.3f);
    // A->A2鍚庡氨娌¤矾浜嗭紙A2鏃犲嚭杈癸級

    // 闀块摼璧风偣B锛堝彲浠ヨ蛋寰堣繙锛?    ReasoningNode* b1 = add_node(vocab, "B", 0.90f, 0.5f);
    ReasoningNode* b2 = add_node(vocab, "B2", 0.85f, 0.4f);
    ReasoningNode* b3 = add_node(vocab, "B3", 0.80f, 0.4f);
    ReasoningNode* b4 = add_node(vocab, "B4", 0.75f, 0.3f);
    add_edge(vocab, b1, b2, 0.9f, 0.9f, 0.5f);
    add_edge(vocab, b2, b3, 0.8f, 0.8f, 0.5f);
    add_edge(vocab, b3, b4, 0.7f, 0.7f, 0.4f);

    printf("妯℃嫙: 鎺掑簭鍚庣殑璧风偣鍒楄〃 A(act=0.95) > B(act=0.90)\n");
    printf("浠嶢鍑哄彂: 鍙兘璧?姝?A鈫扐2)\n");
    printf("璺ㄨ捣鐐归噸璇? 鎹鍑哄彂 鈫?鍙蛋B鈫払2鈫払3鈫払4\n\n");

    // 鍏堣蛋A
    int path[20];
    float scores[20];
    int len = topology_walk_greedy(vocab, a1->node_id, path, scores, 10, NULL, 1.0f, NULL, NULL);
    printf("璧风偣A:\n");
    print_path(vocab, path, len);

    // 璧癇锛堢敤鍚屼竴涓獀isited bitmap涔熻锛屼絾涓嶅叡浜級
    len = topology_walk_greedy(vocab, b1->node_id, path, scores, 10, NULL, 1.0f, NULL, NULL);
    printf("璧风偣B:\n");
    print_path(vocab, path, len);

    master_topology_destroy(master);
}

// ===== 娴嬭瘯4: 涓夎鐜槻寰幆 =====
// 浜衡啍宸モ啍鏅衡啍浜猴紙寰幆锛夛紝涓嶈兘姝诲惊鐜?static void test_loop_prevention() {
    printf("\n========== 娴嬭瘯4: 闃插惊鐜?==========\n");

    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "璇嶆眹鎷撴墤", 100, 10);
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);

    ReasoningNode* a = add_node(vocab, "A", 0.90f, 0.5f);
    ReasoningNode* b = add_node(vocab, "B", 0.85f, 0.4f);
    ReasoningNode* c = add_node(vocab, "C", 0.80f, 0.3f);
    ReasoningNode* d = add_node(vocab, "D", 0.75f, 0.3f);

    // 涓夎鐜? A鈫払鈫扖鈫扐
    add_edge(vocab, a, b, 0.9f, 0.9f, 0.5f);
    add_edge(vocab, b, c, 0.8f, 0.8f, 0.5f);
    add_edge(vocab, c, a, 0.7f, 0.7f, 0.5f);
    // 鍒嗘敮: C鈫扗锛堝敮涓€鍑鸿矾锛?    add_edge(vocab, c, d, 0.6f, 0.6f, 0.4f);

    printf("涓夎鐜?A鈫払鈫扖鈫扐 + 鍒嗘敮 C鈫扗\n");
    printf("涓嶈兘姝诲惊鐜湪A鈫払鈫扖鈫扐锛屽繀椤昏蛋鍒癉\n\n");

    int path[20];
    float scores[20];
    int len = topology_walk_greedy(vocab, a->node_id, path, scores, 10, NULL, 1.0f, NULL, NULL);
    print_path(vocab, path, len);
    printf("  棰勬湡: A鈫払鈫扖鈫扗锛堜笉浼氬洖鍒癆锛塡n");

    master_topology_destroy(master);
}

int main() {
    printf("========================================\n");
    printf("  璧拌竟璺緞鐢熸垚娴嬭瘯\n");
    printf("  娣峰悎璇勫垎: 鍔犳硶(杈规潈閲?杈圭疆淇?杈瑰姩鏈?鐩爣婵€娲?鐩爣缃俊) 脳 鏁堜环涔樻硶鍥犲瓙\n");
    printf("  闃堝€? < 0.05 鍋滄\n");
    printf("========================================\n");

    test_coherence();
    test_termination();
    test_multi_start();
    test_loop_prevention();

    printf("\n========================================\n");
    printf("  娴嬭瘯瀹屾垚\n");
    printf("========================================\n");
    return 0;
}
