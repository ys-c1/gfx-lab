#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    enum { N = 64 };
    CullBox b[N];
    for (int i = 0; i < N; ++i) {
        b[i].x = (i % 8) * 20; b[i].y = (i / 8) * 20;
        b[i].w = 18; b[i].h = 18;
        b[i].layer = i % 3; b[i].seq = N - 1 - i; /* 逆插入序, 展示稳定重排 */
    }
    CullView v = {0, 0, 100, 100};

    CullResult r;
    if (cull_run(b, N, v, &r) != 0) return 1;
    printf("visible %d of %d (insertion order)\n", r.count, N);
    for (int i = 0; i < r.count; ++i) printf("  %d\n", r.ids[i]);
    free(r.ids);

    CullLayered L; CullStatus st;
    if (cull_run_layered(b, N, v, 1, &L, &st) != CULL_OK) return 1;
    printf("layered visible %d, buckets %d, comparisons %llu\n",
           L.count, L.bucket_count, L.comparisons);
    for (int j = 0; j < L.bucket_count; ++j)
        printf("  layer %d: visible %d, clipped %d\n",
               L.buckets[j].layer, L.buckets[j].visible, L.buckets[j].clipped);
    for (int i = 0; i < L.count; ++i) printf("  %d\n", L.ids[i]);
    cull_layered_free(&L);
    return 0;
}
