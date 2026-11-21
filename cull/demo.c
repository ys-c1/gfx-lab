#include "cull.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    enum { N = 64 };
    CullBox b[N];
    double tols[N];
    for (int i = 0; i < N; ++i) {
        b[i].x = (i % 8) * 20;
        b[i].y = (i / 8) * 20;
        b[i].w = 18;
        b[i].h = 18;
        b[i].layer = i % 3;
        b[i].vx = 0.0;
        b[i].vy = 0.0;
        b[i].tol = 0.0;
    }
    /* column 6 sits just outside the right edge (x=120, edge at 100)
       and rushes toward the viewport: tol = 1500/60 = 25 px bridges it */
    for (int i = 6; i < 32; i += 8) b[i].vx = -1500.0;
    /* column 7 is farther away and slower: tol = 300/60 = 5 px,
       not enough to cross the 40 px gap, so it stays culled */
    for (int i = 7; i < 32; i += 8) b[i].vx = -300.0;

    CullView v = {0, 0, 100, 100};
    CullParams params = {1.0 / 60.0, 0.0};
    CullResult legacy, adapt;
    if (cull_run(b, N, v, &legacy) != 0) return 1;
    if (cull_apply_tolerance(b, N, params, tols, 0) != 0) return 1;
    if (cull_run_adaptive(b, N, v, params, tols, &adapt, 0) != 0) return 1;
    printf("legacy   visible %d of %d\n", legacy.count, N);
    printf("adaptive visible %d of %d (dt=1/60, margin=0)\n", adapt.count, N);
    printf("edge tolerances: id 6 tol=%.3f px (rescued), id 7 tol=%.3f px (still culled)\n",
           tols[6], tols[7]);
    for (int i = 0; i < adapt.count; ++i) printf("  %d\n", adapt.ids[i]);
    if (cull_export("build/candidates_demo.txt", &adapt) != 0)
        printf("export failed\n");
    else
        printf("candidate list exported to build/candidates_demo.txt\n");
    free(legacy.ids);
    free(adapt.ids);
    return 0;
}
