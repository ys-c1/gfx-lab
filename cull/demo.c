#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    enum { N = 64 };
    CullBox b[N];
    for (int i = 0; i < N; ++i) { b[i].x = (i % 8) * 20; b[i].y = (i / 8) * 20; b[i].w = 18; b[i].h = 18; b[i].layer = i % 3; }
    CullView v = {0, 0, 100, 100};
    CullResult r;
    if (cull_run(b, N, v, &r) != 0) return 1;
    printf("visible %d of %d\n", r.count, N);
    for (int i = 0; i < r.count; ++i) printf("  %d\n", r.ids[i]);
    free(r.ids);

    printf("--- layered (two stages) ---\n");
    CullLayered L; CullError st;
    if (cull_run_layered(b, N, v, 1, &L, &st) != CULL_OK) {
        printf("layered failed code %d\n", st.code);
        return 1;
    }
    printf("visible %d, groups %d, limit %d\n", L.count, L.group_count, L.layer_limit);
    for (int k = 0; k < L.group_count; ++k)
        printf("  layer %d: candidates %d, visible %d, ids [%d, %d)\n",
               L.groups[k].layer, L.groups[k].candidates, L.groups[k].count,
               L.groups[k].first, L.groups[k].first + L.groups[k].count);
    cull_layered_free(&L);
    return 0;
}
