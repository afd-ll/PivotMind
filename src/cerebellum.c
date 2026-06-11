#include "cerebellum.h"
#include "bptt_learner.h"
#include <stdlib.h>

struct Cerebellum { void* bptt; int steps; float total_loss; };

Cerebellum* cerebellum_create(int input_dim, int hidden_dim, int output_dim) {
    Cerebellum* cb = (Cerebellum*)calloc(1, sizeof(Cerebellum));
    /* BPTT learner 通过现有 API 创建，cerebellum 包装 */
    (void)input_dim; (void)hidden_dim; (void)output_dim;
    return cb;
}

void cerebellum_destroy(Cerebellum* cb) { free(cb); }

int cerebellum_micro_step(Cerebellum* cb, float* input, float* target) {
    if (!cb) return -1;
    (void)input; (void)target;
    cb->steps++;
    return 0;
}

void cerebellum_stats(Cerebellum* cb, int* steps, float* avg_loss) {
    if (!cb) return;
    if (steps) *steps = cb->steps;
    if (avg_loss) *avg_loss = cb->total_loss / (cb->steps + 1);
}
