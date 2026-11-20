#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    enum { N = 64 };
    CullBox b[N];
    CullWeight we[N];
    for (int i = 0; i < N; ++i) {
        b[i].x = (i % 8) * 20; b[i].y = (i / 8) * 20; b[i].w = 18; b[i].h = 18; b[i].layer = i % 3;
        we[i].id = i; we[i].weight = (double)((i * 37) % 10);
    }
    CullWeightTable t = {we, N};
    CullView v = {0, 0, 100, 100};
    CullResult r;
    if (cull_run(b, N, v, &r) != 0) return 1;
    printf("visible %d of %d\n", r.count, N);

    /* 两段式：先按 (layer, weight) 稳定排序，再按序输出 */
    int *order = (int*)malloc(sizeof(int) * (size_t)r.count);
    if (!order) { free(r.ids); return 1; }
    long cmp = 0; int bad = -1;
    if (cull_order(b, r.ids, r.count, &t, 1, order, &cmp, &bad) != 0) {
        printf("cull_order error, object %d\n", bad);
        free(order); free(r.ids);
        return 1;
    }
    printf("draw order (sorted, %ld comparisons):\n", cmp);
    for (int i = 0; i < r.count; ++i)
        printf("  id=%d layer=%d weight=%g\n", order[i], b[order[i]].layer, cull_weight_of(&t, order[i]));

    if (cull_weights_export(&t, "weights.txt") == 0)
        printf("weight table exported to weights.txt\n");

    free(order);
    free(r.ids);
    return 0;
}
