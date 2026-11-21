#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    enum { N = 64 };
    CullBox b[N];
    for (int i = 0; i < N; ++i) {
        b[i].x = (i % 8) * 20; b[i].y = (i / 8) * 20;
        b[i].w = 18; b[i].h = 18; b[i].layer = i % 3;
    }
    CullView v = {0, 0, 100, 100};
    CullResult r;
    if (cull_run(b, N, v, &r) != 0) return 1;
    printf("legacy one-shot cull: visible %d of %d\n", r.count, N);
    free(r.ids);

    /* animated pan: two-phase viewport culling over 6 frames */
    CullView v0 = {0, 0, 100, 100}, v1 = {40, 20, 100, 100};
    CullAnim a;
    CullStepInfo f;
    if (cull_anim_init(&a, b, N, v0, v1, 0, &f) != 0) return 1;
    printf("animated pan (incremental): candidates=%d\n", f.candidates);
    printf("frame     t visible added removed rebuilt\n");
    printf("%5d %.3f %7d %5d %7d %7d\n",
           f.frame, f.t, f.visible_count, f.added, f.removed, f.rebuilt);
    for (int k = 1; k <= 5; ++k) {
        if (cull_anim_step(&a, k / 5.0, &f) != 0) return 1;
        printf("%5d %.3f %7d %5d %7d %7d\n",
               f.frame, f.t, f.visible_count, f.added, f.removed, f.rebuilt);
    }
    if (cull_anim_export_csv(&a, "build/demo_traj.csv") == 0)
        printf("trajectory exported to build/demo_traj.csv\n");
    cull_anim_free(&a);
    return 0;
}
