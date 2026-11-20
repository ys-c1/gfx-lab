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
    return 0;
}
