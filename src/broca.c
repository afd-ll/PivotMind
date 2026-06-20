#include "broca.h"
#include "template_builder.h"
#include <stdio.h>
#include <stdlib.h>

Broca* broca_create(MasterTopology* master) {
    if (!master) return NULL;
    Broca* b = (Broca*)calloc(1, sizeof(Broca));
    if (!b) return NULL;
    b->master              = master;
    b->build_interval_ticks = 300;  /* 默认每300 tick构建一次模板 */
    b->max_build_depth     = 3;
    b->decay_threshold     = 5;     /* 活跃度低于此值衰减 */
    b->decay_rate          = 0.85f;
    printf("[布罗卡区] 就绪 (模板构建间隔=%d ticks, 深度=%d)\n",
           b->build_interval_ticks, b->max_build_depth);
    return b;
}

void broca_destroy(Broca* b) {
    free(b);
}

int broca_tick(Broca* b) {
    if (!b || !b->master) return 0;
    b->tick_count++;

    int new_templates = 0;

    /* 按间隔触发模板自动构建 */
    if (b->tick_count % b->build_interval_ticks == 0) {
        new_templates = template_auto_build(b->master, 50, b->max_build_depth);
        b->total_builds++;
        b->total_new_templates += (new_templates > 0 ? new_templates : 0);
    }

    /* 每5个间隔触发一次模板衰减 */
    if (b->tick_count % (b->build_interval_ticks * 5) == 0) {
        template_decay_inactive_links(b->master, b->decay_threshold, b->decay_rate);
    }

    return new_templates;
}

int broca_build_templates(MasterTopology* topology, int count, int max_depth) {
    return template_auto_build(topology, count, max_depth);
}

void broca_decay_templates(MasterTopology* topology, int threshold, float decay) {
    template_decay_inactive_links(topology, threshold, decay);
}

int broca_template_count(MasterTopology* topology) {
    SubTopology* tpl = master_get_sub_topology_by_type(topology, TOPO_TEMPLATE);
    return (tpl && tpl->net) ? tpl->net->node_count : 0;
}
