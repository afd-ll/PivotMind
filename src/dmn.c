/**
 * @file dmn.c
 * @brief 默认模式网络实现 — 梦境引擎 + 扩散引导
 */

#include "dmn.h"
#include "dream_engine.h"
#include "diffusion.h"
#include <string.h>

int dmn_cycle(MasterTopology* master, MemorySystem* memory,
              const DmnConfig* config, float throttle) {
    if (throttle <= 0.01f) return 0;
    if (!master) return 0;

    /* throttle 调制梦境强度 */
    DmnConfig modulated = *config;
    modulated.sample_vocab    = (int)(config->sample_vocab * throttle + 0.5f);
    modulated.sample_semantic = (int)(config->sample_semantic * throttle + 0.5f);
    modulated.sample_emotion  = (int)(config->sample_emotion * throttle + 0.5f);
    modulated.walk_hops       = (int)(config->walk_hops * throttle + 0.5f);
    if (modulated.sample_vocab < 1) modulated.sample_vocab = 1;
    if (modulated.walk_hops < 1) modulated.walk_hops = 1;

    int result = dream_cycle(master, memory, &modulated);

    /* 扩散增强：梦境采样到的概念做扩散激活，强化语义邻接 */
    if (result > 0) {
        DiffusionCtx dctx;
        if (diffusion_init(&dctx, master) == 0) {
            /* 取一个随机采样节点作为输入，扩散1步 */
            int ri = rand() % master->sub_topologies[0]->net->node_count;
            ReasoningNode* rn = master->sub_topologies[0]->net->nodes[ri];
            if (rn && rn->concept) {
                const char* words[DIFF_MAX_SEQUENCE];
                dctx.depth = 1;  /* 只扩散1步，避免太重 */
                dctx.top_k = 8;
                int n = diffusion_generate(&dctx, rn->concept, words, DIFF_MAX_SEQUENCE);
                (void)n;  /* 扩散过程本身会强化边，结果用于激活 */
            }
        }
    }

    return result;
}
