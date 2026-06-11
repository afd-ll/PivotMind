#include "broca.h"
#include "template_builder.h"

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
