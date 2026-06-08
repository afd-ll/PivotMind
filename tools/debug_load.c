#include <stdio.h>
#include <stdlib.h>
#include "multi_topology.h"

int main() {
    MasterTopology* m = master_topology_create(11);
    master_add_sub_topology(m, TOPO_VOCABULARY, "", 30000, 10);
    master_add_sub_topology(m, TOPO_SEMANTIC, "", 12000, 9);
    master_add_sub_topology(m, TOPO_EMOTION, "", 4000, 8);
    master_add_sub_topology(m, TOPO_SYNTAX, "", 1000, 7);
    master_add_sub_topology(m, TOPO_CONTEXT, "", 1000, 6);
    master_add_sub_topology(m, TOPO_DOMAIN, "", 1000, 5);
    master_add_sub_topology(m, TOPO_PRAGMA, "", 1000, 4);
    master_add_sub_topology(m, TOPO_CULTURE, "", 1000, 3);
    master_add_sub_topology(m, TOPO_CONCEPT, "", 12000, 9);
    master_add_sub_topology(m, TOPO_MASTER, "", 100, 0);
    master_add_sub_topology(m, TOPO_TEMPLATE, "", 4000, 8);

    printf("Topologies created: %d\n", m->sub_topo_count);
    for (int t = 0; t < m->sub_topo_count; t++) {
        printf("  t=%d type=%d name=%s\n", t, (int)m->sub_topologies[t]->type, m->sub_topologies[t]->name);
    }

    int loaded = master_load_state(m, "pivotmind_state.dat");
    printf("\nLoaded: %d nodes, %d links\n", loaded, m->cross_link_count);

    for (int t = 0; t < m->sub_topo_count; t++) {
        SubTopology* s = m->sub_topologies[t];
        if (s && s->net) {
            printf("  Topo %d (type=%d): %d nodes\n", t, (int)s->type, s->net->node_count);
        }
    }

    master_topology_destroy(m);
    return 0;
}
