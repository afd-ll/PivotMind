/**
 * @file dmn.c
 * @brief 默认模式网络实现 — 梦境引擎 + 丘脑 throttle
 */

#include "dmn.h"
#include "dream_engine.h"

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

    return dream_cycle(master, memory, &modulated);
}
