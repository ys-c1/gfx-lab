#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int ids_eq(const CullStepInfo *s, const int *want, int n) {
    if (s->visible_count != n) return 0;
    for (int i = 0; i < n; ++i) if (s->visible[i] != want[i]) return 0;
    return 1;
}

int main(void) {
    /* ---------- legacy behavior (must stay unchanged) ---------- */
    CullBox b[4] = {{0,0,10,10,0},{100,100,10,10,1},{50,50,20,20,0},{-5,-5,10,10,2}};
    CullView v = {0, 0, 60, 60};
    CullResult r;
    CK(cull_run(b, 4, v, &r) == 0);
    CK(r.count == 3);
    CK(r.ids[0] == 0 && r.ids[1] == 2 && r.ids[2] == 3);
    CK(cull_count_within(b, 4, v) == 3);
    CullView bad = {0, 0, 0, 10};
    CK(cull_run(b, 4, bad, &r) == -2);
    free(r.ids);
    /* legacy one-shot cull still accepts the empty set */
    CK(cull_run(b, 0, v, &r) == 0 && r.count == 0);
    free(r.ids);

    /* ---------- animated culling: hand-computed small scene ----------
       see verify/handcheck.csv for the full hand derivation.
       b0 (0..8,0..8)    b1 (0..8,15..23)   b2 (27..35,0..8)
       b3 (35..43,0..8)  b4 (80..88,80..88) b5 (-25..-17,...)
       v0 = (0,0,30,30), v1 = (40,0,30,30), t = 0,.25,.5,.75,1          */
    CullBox s[6] = {
        {0,0,8,8,0},{0,15,8,8,0},{27,0,8,8,1},{35,0,8,8,2},
        {80,80,8,8,0},{-25,-25,8,8,1}};
    CullView v0 = {0,0,30,30}, v1 = {40,0,30,30};
    const double ts[5] = {0.0, 0.25, 0.5, 0.75, 1.0};

    /* phase 1: view interpolation */
    CullView m = cull_view_lerp(v0, v1, 0.25);
    CK(m.x == 10.0 && m.y == 0.0 && m.w == 30.0 && m.h == 30.0);

    CullAnim inc;
    CullStepInfo f;
    CK(cull_anim_init(&inc, s, 6, v0, v1, 0, &f) == 0);
    CK(f.frame == 0 && f.t == 0.0 && f.candidates == 4);
    CK(f.rebuilt == 0);                 /* incremental: no full rebuild */
    int w0[3] = {0,1,2};
    CK(ids_eq(&f, w0, 3) && f.added == 3 && f.removed == 0 && f.unchanged == 0);

    const int *want[5] = {
        (int[]){0,1,2}, (int[]){2,3}, (int[]){2,3},
        (int[]){2,3}, (int[]){3}
    };
    const int wn[5] = {3,2,2,2,1};
    const int wadd[5] = {3,1,0,0,0};
    const int wrem[5] = {0,2,0,0,1};
    const int wunc[5] = {0,1,2,2,1};
    CK(ids_eq(&f, want[0], wn[0]));
    for (int k = 1; k < 5; ++k) {
        CK(cull_anim_step(&inc, ts[k], &f) == 0);
        CK(f.frame == k);
        CK(f.candidates == 4 && f.rebuilt == 0);
        CK(ids_eq(&f, want[k], wn[k]));
        CK(f.added == wadd[k] && f.removed == wrem[k] && f.unchanged == wunc[k]);
    }
    CK(inc.rebuilds == 0);             /* zero full-scan rebuilds */
    cull_anim_free(&inc);

    /* reverse jump on a fresh anim: walk to t=1 then back to t=0;
       both added and removed must be reported against the previous frame */
    CK(cull_anim_init(&inc, s, 6, v0, v1, 0, &f) == 0);
    CK(cull_anim_step(&inc, 1.0, &f) == 0 && ids_eq(&f, want[4], wn[4]));
    CK(cull_anim_step(&inc, 0.0, &f) == 0);
    /* {3} -> {0,1,2}: disjoint sets, all three enter, id 3 leaves */
    CK(ids_eq(&f, w0, 3) && f.added == 3 && f.removed == 1 && f.unchanged == 0);
    cull_anim_free(&inc);
    /* ---------- degenerate mode: every frame rebuilt over all n ---------- */
    CullAnim full;
    CullStepInfo g;
    CK(cull_anim_init(&full, s, 6, v0, v1, CULL_ANIM_FULL_REBUILD, &g) == 0);
    CK(g.rebuilt == 1 && full.rebuilds == 1);
    for (int k = 0; k < 5; ++k) {
        if (k > 0) CK(cull_anim_step(&full, ts[k], &g) == 0);
        CK(g.rebuilt == 1);
        CK(ids_eq(&g, want[k], wn[k]));
        /* must equal legacy cull_run on the same interpolated view */
        CullView vv = cull_view_lerp(v0, v1, ts[k]);
        CullResult rr;
        CK(cull_run(s, 6, vv, &rr) == 0);
        CK(rr.count == g.visible_count);
        int same = 1;
        for (int j = 0; j < rr.count; ++j) if (rr.ids[j] != g.visible[j]) same = 0;
        CK(same);
        free(rr.ids);
    }
    CK(full.rebuilds == 5);
    cull_anim_free(&full);

    /* ---------- error handling ---------- */
    CK(cull_anim_init(NULL, s, 6, v0, v1, 0, NULL) == CULL_E_ARG);
    CullAnim aa;
    CK(cull_anim_init(&aa, NULL, 6, v0, v1, 0, NULL) == CULL_E_ARG);
    CK(cull_anim_init(&aa, s, -1, v0, v1, 0, NULL) == CULL_E_ARG);
    int rc = cull_anim_init(&aa, s, 0, v0, v1, 0, NULL);
    CK(rc == CULL_E_EMPTY);
    printf("empty-input error: %s\n", cull_strerror(rc));
    CK(strstr(cull_strerror(CULL_E_EMPTY), "n == 0") != NULL);
    CK(cull_anim_init(&aa, s, 6, bad, v1, 0, NULL) == CULL_E_VIEW);
    CullView bad2 = {0,0,10,-1};
    CK(cull_anim_init(&aa, s, 6, v0, bad2, 0, NULL) == CULL_E_VIEW);
    CK(cull_anim_step(&aa, 0.5, NULL) == CULL_E_STATE);
    CK(cull_anim_init(&aa, s, 6, v0, v1, 0, NULL) == 0);
    CK(cull_anim_step(&aa, -0.1, NULL) == CULL_E_T);
    CK(cull_anim_step(&aa, 1.0001, NULL) == CULL_E_T);
    CK(cull_anim_export_csv(&aa, "/nonexistent-dir/x.csv") == CULL_E_IO);
    cull_anim_free(&aa);
    CK(cull_strerror(CULL_E_T) != NULL && cull_strerror(42) != NULL);

    /* ---------- trajectory export ---------- */
    CK(cull_anim_init(&aa, s, 6, v0, v1, 0, NULL) == 0);
    for (int k = 1; k < 5; ++k) CK(cull_anim_step(&aa, ts[k], NULL) == 0);
    CK(cull_anim_export_csv(&aa, "build/traj_test.csv") == 0);
    CK(cull_anim_export_boxes(s, 6, "build/boxes_test.csv") == 0);
    FILE *fp = fopen("build/traj_test.csv", "r");
    CK(fp != NULL);
    char line[256]; int rows = 0;
    CK(fgets(line, sizeof line, fp) != NULL);
    CK(strncmp(line, "frame,t,", 8) == 0);
    while (fgets(line, sizeof line, fp)) ++rows;
    fclose(fp);
    CK(rows == 5);
    cull_anim_free(&aa);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
